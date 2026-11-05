#!/usr/bin/env python3
"""Compile actual updater methods with fake HTTP/time, outside the shared build.

Run with a C++17 compiler on PATH (an MSVC developer prompt works on Windows).
This exercises polling/retry transitions, not the full client or HTTP transport.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/game/client/components/tclient/tclient.cpp"

HARNESS = r'''
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while(false)
enum class EHttpState { RUNNING, DONE, ERROR, ABORTED };
enum class IPRESOLVE { V4 };
struct CTimeout { int a, b, c, d; };
struct IHttpRequest {
	EHttpState m_State = EHttpState::RUNNING;
	std::string m_Url;
	bool m_Aborted = false;
	bool Done() const { return m_State != EHttpState::RUNNING; }
	EHttpState State() const { return m_State; }
	void Abort() { m_Aborted = true; m_State = EHttpState::ABORTED; }
	void Timeout(CTimeout) {}
	void IpResolve(IPRESOLVE) {}
	void MaxResponseSize(size_t) {}
	void FailOnErrorStatus(bool) {}
};
std::shared_ptr<IHttpRequest> HttpGet(const char *pUrl) {
	auto pTask = std::make_shared<IHttpRequest>(); pTask->m_Url = pUrl; return pTask;
}
const char *TCLocalize(const char *pText) { return pText; }
template<typename... T> void log_info(T...) {}
template<typename... T> void log_warn(T...) {}
template<typename... T> void str_format(char *pOut, size_t Size, const char *pFormat, T... Args) { std::snprintf(pOut, Size, pFormat, Args...); }
void str_format_v(char *pOut, size_t Size, const char *pFormat, va_list Args) { std::vsnprintf(pOut, Size, pFormat, Args); }
template<size_t N> void str_copy(char (&aOut)[N], const char *pText) { std::snprintf(aOut, N, "%s", pText); }
int64_t time_timestamp() { return 1234; }
const char *TCLIENT_APP_VERSION = "test";
const char *TCLIENT_UPDATE_RAW_BASE = "https://raw/";
const char *TCLIENT_UPDATE_GH_BASE = "https://github/";
const char *TCLIENT_UPDATE_PROXY_BASE = "https://proxy/";
const char *TCLIENT_UPDATE_MANIFEST_SIG_ASSET = "TClient-update-win64.json.sig";
const char *TCLIENT_UPDATE_MANIFEST_SOURCES[] = {"proxy manifest", "github manifest", "raw manifest"};
bool BootstrapHelperPending = false;
struct CClock { float m_Now = 10.0f; float LocalTime() const { return m_Now; } };
struct CHttp { std::vector<std::shared_ptr<IHttpRequest>> m_vRequests; void Run(std::shared_ptr<IHttpRequest> pTask) { m_vRequests.push_back(pTask); } };
struct CTClient {
	enum class EUpdateState { IDLE, CHECKING, AVAILABLE, DOWNLOADING, DOWNLOADED, FAILED };
	struct SUpdateRelease { bool m_Valid = false; };
	EUpdateState m_UpdateState = EUpdateState::IDLE;
	SUpdateRelease m_UpdateRelease;
	std::shared_ptr<IHttpRequest> m_pUpdateManifestTask, m_pUpdateManifestSigTask, m_pUpdateDownloadTask, m_pUpdateHelperTask;
	std::shared_ptr<IHttpRequest> m_pVerifiedManifest;
	int m_UpdateManifestSource = 0, m_UpdateDownloadAttempt = 0;
	float m_UpdateRetryAt = -1.0f;
	int64_t m_LastUpdateCheck = 0;
	char m_aUpdateStatus[256]{}, m_aUpdateReleaseTag[16]{}, m_aUpdateManifestUrl[16]{}, m_aUpdateManifestSigUrl[16]{}, m_aUpdateHelperUrl[16]{}, m_aUpdateBootstrapPath[16]{};
	CClock m_Clock;
	CHttp m_Http;
	int m_VerifyCalls = 0, m_DownloadStarts = 0;
	bool m_SignatureValid = true;
	CClock *Client() { return &m_Clock; }
	CHttp *Http() { return &m_Http; }
	void SetUpdateStatus(const char *pStatus);
	void UpdateFail(const char *pFormat, ...);
	void ResetUpdateTasks();
	void UpdateCheck(bool Force);
	void FetchUpdateManifest();
	void PollUpdateTasks();
	void FinishUpdateManifest(std::shared_ptr<IHttpRequest> pSig, std::shared_ptr<IHttpRequest> pManifest) {
		CHECK(pSig && pSig->State() == EHttpState::DONE);
		CHECK(pManifest && pManifest->State() == EHttpState::DONE);
		++m_VerifyCalls; m_pVerifiedManifest = pManifest;
		if(!m_SignatureValid) { UpdateFail("invalid signature"); return; }
		m_UpdateRelease.m_Valid = true; m_UpdateState = EUpdateState::AVAILABLE;
	}
	void StartDownloadTask() { ++m_DownloadStarts; ResetUpdateTasks(); m_UpdateState = EUpdateState::DOWNLOADING; }
	void FinishUpdateDownload() { UpdateFail("download failure"); }
	void FinishUpdateBootstrap() { m_UpdateState = EUpdateState::DOWNLOADED; }
};
// Actual production methods are inserted here by the test runner.
'''

SCENARIOS = r'''
int main() {
	// Failed signature HTTP requests must keep the exact completed manifest.
	for(int Source = 0; Source < 3; ++Source) {
		CTClient C;
		C.UpdateCheck(true);
		C.m_UpdateManifestSource = Source;
		auto pManifest = C.m_pUpdateManifestTask;
		pManifest->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		auto pFirstSig = C.m_pUpdateManifestSigTask;
		CHECK(pFirstSig);
		const std::string ExpectedPrefix = Source == 0 ? "https://proxy/" : Source == 1 ? "https://github/" : "https://raw/";
		CHECK(pFirstSig->m_Url.find(ExpectedPrefix) == 0);
		pFirstSig->m_State = EHttpState::ERROR;
		C.PollUpdateTasks();
		CHECK(C.m_pUpdateManifestTask == pManifest);
		CHECK(!C.m_pUpdateManifestSigTask && C.m_UpdateDownloadAttempt == 1);
		CHECK(C.m_UpdateRetryAt == 12.0f);
		C.m_Clock.m_Now = 11.99f;
		C.PollUpdateTasks();
		CHECK(C.m_Http.m_vRequests.size() == 2);
		C.m_Clock.m_Now = 12.0f;
		C.PollUpdateTasks();
		CHECK(C.m_pUpdateManifestSigTask && C.m_pUpdateManifestSigTask != pFirstSig);
		CHECK(C.m_pUpdateManifestSigTask->m_Url == pFirstSig->m_Url);
		C.m_pUpdateManifestSigTask->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::AVAILABLE);
		CHECK(C.m_pVerifiedManifest == pManifest && C.m_VerifyCalls == 1);
		CHECK(!C.m_pUpdateManifestTask && !C.m_pUpdateManifestSigTask);
		C.m_Clock.m_Now += 100.0f;
		C.PollUpdateTasks();
		CHECK(C.m_VerifyCalls == 1 && C.m_Http.m_vRequests.size() == 3 && C.m_DownloadStarts == 0);
	}
	// Exhaustion terminates CHECKING; a manual check gets a fresh retry budget.
	{
		CTClient C;
		C.UpdateCheck(true);
		C.UpdateCheck(true);
		CHECK(C.m_Http.m_vRequests.size() == 1);
		C.m_pUpdateManifestTask->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		for(int Attempt = 0; Attempt <= 3; ++Attempt) {
			CHECK(C.m_pUpdateManifestSigTask);
			C.m_pUpdateManifestSigTask->m_State = EHttpState::ERROR;
			C.PollUpdateTasks();
			if(Attempt < 3) {
				CHECK(C.m_UpdateState == CTClient::EUpdateState::CHECKING);
				CHECK(C.m_UpdateDownloadAttempt == Attempt + 1 && C.m_pUpdateManifestTask);
				C.m_Clock.m_Now += 1.0f;
				C.PollUpdateTasks();
				CHECK(!C.m_pUpdateManifestSigTask);
				C.m_Clock.m_Now += 1.0f;
				C.PollUpdateTasks();
			}
		}
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED && C.m_UpdateRetryAt < 0);
		CHECK(!C.m_pUpdateManifestTask && !C.m_pUpdateManifestSigTask);
		C.UpdateCheck(true);
		CHECK(C.m_UpdateState == CTClient::EUpdateState::CHECKING && C.m_UpdateDownloadAttempt == 0);
		CHECK(C.m_UpdateManifestSource == 0 && C.m_UpdateRetryAt < 0);
		C.m_pUpdateManifestTask->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		C.m_pUpdateManifestSigTask->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::AVAILABLE && C.m_VerifyCalls == 1);
	}
	// A verification rejection still consumes tasks and fails without retrying.
	{
		CTClient C;
		C.UpdateCheck(true);
		C.m_pUpdateManifestTask->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		C.m_SignatureValid = false;
		C.m_pUpdateManifestSigTask->m_State = EHttpState::DONE;
		C.PollUpdateTasks();
		C.PollUpdateTasks();
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED && C.m_VerifyCalls == 1);
		CHECK(!C.m_pUpdateManifestTask && !C.m_pUpdateManifestSigTask && C.m_Http.m_vRequests.size() == 2);
	}
	// Manifest-source fallback and exhausted-source recovery remain functional.
	{
		CTClient C;
		C.UpdateCheck(true);
		for(int Source = 0; Source < 3; ++Source) {
			CHECK(C.m_UpdateManifestSource == Source && C.m_pUpdateManifestTask);
			C.m_pUpdateManifestTask->m_State = EHttpState::ERROR;
			C.PollUpdateTasks();
		}
		CHECK(C.m_UpdateState == CTClient::EUpdateState::FAILED);
		C.UpdateCheck(true);
		CHECK(C.m_UpdateManifestSource == 0 && C.m_pUpdateManifestTask);
	}
	// Failure/task reset cancels pending retries and aborts every request type.
	{
		CTClient C;
		auto pManifest = C.m_pUpdateManifestTask = HttpGet("manifest");
		auto pSig = C.m_pUpdateManifestSigTask = HttpGet("signature");
		auto pDownload = C.m_pUpdateDownloadTask = HttpGet("download");
		auto pHelper = C.m_pUpdateHelperTask = HttpGet("helper");
		C.m_UpdateRetryAt = C.m_Clock.m_Now + 2;
		C.UpdateFail("failed");
		CHECK(pManifest->m_Aborted && pSig->m_Aborted && pDownload->m_Aborted && pHelper->m_Aborted);
		CHECK(C.m_UpdateRetryAt < 0);
		C.m_Clock.m_Now += 100;
		C.PollUpdateTasks();
		CHECK(C.m_DownloadStarts == 0 && C.m_Http.m_vRequests.empty());
	}
	std::puts("Updater retry, delay, three sources, exhaustion/recheck, rejection and reset scenarios passed");
}
'''


class UpdateRetryTest(unittest.TestCase):
	def test_production_updater_methods(self):
		compiler = os.environ.get("CXX") or next((path for name in ("cl", "c++", "g++", "clang++") if (path := shutil.which(name))), None)
		if not compiler:
			self.skipTest("requires a standalone C++17 compiler on PATH")
		source = SOURCE.read_text(encoding="utf-8")
		methods = []
		for name in ["SetUpdateStatus", "UpdateFail", "ResetUpdateTasks", "UpdateCheck", "FetchUpdateManifest", "PollUpdateTasks"]:
			match = re.search(r"^void CTClient::" + name + r"\([^\n]*\)\s*\n\{.*?^\}", source, re.MULTILINE | re.DOTALL)
			self.assertIsNotNone(match, "could not extract production method " + name)
			methods.append(match[0])
		with tempfile.TemporaryDirectory(prefix="tclient-updater-test-") as temporary:
			root = Path(temporary)
			cpp = root / "retry.cpp"
			exe = root / ("retry.exe" if os.name == "nt" else "retry")
			cpp.write_text(HARNESS + "\n".join(methods) + SCENARIOS, encoding="utf-8")
			if Path(compiler).name.lower() in ("cl", "cl.exe"):
				command = [compiler, "/nologo", "/std:c++17", "/EHsc", str(cpp), "/Fe:" + str(exe)]
			else:
				command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)]
			compiled = subprocess.run(command, cwd=root, capture_output=True, text=True)
			self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
			result = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True)
			self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
			print(result.stdout.strip())


if __name__ == "__main__":
	unittest.main()
