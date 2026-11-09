#!/usr/bin/env python3
"""Compile actual updater methods with fake HTTP/time, outside the shared build.

Run with a C++17 compiler on PATH (an MSVC developer prompt works on Windows).
This exercises polling/retry transitions, not the full client or HTTP transport.
"""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

try:
	from cryptography.hazmat.primitives import serialization
	from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
except ImportError:
	Ed25519PrivateKey = None


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/game/client/components/tclient/tclient.cpp"

HARNESS = r'''
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <engine/external/json-parser/json.h>
#include <engine/external/ed25519/ed25519.h>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while(false)
constexpr int IO_MAX_PATH_LENGTH = 1024;
enum class EHttpState { RUNNING, DONE, ERROR, ABORTED };
enum class IPRESOLVE { V4 };
enum class HTTPLOG { NONE };
const json_value *json_object_get(const json_value *pValue, const char *pName) { return pValue && pValue->type == json_object ? &(*pValue)[pName] : &json_value_none; }
unsigned json_array_length(const json_value *pValue) { return pValue && pValue->type == json_array ? pValue->u.array.length : 0; }
const json_value *json_array_get(const json_value *pValue, unsigned Index) { return Index < json_array_length(pValue) ? pValue->u.array.values[Index] : &json_value_none; }
struct CTimeout { int a, b, c, d; };
struct SHA256_DIGEST { std::string m_Value; };
int sha256_from_str(SHA256_DIGEST *pDigest, const char *pText) {
	if(!pText || std::strlen(pText) != 64) return -1;
	for(const char *p = pText; *p; ++p) if(!std::strchr("0123456789abcdefABCDEF", *p)) return -1;
	pDigest->m_Value = pText; return 0;
}
const char *TCLocalize(const char *pText) { return pText; }
template<typename... T> void log_info(T...) {}
template<typename... T> void log_warn(T...) {}
template<typename... T> void str_format(char *pOut, size_t Size, const char *pFormat, T... Args) { std::snprintf(pOut, Size, pFormat, Args...); }
void str_format_v(char *pOut, size_t Size, const char *pFormat, va_list Args) { std::vsnprintf(pOut, Size, pFormat, Args); }
void str_copy(char *pOut, const char *pText, size_t Size) { std::snprintf(pOut, Size, "%s", pText); }
template<size_t N> void str_copy(char (&aOut)[N], const char *pText) { str_copy(aOut, pText, N); }
int str_comp(const char *pA, const char *pB) { return std::strcmp(pA, pB); }
int str_length(const char *pText) { return (int)std::strlen(pText); }
int64_t time_timestamp() { return 1234; }
// Intentionally independent of the local release version.
constexpr const char *TCLIENT_APP_VERSION = "2.3.4";
int g_JsonCalls = 0;
std::map<std::string, int64_t> g_Files;
struct IStorage {
	enum { TYPE_SAVE, TYPE_ALL };
	void GetCompletePath(int, const char *pPath, char *pOut, size_t Size) { str_copy(pOut, pPath, Size); }
};
using IOHANDLE = int64_t *;
constexpr int IOFLAG_READ = 1;
IOHANDLE io_open(const char *pPath, int) { auto It = g_Files.find(pPath); return It == g_Files.end() ? nullptr : &It->second; }
int64_t io_length(IOHANDLE File) { return *File; }
void io_close(IOHANDLE) {}
struct IHttpRequest {
	EHttpState m_State = EHttpState::RUNNING;
	std::string m_Url, m_Path, m_ExpectedSha;
	std::map<std::string, std::string> m_Headers;
	std::vector<unsigned char> m_Data;
	bool m_Aborted = false;
	bool Done() const { return m_State != EHttpState::RUNNING; }
	EHttpState State() const { return m_State; }
	void Abort() { m_Aborted = true; m_State = EHttpState::ABORTED; }
	void Timeout(CTimeout) {}
	void IpResolve(IPRESOLVE) {}
	void MaxResponseSize(size_t) {}
	void FailOnErrorStatus(bool) {}
	void HeaderString(const char *pName, const char *pValue) { m_Headers[pName] = pValue; }
	void LogProgress(HTTPLOG) {}
	void ExpectSha256(const SHA256_DIGEST &Digest) { m_ExpectedSha = Digest.m_Value; }
	void ValidateBeforeOverwrite(bool) {}
	void AllowResume(bool) {}
	void Result(unsigned char **ppData, size_t *pSize) { *ppData = m_Data.data(); *pSize = m_Data.size(); }
	json_value *ResultJson() { ++g_JsonCalls; return json_parse((const char *)m_Data.data(), m_Data.size()); }
};
std::shared_ptr<IHttpRequest> HttpGet(const char *pUrl) {
	auto pTask = std::make_shared<IHttpRequest>(); pTask->m_Url = pUrl; return pTask;
}
std::shared_ptr<IHttpRequest> HttpGetFile(const char *pUrl, IStorage *, const char *pPath, int) {
	auto pTask = HttpGet(pUrl); pTask->m_Path = pPath; return pTask;
}
struct CClock { float m_Now = 10.0f; float LocalTime() const { return m_Now; } };
struct CHttp { std::vector<std::shared_ptr<IHttpRequest>> m_vRequests; void Run(std::shared_ptr<IHttpRequest> pTask) { m_vRequests.push_back(pTask); } };
'''

SCENARIOS = r'''
constexpr int NumSources = (int)std::size(TCLIENT_UPDATE_MANIFEST_SOURCES);
int NumAssetSources() {
	int Count = 0;
	for(auto pBase : TCLIENT_UPDATE_SOURCE_SIBLING_BASES) if(pBase) ++Count;
	return Count;
}
void Done(const std::shared_ptr<IHttpRequest> &pTask, const std::vector<unsigned char> &Data) { CHECK(pTask); pTask->m_Data = Data; pTask->m_State = EHttpState::DONE; }
int ApiSource() {
	for(int Source = 0; Source < NumSources; ++Source) if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[Source])) return Source;
	CHECK(false); return -1;
}
std::string ApiAssetUrl(bool Helper) {
	return std::string("https://api.github.com/repos/") + TCLIENT_UPDATE_GH_REPO_PATH + "/releases/assets/" + (Helper ? "202" : "101");
}
std::string ApiReply(bool Zip = true, bool Helper = true) {
	std::string Reply = "{\"assets\":[";
	if(Zip) Reply += std::string("{\"name\":\"") + TCLIENT_UPDATE_ZIP_ASSET + "\",\"url\":\"" + ApiAssetUrl(false) + "\"}";
	if(Zip && Helper) Reply += ",";
	if(Helper) Reply += std::string("{\"name\":\"") + TCLIENT_UPDATE_HELPER_ASSET + "\",\"url\":\"" + ApiAssetUrl(true) + "\"}";
	return Reply + "]}";
}
void ResolveApi(CTClient &C) {
	if(!C.m_pUpdateApiResolveTask) return;
	CHECK(C.m_pUpdateApiResolveTask->m_Url == std::string("https://api.github.com/repos/") + TCLIENT_UPDATE_GH_REPO_PATH + "/releases/tags/v2.3.5");
	const std::string Sha = C.m_UpdateRelease.m_aSha256, HelperSha = C.m_UpdateRelease.m_aHelperSha256;
	const std::string Reply = ApiReply();
	Done(C.m_pUpdateApiResolveTask, std::vector<unsigned char>(Reply.begin(), Reply.end()));
	C.PollUpdateTasks();
	CHECK(!C.m_pUpdateApiResolveTask && C.m_UpdateRelease.m_Valid);
	CHECK(Sha == C.m_UpdateRelease.m_aSha256 && HelperSha == C.m_UpdateRelease.m_aHelperSha256);
}
void Pair(CTClient &C, const std::vector<unsigned char> &Manifest = s_NewManifest, const std::vector<unsigned char> &Sig = s_NewSignature) {
	CHECK(C.m_pUpdateManifestTask);
	const std::string Url = C.m_pUpdateManifestTask->m_Url;
	Done(C.m_pUpdateManifestTask, Manifest);
	C.PollUpdateTasks();
	CHECK(C.m_pUpdateManifestSigTask && C.m_pUpdateManifestSigTask->m_Url == Url + ".sig");
	Done(C.m_pUpdateManifestSigTask, Sig);
	C.PollUpdateTasks();
}
void Clean(const CTClient &C) {
	CHECK(!C.m_pUpdateManifestTask && !C.m_pUpdateManifestSigTask && !C.m_pUpdateDownloadTask && !C.m_pUpdateHelperTask && !C.m_pUpdateApiResolveTask);
	CHECK(C.m_UpdateRetryAt < 0);
}
void WaitRetry(CTClient &C) {
	const auto Count = C.m_Http.m_vRequests.size();
	CHECK(C.m_UpdateRetryAt > C.m_Clock.m_Now);
	C.m_Clock.m_Now = C.m_UpdateRetryAt - 0.01f;
	C.PollUpdateTasks(); CHECK(C.m_Http.m_vRequests.size() == Count);
	C.m_Clock.m_Now += 0.01f;
	C.PollUpdateTasks(); CHECK(C.m_Http.m_vRequests.size() == Count + 1);
	ResolveApi(C);
}
void DownloadDone(CTClient &C) {
	CHECK(C.m_pUpdateDownloadTask);
	g_Files[TCLIENT_UPDATE_ZIP_PATH] = C.m_UpdateRelease.m_Size;
	C.m_pUpdateDownloadTask->m_State = EHttpState::DONE; C.PollUpdateTasks();
	ResolveApi(C);
	CHECK(C.m_pUpdateHelperTask && !C.m_pUpdateDownloadTask);
}
void CheckTag(const std::shared_ptr<IHttpRequest> &pTask, const char *pAsset, const char *pSha) {
	CHECK(pTask && pTask->m_ExpectedSha == pSha);
	if(pTask->m_Url.find("https://api.github.com/") == 0) {
		CHECK(pTask->m_Url == ApiAssetUrl(str_comp(pAsset, TCLIENT_UPDATE_HELPER_ASSET) == 0));
		const auto Accept = pTask->m_Headers.find("Accept");
		CHECK(Accept != pTask->m_Headers.end() && Accept->second == "application/octet-stream");
	} else {
		CHECK(pTask->m_Url.find("/releases/download/v2.3.5/") != std::string::npos);
		CHECK(pTask->m_Url.find("/latest/") == std::string::npos);
		CHECK(pTask->m_Url.substr(pTask->m_Url.size() - std::strlen(pAsset)) == pAsset);
		CHECK(pTask->m_Headers.find("Accept") == pTask->m_Headers.end());
	}
}
void BeginApi(CTClient &C, bool Helper) {
	C.UpdateCheck(true); Pair(C);
	C.m_UpdateDownloadSource = ApiSource();
	if(Helper) C.StartUpdateHelperTask(); else C.StartDownloadTask();
	CHECK(C.m_pUpdateApiResolveTask && C.m_ApiResolvePending == Helper);
	CHECK(!C.m_pUpdateDownloadTask && !C.m_pUpdateHelperTask);
}
int main() {
	int Scenarios = 0;
	CHECK(NumAssetSources() == TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT);
	CHECK(std::size(TCLIENT_UPDATE_SOURCE_SIBLING_BASES) == NumSources + 1);
	CHECK(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[NumSources] == nullptr);
	CHECK(ApiSource() == TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT - 1);
	// Every source obtains its own manifest/signature pair, including raw.
	for(int Source = 0; Source < NumSources; ++Source) {
		++Scenarios;
		CTClient C; C.UpdateCheck(true); C.UpdateCheck(true);
		CHECK(C.m_Http.m_vRequests.size() == 1);
		for(int Previous = 0; Previous < Source; ++Previous) {
			C.m_pUpdateManifestTask->m_State = EHttpState::ERROR; C.PollUpdateTasks();
		}
		CHECK(C.m_UpdateManifestSource == Source);
		Pair(C);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::AVAILABLE && C.m_UpdateRelease.m_Valid);
		Clean(C); C.UpdateStartDownload(); ResolveApi(C);
		CheckTag(C.m_pUpdateDownloadTask, TCLIENT_UPDATE_ZIP_ASSET, C.m_UpdateRelease.m_aSha256);
	}
	// Signature HTTP failures discard the pair and move to the next source.
	{
		++Scenarios;
		CTClient C; C.UpdateCheck(true);
		for(int Source = 0; Source < NumSources; ++Source) {
			auto pManifest = C.m_pUpdateManifestTask; Done(pManifest, s_NewManifest); C.PollUpdateTasks();
			CHECK(C.m_pUpdateManifestSigTask->m_Url == pManifest->m_Url + ".sig");
			C.m_pUpdateManifestSigTask->m_State = EHttpState::ERROR; C.PollUpdateTasks();
			CHECK(!C.m_pUpdateManifestSigTask);
			if(Source + 1 < NumSources) CHECK(C.m_pUpdateManifestTask && C.m_pUpdateManifestTask != pManifest);
		}
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED && !C.m_UpdateRelease.m_Valid);
		Clean(C); CHECK(C.m_Http.m_vRequests.size() == 2 * NumSources);
		C.UpdateCheck(true); Pair(C); CHECK(C.m_UpdateState == CTClient::EUpdateState::AVAILABLE);
	}
	// A real Ed25519 signature for different bytes never reaches JSON parsing.
	{
		++Scenarios;
		CTClient C; C.UpdateCheck(true); const int Calls = g_JsonCalls;
		Pair(C, s_NewManifest, s_EqualSignature);
		CHECK(C.m_UpdateManifestSource == 1 && !C.m_UpdateRelease.m_Valid && g_JsonCalls == Calls);
		Pair(C); CHECK(C.m_UpdateState == CTClient::EUpdateState::AVAILABLE);
	}
	for(int Size : {0, 1, 63, 65}) {
		++Scenarios;
		CTClient C; C.UpdateCheck(true); const int Calls = g_JsonCalls;
		Pair(C, s_NewManifest, std::vector<unsigned char>(Size, 0));
		CHECK(C.m_UpdateManifestSource == 1 && !C.m_UpdateRelease.m_Valid && g_JsonCalls == Calls);
	}
	{
		++Scenarios;
		CTClient C; C.UpdateCheck(true);
		for(int Source = 0; Source < NumSources; ++Source) Pair(C, s_NewManifest, s_EqualSignature);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED); Clean(C);
	}
	// Older mirrors cannot block a newer source or prove that this is latest.
	{
		++Scenarios;
		CTClient C; C.UpdateCheck(true); Pair(C, s_OldManifest, s_OldSignature);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::CHECKING && C.m_UpdateManifestSource == 1);
		Pair(C); CHECK(C.m_UpdateState == CTClient::EUpdateState::AVAILABLE);
	}
	for(bool Equal : {false, true}) {
		++Scenarios;
		CTClient C; C.UpdateCheck(true);
		for(int Source = 0; Source < NumSources - 1; ++Source) { C.m_pUpdateManifestTask->m_State = EHttpState::ERROR; C.PollUpdateTasks(); }
		Pair(C, Equal ? s_EqualManifest : s_OldManifest, Equal ? s_EqualSignature : s_OldSignature);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED); Clean(C);
	}
	{
		++Scenarios;
		CTClient C; C.UpdateCheck(true); Pair(C, s_EqualManifest, s_EqualSignature);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::CHECKING);
		for(int Source = 1; Source < NumSources; ++Source) { C.m_pUpdateManifestTask->m_State = EHttpState::ERROR; C.PollUpdateTasks(); }
		CHECK(C.m_UpdateState == CTClient::EUpdateState::UP_TO_DATE && !C.m_UpdateRelease.m_Valid); Clean(C);
	}
	// Signed malformed versions cannot create an injectable or truncated tag.
	for(const auto &PairData : s_InvalidVersions) {
		++Scenarios;
		CTClient C; C.UpdateCheck(true);
		for(int Source = 0; Source < NumSources; ++Source) Pair(C, PairData.first, PairData.second);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED); Clean(C);
	}
	// ZIP and helper each cycle four mirrors once, always preserving signed metadata.
	{
		++Scenarios;
		CTClient C; C.UpdateCheck(true); Pair(C); C.UpdateStartDownload(); ResolveApi(C);
		std::vector<std::string> ZipUrls, HelperUrls;
		for(int Attempt = 0; Attempt < NumAssetSources(); ++Attempt) {
			CheckTag(C.m_pUpdateDownloadTask, TCLIENT_UPDATE_ZIP_ASSET, C.m_UpdateRelease.m_aSha256);
			ZipUrls.push_back(C.m_pUpdateDownloadTask->m_Url);
			if(Attempt + 1 == NumAssetSources()) break;
			C.m_pUpdateDownloadTask->m_State = EHttpState::ERROR; C.PollUpdateTasks(); WaitRetry(C);
		}
		DownloadDone(C);
		for(int Attempt = 0; Attempt < NumAssetSources(); ++Attempt) {
			CheckTag(C.m_pUpdateHelperTask, TCLIENT_UPDATE_HELPER_ASSET, C.m_UpdateRelease.m_aHelperSha256);
			HelperUrls.push_back(C.m_pUpdateHelperTask->m_Url);
			if(Attempt + 1 == NumAssetSources()) break;
			C.m_pUpdateHelperTask->m_State = EHttpState::ERROR; C.PollUpdateTasks(); WaitRetry(C);
			CHECK(!C.m_pUpdateDownloadTask);
		}
		for(size_t i = 0; i < ZipUrls.size(); ++i) for(size_t j = i + 1; j < ZipUrls.size(); ++j) CHECK(ZipUrls[i] != ZipUrls[j] && HelperUrls[i] != HelperUrls[j]);
		g_Files[TCLIENT_UPDATE_BOOTSTRAP_PATH] = C.m_UpdateRelease.m_HelperSize;
		C.m_pUpdateHelperTask->m_State = EHttpState::DONE; C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::DOWNLOADED && C.BootstrapHelperPending); Clean(C);
	}
	// Both transfer and final size failures rotate/exhaust with no stale retries.
	for(bool Helper : {false, true}) for(bool WrongSize : {false, true}) {
		++Scenarios;
		CTClient C; C.UpdateCheck(true); Pair(C); C.UpdateStartDownload(); ResolveApi(C);
		if(Helper) DownloadDone(C);
		for(int Attempt = 0; Attempt < NumAssetSources(); ++Attempt) {
			auto pTask = Helper ? C.m_pUpdateHelperTask : C.m_pUpdateDownloadTask; CHECK(pTask);
			g_Files[pTask->m_Path] = 7;
			pTask->m_State = WrongSize ? EHttpState::DONE : EHttpState::ERROR; C.PollUpdateTasks();
			if(Attempt + 1 < NumAssetSources()) WaitRetry(C);
		}
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED && !C.m_UpdateRelease.m_Valid && !C.BootstrapHelperPending); Clean(C);
		const auto Count = C.m_Http.m_vRequests.size(); C.m_Clock.m_Now += 100; C.PollUpdateTasks(); CHECK(C.m_Http.m_vRequests.size() == Count);
	}
	// Cancellation during any request, or a delayed retry, is terminal.
	for(int Stage = 0; Stage < 5; ++Stage) {
		++Scenarios;
		CTClient C; C.UpdateCheck(true);
		if(Stage == 1) { Done(C.m_pUpdateManifestTask, s_NewManifest); C.PollUpdateTasks(); }
		if(Stage >= 2) { Pair(C); C.UpdateStartDownload(); ResolveApi(C); }
		if(Stage == 3) DownloadDone(C);
		if(Stage == 4) { C.m_pUpdateDownloadTask->m_State = EHttpState::ERROR; C.PollUpdateTasks(); C.ResetUpdateTasks(); }
		else { auto pTask = Stage == 0 ? C.m_pUpdateManifestTask : Stage == 1 ? C.m_pUpdateManifestSigTask : Stage == 2 ? C.m_pUpdateDownloadTask : C.m_pUpdateHelperTask; pTask->m_State = EHttpState::ABORTED; C.PollUpdateTasks(); }
		Clean(C); CHECK(!C.BootstrapHelperPending);
		const auto Count = C.m_Http.m_vRequests.size(); C.m_Clock.m_Now += 100; C.PollUpdateTasks(); CHECK(C.m_Http.m_vRequests.size() == Count);
	}
	{
		++Scenarios;
		CTClient C;
		auto M = C.m_pUpdateManifestTask = HttpGet("manifest"), S = C.m_pUpdateManifestSigTask = HttpGet("signature"), Z = C.m_pUpdateDownloadTask = HttpGet("zip"), H = C.m_pUpdateHelperTask = HttpGet("helper"), A = C.m_pUpdateApiResolveTask = HttpGet("api");
		C.m_UpdateRetryAt = 20; C.BootstrapHelperPending = true; C.UpdateFail("failed");
		CHECK(M->m_Aborted && S->m_Aborted && Z->m_Aborted && H->m_Aborted && A->m_Aborted && !C.BootstrapHelperPending); Clean(C);
	}
	// API resolution is bounded, tag-pinned, and does not duplicate a pending request.
	for(bool Helper : {false, true}) {
		++Scenarios;
		CTClient C; BeginApi(C, Helper);
		const auto Count = C.m_Http.m_vRequests.size();
		for(int i = 0; i < 20; ++i) { C.PollUpdateTasks(); C.UpdateStartDownload(); }
		CHECK(C.m_Http.m_vRequests.size() == Count && C.m_pUpdateApiResolveTask);
		ResolveApi(C);
		CheckTag(Helper ? C.m_pUpdateHelperTask : C.m_pUpdateDownloadTask, Helper ? TCLIENT_UPDATE_HELPER_ASSET : TCLIENT_UPDATE_ZIP_ASSET, Helper ? C.m_UpdateRelease.m_aHelperSha256 : C.m_UpdateRelease.m_aSha256);
		if(!Helper) {
			const auto ResolvedCount = C.m_Http.m_vRequests.size();
			DownloadDone(C);
			CHECK(C.m_Http.m_vRequests.size() == ResolvedCount + 1);
			CheckTag(C.m_pUpdateHelperTask, TCLIENT_UPDATE_HELPER_ASSET, C.m_UpdateRelease.m_aHelperSha256);
		}
		g_Files[TCLIENT_UPDATE_BOOTSTRAP_PATH] = C.m_UpdateRelease.m_HelperSize;
		C.m_pUpdateHelperTask->m_State = EHttpState::DONE; C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::DOWNLOADED && C.BootstrapHelperPending); Clean(C);
	}
	// HTTP errors, invalid JSON and either missing asset all rotate the same budget.
	for(bool Helper : {false, true}) for(int Failure = 0; Failure < 5; ++Failure) {
		++Scenarios;
		CTClient C; BeginApi(C, Helper);
		if(Failure == 0) C.m_pUpdateApiResolveTask->m_State = EHttpState::ERROR;
		else {
			const std::string Reply = Failure == 1 ? "{" : Failure == 2 ? "[]" : ApiReply(Failure != 3, Failure != 4);
			Done(C.m_pUpdateApiResolveTask, std::vector<unsigned char>(Reply.begin(), Reply.end()));
		}
		C.PollUpdateTasks();
		CHECK(!C.m_pUpdateApiResolveTask && C.m_UpdateRelease.m_Valid);
		CHECK(C.m_UpdateDownloadSource == (ApiSource() + 1) % NumAssetSources());
		CHECK(C.m_UpdateDownloadAttempt == 1 && C.m_UpdateRetryHelper == Helper);
		CHECK(!C.m_pUpdateDownloadTask && !C.m_pUpdateHelperTask);
		WaitRetry(C);
		CheckTag(Helper ? C.m_pUpdateHelperTask : C.m_pUpdateDownloadTask, Helper ? TCLIENT_UPDATE_HELPER_ASSET : TCLIENT_UPDATE_ZIP_ASSET, Helper ? C.m_UpdateRelease.m_aHelperSha256 : C.m_UpdateRelease.m_aSha256);
	}
	for(bool Helper : {false, true}) {
		++Scenarios;
		CTClient C; BeginApi(C, Helper);
		C.m_pUpdateApiResolveTask->m_State = EHttpState::ABORTED; C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED && !C.m_UpdateRelease.m_Valid); Clean(C);
		const auto Count = C.m_Http.m_vRequests.size(); C.m_Clock.m_Now += 100; C.PollUpdateTasks();
		CHECK(C.m_Http.m_vRequests.size() == Count && !C.BootstrapHelperPending);
	}
	for(bool Helper : {false, true}) {
		++Scenarios;
		CTClient C; BeginApi(C, Helper);
		C.m_UpdateDownloadAttempt = NumAssetSources() - 1;
		C.m_pUpdateApiResolveTask->m_State = EHttpState::ERROR; C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED && !C.m_UpdateRelease.m_Valid); Clean(C);
	}
	{
		++Scenarios;
		CTClient C; BeginApi(C, false);
		auto Old = C.m_pUpdateApiResolveTask; C.UpdateFail("cancelled"); Clean(C);
		C.UpdateCheck(true); const auto Count = C.m_Http.m_vRequests.size();
		const std::string Reply = ApiReply(); Done(Old, std::vector<unsigned char>(Reply.begin(), Reply.end()));
		C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::CHECKING && !C.m_UpdateRelease.m_Valid);
		CHECK(C.m_pUpdateManifestTask && !C.m_pUpdateApiResolveTask && C.m_Http.m_vRequests.size() == Count);
	}
	CHECK(Scenarios == NumSources + 41);
	std::printf("%d updater scenarios passed: %d paired sources, real Ed25519 rejection, fixed tags, ZIP/helper mirror budgets, API assets and headers, resolve failures, stale completion and cancellation\n", Scenarios, NumSources);
}
'''


class UpdateRetryTest(unittest.TestCase):
	def test_production_updater_methods(self):
		compiler = os.environ.get("CXX") or next((path for name in ("cl", "c++", "g++", "clang++") if (path := shutil.which(name))), None)
		if not compiler:
			self.skipTest("requires a standalone C++17 compiler on PATH")
		if Ed25519PrivateKey is None:
			self.skipTest("requires cryptography to sign isolated fixture manifests")
		source = SOURCE.read_text(encoding="utf-8-sig")
		header = (SOURCE.parent / "tclient.h").read_text(encoding="utf-8-sig")
		constants = re.search(r"^static constexpr const char \*TCLIENT_UPDATE_LATEST_URL.*?^static constexpr int64_t TCLIENT_UPDATE_CHECK_INTERVAL[^\n]*", source, re.MULTILINE | re.DOTALL)[0]
		helpers = []
		for name in ["ParseAppVersion", "CompareAppVersion", "JsonObj", "JsonStr", "JsonGetInt", "IsHexSha256"]:
			match = re.search(r"^static [^\n]*\b" + name + r"\([^\n]*\)\s*\n\{.*?^\}", source, re.MULTILINE | re.DOTALL)
			self.assertIsNotNone(match, "could not extract production helper " + name)
			helpers.append(match[0])
		helpers.insert(0, re.search(r"^static constexpr int APP_VERSION_MAX_SEGMENTS[^\n]*", source, re.MULTILINE)[0])
		public = header[header.index("\tenum class EUpdateState"):header.index("\nprivate:")]
		fields = header[header.index("\t// ----- Client-side auto-update internals -----"):header.index("\t// 游戏内中键玩家菜单")]
		fields = re.sub(r" \[\[gnu::format\(printf, 2, 3\)\]\]", "", fields)
		client = "struct CTClient {\n" + public + fields + r'''
	CClock m_Clock;
	CHttp m_Http;
	IStorage m_Storage;
	CClock *Client() { return &m_Clock; }
	CHttp *Http() { return &m_Http; }
	IStorage *Storage() { return &m_Storage; }
	void UpdateCheck(bool Force);
	void UpdateStartDownload();
};
'''
		methods = []
		for name in ["SetUpdateStatus", "UpdateFail", "ResetUpdateTasks", "VerifyUpdateManifestSignature", "UpdateCheck", "FetchUpdateManifest", "FinishUpdateManifest", "UpdateStartDownload", "StartDownloadTask", "FinishUpdateDownload", "FinishUpdateBootstrap", "PollUpdateTasks", "NextUpdateManifestSource", "RetryUpdateDownload", "StartUpdateHelperTask", "StartApiResolveTask", "FinishApiResolve"]:
			match = re.search(r"^(?:void|bool) CTClient::" + name + r"\([^\n]*\)\s*\n\{.*?^\}", source, re.MULTILINE | re.DOTALL)
			if not match and name in {"NextUpdateManifestSource", "RetryUpdateDownload", "StartUpdateHelperTask"}:
				continue  # Allows the regression to run red before the production fix.
			self.assertIsNotNone(match, "could not extract production method " + name)
			methods.append(match[0])
		key = Ed25519PrivateKey.from_private_bytes(bytes(range(32)))
		def byte_vector(name, data):
			return "const std::vector<unsigned char> " + name + " = {" + ",".join(map(str, data)) + "};\n"
		fixtures = "const unsigned char s_aUpdateEd25519PublicKey[] = {" + ",".join(map(str, key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw))) + "};\n"
		base = {"product": "TClient", "channel": "stable", "platform": "win64", "asset": "TClient-update-win64.zip", "size": 12345, "sha256": "a" * 64, "helper": {"size": 987, "sha256": "b" * 64}}
		for name, version in [("New", "2.3.5"), ("Equal", "2.3.4"), ("Old", "2.3.3"), ("BadPath", "2.3.5/../../latest"), ("TooLong", ".".join(["123456"] * 8))]:
			data = json.dumps({**base, "version": version}, separators=(",", ":")).encode("utf-8")
			fixtures += byte_vector("s_" + name + "Manifest", data) + byte_vector("s_" + name + "Signature", key.sign(data))
		fixtures += "const std::vector<std::pair<std::vector<unsigned char>, std::vector<unsigned char>>> s_InvalidVersions = {{s_BadPathManifest, s_BadPathSignature}, {s_TooLongManifest, s_TooLongSignature}};\n"
		with tempfile.TemporaryDirectory(prefix="tclient-updater-test-") as temporary:
			root = Path(temporary)
			cpp = root / "retry.cpp"
			exe = root / ("retry.exe" if os.name == "nt" else "retry")
			cpp.write_text(HARNESS + constants + "\n".join(helpers) + fixtures + client + "\n".join(methods) + SCENARIOS, encoding="utf-8")
			external = ROOT / "src/engine/external"
			dependencies = [external / "json-parser/json.c"] + [external / "ed25519" / (name + ".c") for name in ["verify", "fe", "ge", "sc", "sha512"]]
			if Path(compiler).name.lower() in ("cl", "cl.exe"):
				command = [compiler, "/nologo", "/utf-8", "/std:c++17", "/EHsc", "/I" + str(ROOT / "src"), str(cpp), *map(str, dependencies), "/Fe:" + str(exe)]
			else:
				command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "src"), str(cpp), *map(str, dependencies), "-o", str(exe)]
			# Localized compiler diagnostics must not hide the actual exit status.
			compiled = subprocess.run(command, cwd=root, capture_output=True, text=True, errors="replace")
			self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
			result = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True, errors="replace")
			self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
			print(result.stdout.strip())


if __name__ == "__main__":
	unittest.main()
