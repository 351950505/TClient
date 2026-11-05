// Standalone offline regression tests. The runner compiles the verbatim lyrics
// section of island_music.cpp, the production lyric parser and JSON parser.
// Only HTTP transport, the media clock and cover publication are faked.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <engine/shared/json.h>
#include <game/client/components/tclient/lyric_parser.h>

using TcLyric::ParseSyncedLyrics;
using TcLyric::ParseYrcLyrics;
using TcLyric::SLyricLine;

static void Check(bool Condition, const char *pMessage)
{
	if(!Condition)
		throw std::runtime_error(pMessage);
}

static int64_t time_get() { return 1000; }
static int64_t time_freq() { return 1000; }
static int str_comp(const char *pLeft, const char *pRight) { return strcmp(pLeft, pRight); }
static void str_format(char *pBuffer, size_t Size, const char *pFormat, ...)
{
	va_list Args;
	va_start(Args, pFormat);
	vsnprintf(pBuffer, Size, pFormat, Args);
	va_end(Args);
}

template<size_t Size>
static void EscapeUrl(char (&aBuffer)[Size], const char *pText)
{
	// Fixtures use URL-safe metadata; URL encoding is outside this regression.
	snprintf(aBuffer, Size, "%s", pText);
}

enum class EHttpState { ERROR = -1, QUEUED, RUNNING, DONE, ABORTED };
enum class HTTPLOG { FAILURE };
struct CTimeout { long m_ConnectTimeoutMs, m_TimeoutMs, m_LowSpeedLimit, m_LowSpeedTime; };

class IHttpRequest
{
public:
	std::string m_Url;
	std::string m_Body;
	EHttpState m_State = EHttpState::QUEUED;
	int m_Status = 0;
	int m_Runs = 0;
	bool m_Aborted = false;
	CTimeout m_Timeout{};
	explicit IHttpRequest(const char *pUrl) : m_Url(pUrl) {}
	void Timeout(CTimeout Timeout) { m_Timeout = Timeout; }
	void LogProgress(HTTPLOG) {}
	void FailOnErrorStatus(bool) {}
	void HeaderString(const char *, const char *) {}
	void Abort() { m_Aborted = true; }
	bool Done() const { return m_State != EHttpState::QUEUED && m_State != EHttpState::RUNNING; }
	EHttpState State() const { return m_State; }
	int StatusCode() const { return m_Status; }
	json_value *ResultJson() const { return json_parse(m_Body.c_str(), m_Body.size()); }
	void Result(unsigned char **ppResult, size_t *pLength)
	{
		*ppResult = reinterpret_cast<unsigned char *>(m_Body.data());
		*pLength = m_Body.size();
	}
	void Complete(const std::string &Body, EHttpState State = EHttpState::DONE, int Status = 200)
	{
		Check(m_Runs == 1, "request was never submitted with IHttp::Run");
		m_Body = Body;
		m_State = State;
		m_Status = Status;
	}
};

static std::vector<std::shared_ptr<IHttpRequest>> s_vCreated;
static std::shared_ptr<IHttpRequest> HttpGet(const char *pUrl)
{
	auto pRequest = std::make_shared<IHttpRequest>(pUrl);
	s_vCreated.push_back(pRequest);
	return pRequest;
}

class IHttp
{
public:
	void Run(const std::shared_ptr<IHttpRequest> &pRequest)
	{
		Check(pRequest->m_Runs++ == 0, "request submitted more than once");
		pRequest->m_State = EHttpState::RUNNING;
	}
};

// Generated from the production file on every run, without rewriting its logic.
#include "island_music_test_impl.h"

using CMusic = CIslandMusicTest;
using EState = CMusic::ELyricsState;
static constexpr const char *NET_SEARCH = "/api/search/get/web";
static constexpr const char *NET_LYRIC = "/api/song/lyric";
static constexpr const char *QQ_SEARCH = "/soso/";
static constexpr const char *QQ_LYRIC = "/lyric/fcgi-bin/";
static constexpr const char *LRCLIB = "lrclib.net";
static constexpr const char *NET_FOUND = R"({"result":{"songs":[{"id":7,"name":"TrackA","duration":120000,"artists":[{"name":"Artist"}],"album":{"picUrl":"skip-cover"}}]}})";
static constexpr const char *QQ_FOUND = R"({"code":0,"data":{"song":{"list":[{"songmid":"midA"}]}}})";
static constexpr const char *QQ_LINES = R"({"code":0,"lyric":"[00:01.00]QQ lyric\n[00:03.00]next"})";
static constexpr const char *LRC_LINES = R"([{"syncedLyrics":"[00:01.00]lrclib lyric\n[00:03.00]next"}])";

static CMusic::STrack Track(const char *pTitle = "TrackA", const char *pService = "netease-title")
{
	CMusic::STrack Result;
	Result.m_Valid = true;
	Result.m_Title = pTitle;
	Result.m_Artist = "Artist";
	Result.m_ServiceId = pService;
	Result.m_DurationMs = 120000;
	return Result;
}

static size_t Count(const char *pUrlPart)
{
	return std::count_if(s_vCreated.begin(), s_vCreated.end(), [pUrlPart](const auto &pRequest) { return pRequest->m_Url.find(pUrlPart) != std::string::npos; });
}

static std::shared_ptr<IHttpRequest> Last(const char *pUrlPart)
{
	for(auto It = s_vCreated.rbegin(); It != s_vCreated.rend(); ++It)
		if((*It)->m_Url.find(pUrlPart) != std::string::npos)
			return *It;
	throw std::runtime_error(std::string("expected request missing: ") + pUrlPart);
}

static void Tick(CMusic &Music, const CMusic::STrack &Current, int Frames = 3)
{
	for(int i = 0; i < Frames; ++i)
		Music.UpdateLyrics(Current);
}

static void NeteaseMiss(CMusic &Music, const CMusic::STrack &Current)
{
	Tick(Music, Current);
	Last(NET_SEARCH)->Complete(NET_FOUND);
	Tick(Music, Current);
	Last(NET_LYRIC)->Complete(R"({"lrc":{"lyric":""}})");
	Tick(Music, Current);
}

static void TestQQSubmittedAndOwned()
{
	IHttp Http;
	CMusic Music;
	Music.m_pHttp = &Http;
	const auto Current = Track();
	NeteaseMiss(Music, Current);
	Check(Last(QQ_SEARCH)->m_Runs == 1, "QQ search is stuck QUEUED");
	Last(QQ_SEARCH)->Complete(QQ_FOUND);
	Tick(Music, Current);
	Check(Last(QQ_LYRIC)->m_Runs == 1, "QQ lyric is stuck QUEUED or lost its track key");
	Last(QQ_LYRIC)->Complete(QQ_LINES);
	Tick(Music, Current, 100);
	Check(Music.m_LyricsState == EState::READY, "QQ success never reaches READY");
	Check(Music.m_vLines[0].m_Text == "QQ lyric", "QQ lyrics stored under the wrong track");
	Check(Count(QQ_SEARCH) == 1 && Count(QQ_LYRIC) == 1 && Count(LRCLIB) == 0, "successful QQ chain created extra requests");
}

struct SFailure { const char *m_pName; const char *m_pBody; EHttpState m_State; int m_Status; };
static const SFailure s_aFailures[] = {
	{"network", "", EHttpState::ERROR, 0},
	{"aborted", "", EHttpState::ABORTED, 0},
	{"http", "{}", EHttpState::DONE, 503},
	{"invalid-json", "{", EHttpState::DONE, 200},
	{"wrong-root", "[]", EHttpState::DONE, 200},
	{"empty", "{}", EHttpState::DONE, 200},
};

static void TestFailures(const char *pStage)
{
	for(const auto &Failure : s_aFailures)
	{
		s_vCreated.clear();
		IHttp Http;
		CMusic Music;
		Music.m_pHttp = &Http;
		const auto Current = Track();
		Tick(Music, Current);
		if(pStage != NET_SEARCH)
		{
			Last(NET_SEARCH)->Complete(NET_FOUND);
			Tick(Music, Current);
		}
		if(pStage == QQ_SEARCH || pStage == QQ_LYRIC)
		{
			Last(NET_LYRIC)->Complete("{}");
			Tick(Music, Current);
		}
		if(pStage == QQ_LYRIC)
		{
			Last(QQ_SEARCH)->Complete(QQ_FOUND);
			Tick(Music, Current);
		}
		Last(pStage)->Complete(Failure.m_pBody, Failure.m_State, Failure.m_Status);
		Tick(Music, Current);
		if(pStage == NET_SEARCH || pStage == NET_LYRIC)
		{
			Last(QQ_SEARCH)->Complete("{}");
			Tick(Music, Current);
		}
		Last(LRCLIB)->Complete(LRC_LINES);
		Tick(Music, Current, 100);
		Check(Music.m_LyricsState == EState::READY && Music.m_vLines[0].m_Text == "lrclib lyric", Failure.m_pName);
		Check(Count(NET_SEARCH) == 1 && Count(QQ_SEARCH) == 1 && Count(LRCLIB) == 1, "failure retried a provider every frame");
	}
}

static void TestSuccessfulNetease()
{
	const char *apResponses[] = {
		R"({"yrc":{"lyric":"[1000,1000](1000,400,0)Y(1400,600,0)RC"},"lrc":{"lyric":"[00:01.00]fallback"}})",
		R"({"yrc":{"lyric":"bad yrc"},"lrc":{"lyric":"[00:01.00]fallback"}})",
	};
	for(int i = 0; i < 2; ++i)
	{
		s_vCreated.clear();
		IHttp Http;
		CMusic Music;
		Music.m_pHttp = &Http;
		const auto Current = Track();
		Tick(Music, Current);
		Last(NET_SEARCH)->Complete(NET_FOUND);
		Tick(Music, Current);
		Last(NET_LYRIC)->Complete(apResponses[i]);
		Tick(Music, Current, 100);
		Check(Music.m_LyricsState == EState::READY, "valid NetEase lyrics regressed");
		Check(Music.m_vLines[0].m_Text == (i == 0 ? "YRC" : "fallback"), "YRC priority / LRC degradation regressed");
		Check(!Music.m_vLines[0].m_vWords.empty(), "karaoke word timings lost");
		if(i == 0)
			Check(Music.m_vLines[0].m_vWords[1].m_StartMs == 1400, "real YRC timings were synthesized");
		Check(Count(QQ_SEARCH) == 0 && Count(LRCLIB) == 0, "valid NetEase lyrics triggered fallback");
	}
}

int main()
{
	int Failed = 0;
	auto Run = [&](const char *pName, auto Test) {
		s_vCreated.clear();
		try { Test(); printf("PASS %s\n", pName); }
		catch(const std::exception &Error) { ++Failed; printf("FAIL %s: %s\n", pName, Error.what()); }
	};
	Run("QQ submission and stable track identity", TestQQSubmittedAndOwned);
	Run("NetEase search failure matrix", [] { TestFailures(NET_SEARCH); });
	Run("NetEase lyric failure matrix", [] { TestFailures(NET_LYRIC); });
	Run("QQ search failure matrix", [] { TestFailures(QQ_SEARCH); });
	Run("QQ lyric failure matrix", [] { TestFailures(QQ_LYRIC); });
	Run("NetEase YRC priority and LRC degradation", TestSuccessfulNetease);
	printf("%d failing test groups\n", Failed);
	return Failed ? 1 : 0;
}
