// Standalone offline regression tests. The runner extracts the current production
// lyrics state machine and IJob implementation verbatim, and links real parsers.
// Only transports, scheduling, the media clock and cover publication are faked.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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

static thread_local bool s_WorkerThread = false;

// Gates establish actual happens-before edges; no sleeps or timing-dependent races.
class CGate
{
	std::mutex m_Mutex;
	std::condition_variable m_Condition;
	bool m_Entered = false;
	bool m_Released = false;

public:
	void Enter()
	{
		std::unique_lock<std::mutex> Lock(m_Mutex);
		m_Entered = true;
		m_Condition.notify_all();
		m_Condition.wait(Lock, [&] { return m_Released; });
	}
	void Wait()
	{
		std::unique_lock<std::mutex> Lock(m_Mutex);
		Check(m_Condition.wait_for(Lock, std::chrono::seconds(10), [&] { return m_Entered; }), "worker never reached its gate");
	}
	void Release()
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_Released = true;
		m_Condition.notify_all();
	}
};

// The production IJob declaration and methods, not a parallel model of Done/Abort.
#include "island_music_test_job.h"

// IJob grants friendship to CJobPool. This deterministic scheduling boundary lets
// tests choose queue order and hold a real worker before STATE_DONE publication.
class CJobPool
{
	std::vector<std::shared_ptr<IJob>> m_vQueue;
	std::thread m_Worker;
	std::exception_ptr m_Error;
	std::shared_ptr<CGate> m_pReadGate;
	std::shared_ptr<CGate> m_pPublishGate;

public:
	size_t m_Added = 0;

	~CJobPool()
	{
		for(const auto &pJob : m_vQueue)
			pJob->Abort();
		if(m_pReadGate)
			m_pReadGate->Release();
		if(m_pPublishGate)
			m_pPublishGate->Release();
		if(m_Worker.joinable())
			m_Worker.join();
	}

	void AddJob(std::shared_ptr<IJob> pJob)
	{
		Check(!s_WorkerThread, "a parser enqueued work from the worker thread");
		Check(pJob->State() == IJob::STATE_QUEUED, "invalid job submitted");
		Check(std::find(m_vQueue.begin(), m_vQueue.end(), pJob) == m_vQueue.end(), "same job submitted twice");
		++m_Added;
		m_vQueue.push_back(std::move(pJob));
	}

	void Start(const std::shared_ptr<IJob> &pJob, std::shared_ptr<CGate> pReadGate = {}, std::shared_ptr<CGate> pPublishGate = {})
	{
		Check(!m_Worker.joinable(), "previous worker must be joined first");
		const auto It = std::find(m_vQueue.begin(), m_vQueue.end(), pJob);
		Check(It != m_vQueue.end(), "job was not queued");
		auto pOwned = *It;
		m_vQueue.erase(It);
		m_pReadGate = std::move(pReadGate);
		m_pPublishGate = std::move(pPublishGate);
		m_Error = nullptr;
		m_Worker = std::thread([this, pOwned] {
			s_WorkerThread = true;
			try
			{
				IJob::EJobState Expected = IJob::STATE_QUEUED;
				if(pOwned->m_State.compare_exchange_strong(Expected, IJob::STATE_RUNNING))
				{
					pOwned->Run();
					if(m_pPublishGate)
						m_pPublishGate->Enter();
					Expected = IJob::STATE_RUNNING;
					if(!pOwned->m_State.compare_exchange_strong(Expected, IJob::STATE_DONE))
						Check(Expected == IJob::STATE_ABORTED, "unexpected worker completion state");
				}
				else
					Check(Expected == IJob::STATE_ABORTED, "unexpected queued job state");
			}
			catch(...)
			{
				m_Error = std::current_exception();
				pOwned->Abort();
			}
			s_WorkerThread = false;
		});
	}

	void Join()
	{
		if(m_Worker.joinable())
			m_Worker.join();
		m_pReadGate.reset();
		m_pPublishGate.reset();
		if(m_Error)
			std::rethrow_exception(std::exchange(m_Error, nullptr));
	}

	void Run(const std::shared_ptr<IJob> &pJob)
	{
		Start(pJob);
		Join();
	}

	void RunAll()
	{
		while(!m_vQueue.empty())
			Run(m_vQueue.front());
	}
};

using IEngine = CJobPool;

static int64_t s_TestTime = 1000;
static int64_t time_get() { return s_TestTime; }
static int64_t time_freq() { return 1000; }
static void AdvanceTime(int Seconds) { s_TestTime += time_freq() * Seconds; }
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
	void BeginRead() const
	{
		Check(s_WorkerThread, "HTTP response was parsed/copied on a render tick");
		Check(m_State == EHttpState::DONE, "response read before successful HTTP completion");
		if(m_pReadGate)
			m_pReadGate->Enter();
	}

public:
	std::string m_Url;
	std::string m_Body;
	EHttpState m_State = EHttpState::QUEUED;
	int m_Status = 0;
	int m_Runs = 0;
	bool m_Aborted = false;
	size_t m_MaxResponseSize = 0;
	size_t m_ResultLengthOverride = 0;
	mutable std::atomic<unsigned> m_JsonReads{0};
	mutable std::atomic<unsigned> m_RawReads{0};
	std::shared_ptr<CGate> m_pReadGate;
	CTimeout m_Timeout{};
	explicit IHttpRequest(const char *pUrl) : m_Url(pUrl) {}
	void Timeout(CTimeout Timeout) { m_Timeout = Timeout; }
	void MaxResponseSize(size_t Size) { m_MaxResponseSize = Size; }
	void LogProgress(HTTPLOG) {}
	void FailOnErrorStatus(bool) {}
	void HeaderString(const char *, const char *) {}
	void Abort() { m_Aborted = true; }
	bool Done() const { return m_State != EHttpState::QUEUED && m_State != EHttpState::RUNNING; }
	EHttpState State() const { return m_State; }
	int StatusCode() const
	{
		Check(m_State == EHttpState::DONE, "StatusCode called on ERROR/ABORTED transport");
		return m_Status;
	}
	json_value *ResultJson() const
	{
		++m_JsonReads;
		BeginRead();
		return json_parse(m_Body.c_str(), m_Body.size());
	}
	void Result(unsigned char **ppResult, size_t *pLength)
	{
		++m_RawReads;
		BeginRead();
		*ppResult = reinterpret_cast<unsigned char *>(m_Body.data());
		*pLength = m_ResultLengthOverride ? m_ResultLengthOverride : m_Body.size();
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

// 字形布局替身只记录测量次数；缓存与字体选择逻辑逐字抽取自生产文件。
class ITextRender
{
public:
	int m_Calls = 0;
	float m_Width = 10.0f;
	std::vector<std::string> m_vFaces{"Default"};
	float TextWidth(float, const char *) { ++m_Calls; return m_Width; }
	std::vector<std::string> *GetCustomFaces() { return &m_vFaces; }
};

class CTestFontConfig
{
public:
	int m_TcFontSize = 100;
	char m_TcIslandFont[128] = "";
	char m_TcCustomFont[255] = "Default";
	char m_TcIslandLrclibBase[128] = "https://lrclib.net";
};
static CTestFontConfig g_Config;
static int str_length(const char *pText) { return (int)strlen(pText); }
static void str_copy(char *pBuffer, const char *pText, int BufferSize)
{
	snprintf(pBuffer, BufferSize, "%s", pText);
}
template<size_t Size>
static void str_copy(char (&aBuffer)[Size], const char *pText)
{
	str_copy(aBuffer, pText, Size);
}

static int str_base64_decode(void *pDestination, int DestinationSize, const char *pData)
{
	static constexpr char DIGITS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const int DataLength = str_length(pData);
	if(DataLength % 4 != 0 || DataLength / 4 * 3 > DestinationSize)
		return -1;
	unsigned char *pOutput = static_cast<unsigned char *>(pDestination);
	int OutputLength = 0;
	for(int i = 0; i < DataLength; i += 4)
	{
		char aCopy[4] = {pData[i], pData[i + 1], pData[i + 2], pData[i + 3]};
		int OutputBytes = 3;
		if(i == DataLength - 4 && aCopy[3] == '=')
		{
			aCopy[3] = 'A';
			OutputBytes = 2;
			if(aCopy[2] == '=')
			{
				aCopy[2] = 'A';
				OutputBytes = 1;
			}
		}
		int aValues[4];
		for(int j = 0; j < 4; ++j)
		{
			const char *pDigit = strchr(DIGITS, aCopy[j]);
			if(pDigit == nullptr)
				return -1;
			aValues[j] = (int)(pDigit - DIGITS);
		}
		const int Value = (aValues[0] << 18) | (aValues[1] << 12) | (aValues[2] << 6) | aValues[3];
		for(int j = 0; j < OutputBytes; ++j)
			pOutput[OutputLength++] = (unsigned char)((Value >> (16 - j * 8)) & 0xff);
	}
	return OutputLength;
}

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
static constexpr const char *NET_FOUND_DETAIL = R"({"result":{"songs":[{"id":7,"name":"TrackA","duration":120000,"artists":[{"name":"Artist"}]}]}})";
static constexpr const char *NET_FOUND_COVER = R"({"result":{"songs":[{"id":7,"name":"TrackA","duration":120000,"artists":[{"name":"Artist"}],"album":{"picUrl":"https://cover.invalid/a.png"}}]}})";
static constexpr const char *NET_LINES = R"({"lrc":{"lyric":"[00:01.00]Net lyric\n[00:03.00]next"}})";
static constexpr const char *DETAIL_FOUND = R"({"songs":[{"album":{"picUrl":"https://cover.invalid/detail.png"}}]})";
static constexpr const char *QQ_FOUND = R"({"code":0,"data":{"song":{"list":[{"songmid":"midA","songname":"TrackA","albumname":"Album","interval":120,"singer":[{"name":"Artist"}]}]}}})";
static constexpr const char *QQ_LINES = R"({"code":0,"lyric":"[00:01.00]QQ lyric\n[00:03.00]next"})";
static constexpr const char *LRC_LINES = R"([{"syncedLyrics":"[00:01.00]lrclib lyric\n[00:03.00]next"}])";

static CMusic::STrack Track(const char *pTitle = "TrackA", const char *pServiceId = "netease-title")
{
	CMusic::STrack Result;
	Result.m_Valid = true;
	Result.m_Title = pTitle;
	Result.m_Artist = "Artist";
	Result.m_ServiceId = pServiceId;
	Result.m_DurationMs = 120000;
	return Result;
}

class CFixture
{
public:
	IHttp m_Http;
	IEngine m_Engine;
	std::unique_ptr<CMusic> m_pMusic = std::make_unique<CMusic>();
	CMusic::STrack m_Track = Track();

	CFixture()
	{
		m_pMusic->m_pHttp = &m_Http;
		m_pMusic->m_pEngine = &m_Engine;
	}
	~CFixture() { DestroyMusic(); }
	CMusic &Music() { return *m_pMusic; }
	void DestroyMusic()
	{
		// The actual CImpl destructor calls this before shutting down its workers.
		if(m_pMusic)
			m_pMusic->ResetLyrics();
		m_pMusic.reset();
	}
};

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

static void Render(CFixture &Fixture, int Frames = 1)
{
	for(int i = 0; i < Frames; ++i)
		Fixture.Music().UpdateLyrics(Fixture.m_Track);
}

static void Tick(CFixture &Fixture, int Frames = 3)
{
	for(int i = 0; i < Frames; ++i)
	{
		Render(Fixture);
		Fixture.m_Engine.RunAll();
	}
}

enum class EStage { NET_SEARCH, NET_LYRIC, QQ_SEARCH, QQ_LYRIC, LRCLIB, DETAIL, COVER };
static constexpr EStage STAGES[] = {EStage::NET_SEARCH, EStage::NET_LYRIC, EStage::QQ_SEARCH, EStage::QQ_LYRIC, EStage::LRCLIB, EStage::DETAIL, EStage::COVER};
static const char *StageName(EStage Stage)
{
	static constexpr const char *NAMES[] = {"NetEase search", "NetEase lyrics", "QQ search", "QQ lyrics", "lrclib", "detail", "cover"};
	return NAMES[static_cast<int>(Stage)];
}

static std::shared_ptr<IHttpRequest> RequestFor(CFixture &Fixture, EStage Stage)
{
	auto &Music = Fixture.Music();
	if(Stage == EStage::LRCLIB) return Music.m_pRequest;
	if(Stage == EStage::QQ_SEARCH || Stage == EStage::QQ_LYRIC) return Music.m_pQQRequest;
	return Music.m_pNeteaseRequest;
}

static std::shared_ptr<IJob> JobFor(CFixture &Fixture, EStage Stage)
{
	auto &Music = Fixture.Music();
	if(Stage == EStage::LRCLIB) return Music.m_pRequestJob;
	if(Stage == EStage::QQ_SEARCH || Stage == EStage::QQ_LYRIC) return Music.m_pQQJob;
	return Music.m_pNeteaseJob;
}

static void Poll(CFixture &Fixture, EStage Stage)
{
	auto &Music = Fixture.Music();
	if(Stage == EStage::LRCLIB) Music.ProcessRequest();
	else if(Stage == EStage::QQ_SEARCH || Stage == EStage::QQ_LYRIC) Music.ProcessQQ();
	else Music.ProcessNetease();
}

static std::string ResponseFor(EStage Stage)
{
	switch(Stage)
	{
	case EStage::NET_SEARCH: return NET_FOUND;
	case EStage::NET_LYRIC: return NET_LINES;
	case EStage::QQ_SEARCH: return QQ_FOUND;
	case EStage::QQ_LYRIC: return QQ_LINES;
	case EStage::LRCLIB: return LRC_LINES;
	case EStage::DETAIL: return DETAIL_FOUND;
	case EStage::COVER: return "\x89PNG\r\nfixture";
	}
	throw std::runtime_error("unknown stage");
}

static void Prepare(CFixture &Fixture, EStage Stage, bool ParallelLyrics = false)
{
	Tick(Fixture);
	// These legacy provider-chain cases cover NetEase -> QQ -> lrclib. v79 added
	// Kugou between QQ and lrclib, which is tested separately by production code.
	Fixture.Music().m_NeteaseData.at(Fixture.Music().m_ActiveKey).m_TriedKugou = true;
	if(Stage == EStage::NET_SEARCH) return;
	Last(NET_SEARCH)->Complete(Stage == EStage::DETAIL ? NET_FOUND_DETAIL : Stage == EStage::COVER ? NET_FOUND_COVER : NET_FOUND);
	Tick(Fixture);
	if(Stage == EStage::NET_LYRIC) return;
	const bool Auxiliary = Stage == EStage::DETAIL || Stage == EStage::COVER;
	Last(NET_LYRIC)->Complete(Auxiliary && !ParallelLyrics ? NET_LINES : "{}");
	Tick(Fixture);
	if(Auxiliary || Stage == EStage::QQ_SEARCH) return;
	Last(QQ_SEARCH)->Complete(QQ_FOUND);
	Tick(Fixture);
	if(Stage == EStage::QQ_LYRIC) return;
	Last(QQ_LYRIC)->Complete("{}");
	Tick(Fixture);
	Check(static_cast<bool>(RequestFor(Fixture, Stage)), "stage did not create a request");
}

static std::string LyricSignature(CFixture &Fixture)
{
	auto &Music = Fixture.Music();
	const auto &Meta = Music.m_NeteaseData.at(Music.m_ActiveKey);
	std::string Result = std::to_string(static_cast<int>(Meta.m_LyricState)) + ":" + std::to_string(Meta.m_LinesRevision) + ":" + std::to_string(Meta.m_TriedLyric) + std::to_string(Meta.m_TriedQQ) + std::to_string(Meta.m_TriedLrclib) + ":" + Meta.m_QQMid;
	for(const auto &Line : Meta.m_vLines) Result += "|" + Line.m_Text;
	for(const auto &Line : Meta.m_vFallbackLines) Result += "!" + Line.m_Text;
	return Result;
}

static std::string PublicationSignature(CFixture &Fixture)
{
	auto &Music = Fixture.Music();
	const auto &Meta = Music.m_NeteaseData.at(Music.m_ActiveKey);
	std::string Result = LyricSignature(Fixture) + ":" + std::to_string(Meta.m_Resolved) + ":" + std::to_string(Meta.m_SongId) + ":" + Meta.m_PicUrl + ":" + std::to_string(Meta.m_DetailQueued) + std::to_string(Meta.m_CoverQueued) + ":" + std::to_string(Music.m_Cache.size()) + ":" + std::to_string(static_cast<int>(Music.m_LyricsState)) + ":" + Music.m_CoverKey + std::to_string(Music.m_CoverDirty);
	for(const auto &Line : Music.m_vLines) Result += "/" + Line.m_Text;
	Result.append(Music.m_CoverPng.begin(), Music.m_CoverPng.end());
	return Result;
}

static void CheckReady(CFixture &Fixture, const char *pText)
{
	Tick(Fixture, 100);
	auto &Music = Fixture.Music();
	Check(Music.m_LyricsState == EState::READY, "lyrics never reached READY");
	Check(!Music.m_vLines.empty() && Music.m_vLines[0].m_Text == pText, "wrong lyrics published");
}

static void TestQQMusicStartsOwnProvider()
{
	CFixture Fixture;
	Fixture.m_Track = Track("TrackA", "Tencent.QQMusic");
	Render(Fixture);
	Check(Count(NET_SEARCH) == 0, "QQMusic incorrectly waited for a NetEase resolve");
	Check(Count(QQ_SEARCH) == 1, "QQMusic did not start QQ search directly");

	Last(QQ_SEARCH)->Complete(QQ_FOUND);
	Tick(Fixture);
	Check(Count(QQ_LYRIC) == 1, "QQ search did not start QQ lyric request");
	Last(QQ_LYRIC)->Complete(QQ_LINES);
	CheckReady(Fixture, "QQ lyric");
	Check(Count(NET_SEARCH) == 0 && Count(QQ_SEARCH) == 1 && Count(QQ_LYRIC) == 1, "QQMusic provider chain repeated or used the wrong source");
}

static void TestDeferredPublication(EStage Stage, bool AbortBeforePublish)
{
	CFixture Fixture;
	Prepare(Fixture, Stage);
	auto pRequest = RequestFor(Fixture, Stage);
	pRequest->Complete(ResponseFor(Stage));
	const auto Before = PublicationSignature(Fixture);
	const auto Added = Fixture.m_Engine.m_Added;
	Render(Fixture, 20);
	auto pJob = JobFor(Fixture, Stage);
	Check(pJob && pJob->State() == IJob::STATE_QUEUED, "HTTP completion did not enqueue a job");
	Check(Fixture.m_Engine.m_Added == Added + 1, "completed HTTP was enqueued repeatedly");
	Check(pRequest->m_JsonReads == 0 && pRequest->m_RawReads == 0, "render tick read an HTTP body");
	Check(RequestFor(Fixture, Stage) == pRequest, "HTTP slot released before job completion");
	Check(PublicationSignature(Fixture) == Before, "queued response was published");
	if(Stage == EStage::DETAIL || Stage == EStage::COVER)
	{
		const auto &Snapshot = Fixture.Music().m_pNeteaseJob->m_Result.m_Meta;
		Check(Snapshot.m_vLines.empty() && Snapshot.m_vFallbackLines.empty(), "job snapshot deep-copied cached lyric vectors");
	}
	auto pPublishGate = std::make_shared<CGate>();
	Fixture.m_Engine.Start(pJob, {}, pPublishGate);
	pPublishGate->Wait();
	Check(pJob->State() == IJob::STATE_RUNNING, "worker published DONE before the gate");
	if(AbortBeforePublish)
	{
		Check(pJob->Abort(), "response job is not abortable");
		Check(pJob->Done() && pJob->State() == IJob::STATE_ABORTED, "production IJob abort semantics changed");
	}
	Render(Fixture, 20);
	Check(PublicationSignature(Fixture) == Before, "RUNNING/ABORTED result published before STATE_DONE");
	Check(RequestFor(Fixture, Stage) == pRequest, "unpublished job lost its request slot");
	Check(Fixture.m_Engine.m_Added == Added + 1, "waiting for worker resubmitted parsing");
	pPublishGate->Release();
	Fixture.m_Engine.Join();
	Poll(Fixture, Stage);
	if(AbortBeforePublish)
		Check(PublicationSignature(Fixture) == Before, "aborted result was published after worker exit");
	else
		Check(RequestFor(Fixture, Stage) != pRequest, "STATE_DONE was not consumed");
	Check(pRequest->m_JsonReads + pRequest->m_RawReads == 1, "response parsed/copied more than once");
}

static void TestCancelAndReenter(EStage Stage, bool Running, bool SwitchAway)
{
	CFixture Fixture;
	Prepare(Fixture, Stage);
	auto pRequest = RequestFor(Fixture, Stage);
	pRequest->Complete(ResponseFor(Stage));
	Poll(Fixture, Stage);
	auto pJob = JobFor(Fixture, Stage);
	auto pReadGate = std::make_shared<CGate>();
	if(Running)
	{
		pRequest->m_pReadGate = pReadGate;
		Fixture.m_Engine.Start(pJob, pReadGate);
		pReadGate->Wait();
	}
	if(SwitchAway)
	{
		Fixture.m_Track = Track("TrackB");
		Render(Fixture);
		Fixture.m_Track = Track();
	}
	else
		Fixture.Music().ResetLyrics();
	Render(Fixture);
	Check(pJob->State() == IJob::STATE_ABORTED && pRequest->m_Aborted, "reset/switch did not abort request and job");
	Check(RequestFor(Fixture, Stage) != pRequest, "same-track reentry reused a canceled HTTP response");
	const auto Before = PublicationSignature(Fixture);
	if(Running)
	{
		pReadGate->Release();
		Fixture.m_Engine.Join();
	}
	else
		Fixture.m_Engine.Run(pJob);
	Render(Fixture, 20);
	Check(PublicationSignature(Fixture) == Before, "late old result corrupted same-key reentry");
	Check(pJob->State() == IJob::STATE_ABORTED, "worker overwrote ABORTED with DONE");
	Check(pRequest->m_JsonReads + pRequest->m_RawReads == (Running ? 1u : 0u), "queued abort ran or running cancellation reread response");
}

static void TestOwnerDestruction(EStage Stage, bool Running)
{
	CFixture Fixture;
	Prepare(Fixture, Stage);
	auto pRequest = RequestFor(Fixture, Stage);
	pRequest->Complete(ResponseFor(Stage));
	Poll(Fixture, Stage);
	auto pJob = JobFor(Fixture, Stage);
	auto pReadGate = std::make_shared<CGate>();
	std::weak_ptr<IHttpRequest> WeakRequest = pRequest;
	if(Running)
	{
		pRequest->m_pReadGate = pReadGate;
		Fixture.m_Engine.Start(pJob, pReadGate);
		pReadGate->Wait();
	}
	s_vCreated.clear();
	pRequest.reset();
	Fixture.DestroyMusic();
	pJob.reset();
	Check(!WeakRequest.expired(), "worker/queue failed to retain its HTTP response");
	if(Running)
	{
		pReadGate->Release();
		Fixture.m_Engine.Join();
	}
	else
		Fixture.m_Engine.RunAll();
	Check(WeakRequest.expired(), "completed/canceled work retained its HTTP response");
}

struct SFailure { const char *m_pName; const char *m_pBody; EHttpState m_State; int m_Status; };
static const SFailure FAILURES[] = {
	{"network", "", EHttpState::ERROR, 0},
	{"aborted", "", EHttpState::ABORTED, 0},
	{"zero-status", "{}", EHttpState::DONE, 0},
	{"404", "{}", EHttpState::DONE, 404},
	{"503", "{}", EHttpState::DONE, 503},
	{"invalid-json", "{", EHttpState::DONE, 200},
	{"wrong-root", "[]", EHttpState::DONE, 200},
	{"empty", "{}", EHttpState::DONE, 200},
	{"instrumental", "{\"instrumental\":true,\"plainLyrics\":\"no timed lyrics\"}", EHttpState::DONE, 200},
};

static void TestProviderFailure(EStage Stage, const SFailure &Failure)
{
	CFixture Fixture;
	Prepare(Fixture, Stage);
	RequestFor(Fixture, Stage)->Complete(Failure.m_pBody, Failure.m_State, Failure.m_Status);
	Tick(Fixture);
	if(Stage == EStage::NET_SEARCH || Stage == EStage::NET_LYRIC)
	{
		Last(QQ_SEARCH)->Complete("{}");
		Tick(Fixture);
	}
	Last(LRCLIB)->Complete(LRC_LINES);
	CheckReady(Fixture, "lrclib lyric");
	Check(Count(NET_SEARCH) == 1 && Count(QQ_SEARCH) == 1 && Count(LRCLIB) == 1, "failure restarted a provider every frame");
}

static void TestLrclibFailure(const SFailure &Failure)
{
	CFixture Fixture;
	Prepare(Fixture, EStage::LRCLIB);
	const bool Offline = Failure.m_State != EHttpState::DONE || Failure.m_Status == 0;
	const int Attempts = Offline ? 3 : 1;
	for(int Attempt = 0; Attempt < Attempts; ++Attempt)
	{
		auto pRequest = RequestFor(Fixture, EStage::LRCLIB);
		pRequest->Complete(Failure.m_pBody, Failure.m_State, Failure.m_Status);
		Tick(Fixture);
		if(Offline && Attempt + 1 < Attempts)
		{
			Check(Count(LRCLIB) == (size_t)Attempt + 1, "lrclib retried before its backoff elapsed");
			AdvanceTime(3);
			Tick(Fixture);
			Check(Count(LRCLIB) == (size_t)Attempt + 2, "lrclib did not retry after its backoff elapsed");
		}
		if(Offline || Failure.m_Status >= 400)
			Check(pRequest->m_JsonReads == 0, "failed transport/status was parsed");
	}
	Tick(Fixture, 10);
	Check(Fixture.Music().m_LyricsState == (Offline ? EState::OFFLINE : EState::NOTFOUND), "lrclib failure did not settle to OFFLINE/NOTFOUND");
	Check(Fixture.Music().m_vLines.empty(), "instrumental/invalid response created lyrics");
	Check(Count(LRCLIB) == (size_t)Attempts, "lrclib failure used the wrong retry count");
}

static void TestInterleaving(EStage Auxiliary, bool UseLrclib, bool ParseAuxiliaryFirst)
{
	CFixture Fixture;
	Prepare(Fixture, Auxiliary, true);
	RequestFor(Fixture, Auxiliary)->Complete(ResponseFor(Auxiliary));
	Poll(Fixture, Auxiliary);
	auto pAuxiliaryJob = JobFor(Fixture, Auxiliary);
	if(ParseAuxiliaryFirst)
		Fixture.m_Engine.Run(pAuxiliaryJob);

	// Deliberately do not poll NetEase while finishing the independent lyric source.
	Last(QQ_SEARCH)->Complete(UseLrclib ? "{}" : QQ_FOUND);
	Poll(Fixture, EStage::QQ_SEARCH);
	Fixture.m_Engine.Run(JobFor(Fixture, EStage::QQ_SEARCH));
	Poll(Fixture, EStage::QQ_SEARCH);
	const EStage LyricStage = UseLrclib ? EStage::LRCLIB : EStage::QQ_LYRIC;
	if(UseLrclib)
		Fixture.Music().StartRequest(Fixture.m_Track);
	RequestFor(Fixture, LyricStage)->Complete(ResponseFor(LyricStage));
	Poll(Fixture, LyricStage);
	Fixture.m_Engine.Run(JobFor(Fixture, LyricStage));
	Poll(Fixture, LyricStage);
	const auto LyricsBefore = LyricSignature(Fixture);
	if(!ParseAuxiliaryFirst)
		Fixture.m_Engine.Run(pAuxiliaryJob);
	Poll(Fixture, Auxiliary);
	Check(LyricSignature(Fixture) == LyricsBefore, "detail/cover merge overwrote concurrent lyrics or source flags");
	CheckReady(Fixture, UseLrclib ? "lrclib lyric" : "QQ lyric");
	if(Auxiliary == EStage::DETAIL)
		Check(Fixture.Music().m_NeteaseData.at(Fixture.Music().m_ActiveKey).m_PicUrl.find("detail.png") != std::string::npos, "detail URL was not merged");
	else
		Check(Fixture.Music().m_CoverDirty && !Fixture.Music().m_CoverPng.empty(), "worker cover bytes were not published");
}

class CTests
{
	int m_Total = 0;
	int m_Failed = 0;

public:
	template<typename F>
	void Case(const std::string &Name, F Test)
	{
		++m_Total;
		s_vCreated.clear();
		try { Test(); }
		catch(const std::exception &Error)
		{
			++m_Failed;
			printf("FAIL %s: %s\n", Name.c_str(), Error.what());
		}
		s_vCreated.clear();
	}

	template<typename F>
	void Group(const char *pName, F Tests)
	{
		const int Before = m_Total, FailuresBefore = m_Failed;
		Tests();
		printf("%s %s: %d/%d cases\n", m_Failed == FailuresBefore ? "PASS" : "FAIL", pName, m_Total - Before - (m_Failed - FailuresBefore), m_Total - Before);
	}

	int Finish() const
	{
		printf("RESULT %d/%d cases passed; %d failures\n", m_Total - m_Failed, m_Total, m_Failed);
		return m_Failed ? 1 : 0;
	}
};

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	CTests Tests;
	Tests.Group("production text width cache invalidation", [&] {
		Tests.Case("font changes invalidate and stable fonts reuse widths", [&] {
			CMusic Music;
			ITextRender Text;
			g_Config = CTestFontConfig{};
			Check(Music.MeasureCached(&Text, 12.0f, "00:00") == 10.0f, "initial width wrong");
			Music.MeasureCached(&Text, 12.0f, "00:00");
			Check(Text.m_Calls == 1, "unchanged font missed the cache");
			str_copy(g_Config.m_TcIslandFont, "Wide");
			Text.m_Width = 20.0f;
			Check(Music.MeasureCached(&Text, 12.0f, "00:00") == 20.0f, "island font reused stale width");
			str_copy(g_Config.m_TcIslandFont, "");
			str_copy(g_Config.m_TcCustomFont, "GlobalWide");
			Text.m_Width = 30.0f;
			Check(Music.MeasureCached(&Text, 12.0f, "00:00") == 30.0f, "global font reused stale width");
			str_copy(g_Config.m_TcCustomFont, "");
			Text.m_Width = 40.0f;
			Check(Music.MeasureCached(&Text, 12.0f, "00:00") == 40.0f, "fallback font reused stale width");
			Text.m_vFaces[0] = "NewDefault";
			Text.m_Width = 50.0f;
			Check(Music.MeasureCached(&Text, 12.0f, "00:00") == 50.0f, "changed fallback font reused stale width");
			g_Config.m_TcFontSize = 120;
			Text.m_Width = 60.0f;
			Check(Music.MeasureCached(&Text, 12.0f, "00:00") == 60.0f, "font size reused stale width");
			Music.MeasureCached(&Text, 12.0f, "00:00");
			Check(Text.m_Calls == 6, "stable width was measured again");
			g_Config = CTestFontConfig{};
		});
	});
	Tests.Group("QQMusic session starts its native lyric provider", [&] {
		Tests.Case("Tencent.QQMusic bypasses NetEase resolve", TestQQMusicStartsOwnProvider);
	});
	Tests.Group("worker-only parsing, single enqueue, STATE_DONE publication", [&] {
		for(const auto Stage : STAGES)
			Tests.Case(StageName(Stage), [&] { TestDeferredPublication(Stage, false); });
	});
	Tests.Group("ABORTED is not a publishable completed result", [&] {
		for(const auto Stage : STAGES)
			Tests.Case(StageName(Stage), [&] { TestDeferredPublication(Stage, true); });
	});
	Tests.Group("queued/running cancellation, same-song reset and A-B-A switch", [&] {
		for(const auto Stage : STAGES)
			for(bool Running : {false, true})
				for(bool SwitchAway : {false, true})
					Tests.Case(std::string(StageName(Stage)) + (Running ? " running" : " queued") + (SwitchAway ? " A-B-A" : " reset"), [&] { TestCancelAndReenter(Stage, Running, SwitchAway); });
	});
	Tests.Group("request ownership after CImpl reset/destruction", [&] {
		for(const auto Stage : STAGES)
			for(bool Running : {false, true})
				Tests.Case(std::string(StageName(Stage)) + (Running ? " running" : " queued"), [&] { TestOwnerDestruction(Stage, Running); });
	});
	Tests.Group("NetEase/QQ transport, 404, malformed and instrumental fallback", [&] {
		for(const auto Stage : {EStage::NET_SEARCH, EStage::NET_LYRIC, EStage::QQ_SEARCH, EStage::QQ_LYRIC})
			for(const auto &Failure : FAILURES)
				Tests.Case(std::string(StageName(Stage)) + " " + Failure.m_pName, [&] { TestProviderFailure(Stage, Failure); });
	});
	Tests.Group("lrclib terminal failures and array/object response formats", [&] {
		for(const auto &Failure : FAILURES)
			Tests.Case(Failure.m_pName, [&] { TestLrclibFailure(Failure); });
		for(const char *pBody : {LRC_LINES, R"({"syncedLyrics":"[00:01.00]lrclib lyric"})", R"([null,{}, {"plainLyrics":"untimed"}, {"syncedLyrics":"[00:01.00]lrclib lyric"}])"})
			Tests.Case("synced lyric format", [&] {
				CFixture Fixture;
				Prepare(Fixture, EStage::LRCLIB);
				Last(LRCLIB)->Complete(pBody);
				CheckReady(Fixture, "lrclib lyric");
			});
	});
	Tests.Group("detail/cover versus QQ/lrclib in both completion orders", [&] {
		for(const auto Auxiliary : {EStage::DETAIL, EStage::COVER})
			for(bool UseLrclib : {false, true})
				for(bool ParseFirst : {false, true})
					Tests.Case(std::string(StageName(Auxiliary)) + (UseLrclib ? " lrclib" : " QQ") + (ParseFirst ? " first" : " last"), [&] { TestInterleaving(Auxiliary, UseLrclib, ParseFirst); });
	});
	Tests.Group("HTTP body bounds and oversized cover rejection", [&] {
		for(const auto Stage : STAGES)
			Tests.Case(StageName(Stage), [&] {
				CFixture Fixture;
				Prepare(Fixture, Stage);
				Check(RequestFor(Fixture, Stage)->m_MaxResponseSize == (Stage == EStage::COVER ? 16u : 4u) * 1024u * 1024u, "HTTP response limit missing/wrong");
			});
		Tests.Case("oversized raw cover", [&] {
			CFixture Fixture;
			Prepare(Fixture, EStage::COVER);
			auto pRequest = RequestFor(Fixture, EStage::COVER);
			pRequest->Complete("x");
			pRequest->m_ResultLengthOverride = 16u * 1024u * 1024u + 1;
			Tick(Fixture);
			Check(pRequest->m_RawReads == 1 && Fixture.Music().m_CoverPng.empty() && !Fixture.Music().m_CoverDirty, "oversized cover was copied/published");
		});
	});
	Tests.Group("missing HTTP/engine remains offline without requests", [&] {
		for(int Missing = 1; Missing <= 3; ++Missing)
			for(bool DirectLrclib : {false, true})
				Tests.Case("missing dependency", [&] {
					CFixture Fixture;
					if(Missing & 1) Fixture.Music().m_pHttp = nullptr;
					if(Missing & 2) Fixture.Music().m_pEngine = nullptr;
					if(DirectLrclib)
					{
						Fixture.Music().m_ActiveKey = "isolated-lrclib";
						Fixture.Music().StartRequest(Fixture.m_Track);
					}
					else
						Render(Fixture, 20);
					Check(Fixture.Music().m_LyricsState == EState::OFFLINE && s_vCreated.empty() && Fixture.m_Engine.m_Added == 0, "missing engine/HTTP enqueued work or stayed loading");
				});
	});
	Tests.Group("QQ submission and stable track identity", [&] {
		Tests.Case("QQ lyrics", [&] {
			CFixture Fixture;
			Prepare(Fixture, EStage::QQ_LYRIC);
			Last(QQ_LYRIC)->Complete(QQ_LINES);
			CheckReady(Fixture, "QQ lyric");
			Check(Count(QQ_SEARCH) == 1 && Count(QQ_LYRIC) == 1 && Count(LRCLIB) == 0, "successful QQ chain created extra requests");
		});
	});
	Tests.Group("real YRC priority and LRC degradation", [&] {
		const char *apResponses[] = {
			R"({"yrc":{"lyric":"[1000,1000](1000,400,0)Y(1400,600,0)RC"},"lrc":{"lyric":"[00:01.00]fallback"}})",
			R"({"yrc":{"lyric":"bad yrc"},"lrc":{"lyric":"[00:01.00]fallback"}})",
		};
		for(int i = 0; i < 2; ++i)
			Tests.Case(i ? "LRC fallback" : "YRC words", [&] {
				CFixture Fixture;
				Prepare(Fixture, EStage::NET_LYRIC);
				Last(NET_LYRIC)->Complete(apResponses[i]);
				CheckReady(Fixture, i == 0 ? "YRC" : "fallback");
				const auto &Words = Fixture.Music().m_vLines[0].m_vWords;
				Check(!Words.empty(), "karaoke word timings lost");
				if(i == 0) Check(Words.size() > 1 && Words[1].m_StartMs == 1400, "real YRC timings were synthesized");
				Check(Count(QQ_SEARCH) == 0 && Count(LRCLIB) == 0, "valid NetEase lyrics triggered fallback");
			});
	});
	return Tests.Finish();
}
