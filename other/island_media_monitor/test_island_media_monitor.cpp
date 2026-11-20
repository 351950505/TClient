// 音乐监控助手的离线夹具测试。
//
// 直接包含 monitor 源文件，复用其中的 static 解析/门控函数；wWinMain 改名
// 避免与测试 main 冲突。构建：
//   cl /nologo /O2 /EHsc /W0 /utf-8 /DUNICODE /D_UNICODE test_island_media_monitor.cpp
//      /Fe:test_island_media_monitor.exe /link shell32.lib /SUBSYSTEM:CONSOLE
// 退出码 0 = 全部通过。

#define wWinMain TClientMonitorWWinMain
#include "island_media_monitor.cpp"
#undef wWinMain

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int s_Failures = 0;

static void Check(bool Condition, const char *pName)
{
	if(!Condition)
	{
		++s_Failures;
		printf("FAIL: %s\n", pName);
	}
	else
		printf("PASS: %s\n", pName);
}

static bool Nearly(double A, double B, double Eps = 1e-9)
{
	return A > B - Eps && A < B + Eps;
}

// 数字串顶到缓冲尾、后面没有任何 NUL/分隔符：旧实现 strtod 会越界读。
static void TestParseJsonNumberAtBufferEnd()
{
	uint8_t *pBuf = (uint8_t *)malloc(4);
	memcpy(pBuf, "0.42", 4);
	Check(Nearly(ParseJsonNumber(pBuf, pBuf + 4), 0.42), "number ending exactly at buffer end");
	free(pBuf);

	pBuf = (uint8_t *)malloc(4);
	memcpy(pBuf, "219.", 4);
	Check(Nearly(ParseJsonNumber(pBuf, pBuf + 4), 219.0), "decimal point at buffer end stays finite");
	free(pBuf);
}

static void TestParseJsonNumberDelimiters()
{
	const char *pCases[] = {
		"0.25,pauseStatus", "0.25}rest", "0.25]rest", "0.25 rest", "0.25\r\nrest",
	};
	for(const char *pCase : pCases)
	{
		const uint8_t *p = (const uint8_t *)pCase;
		Check(Nearly(ParseJsonNumber(p, p + strlen(pCase)), 0.25), pCase);
	}
	const char *pLong = "1234567890123456789012345678901234567890";
	Check(ParseJsonNumber((const uint8_t *)pLong, (const uint8_t *)pLong + strlen(pLong)) > 1e30, "long number capped at 31 chars");
	Check(Nearly(ParseJsonNumber((const uint8_t *)"-1.5e2]", (const uint8_t *)"-1.5e2]" + 7), -150.0), "sign and exponent");
	Check(ParseJsonNumber((const uint8_t *)"", (const uint8_t *)"") == 0.0, "empty input returns zero");
}

static void WriteAll(const wchar_t *pPath, const char *pData)
{
	FILE *pFile = _wfopen(pPath, L"wb");
	if(!pFile)
	{
		printf("FAIL: cannot write fixture %ls\n", pPath);
		++s_Failures;
		return;
	}
	fwrite(pData, 1, strlen(pData), pFile);
	fclose(pFile);
}

static void TestParseLSFileDurationAtEnd()
{
	wchar_t aPath[MAX_PATH];
	GetTempPathW(MAX_PATH, aPath);
	wcscat_s(aPath, L"tclient_ls_fixture.log");
	const char *pData =
		"{\"lastPlaying\":{\"songId\":\"1\",\"playPostion\":0.25,\"pauseStatus\":false}}"
		"{\"lastPlayInfoForRestore\":{\"playInfo\":{\"duration\":219.86";
	WriteAll(aPath, pData);

	g_LsHave = false;
	g_LsFraction = 0.0;
	g_LsDurationMs = 0.0;
	FILETIME Write = {0, 0};
	ParseLSFile(aPath, &Write);
	Check(g_LsHave, "fixture lastPlaying parsed");
	Check(Nearly(g_LsFraction, 0.25), "fixture playPostion value");
	Check(Nearly(g_LsDurationMs, 219860.0, 0.01), "fixture duration at buffer end");
	DeleteFileW(aPath);
}

static void TestMtimeGate()
{
	wchar_t aPath[MAX_PATH];
	GetTempPathW(MAX_PATH, aPath);
	wcscat_s(aPath, L"tclient_ls_mtime.log");
	WriteAll(aPath, "x");

	FILETIME Write = {0, 0};
	Write.dwLowDateTime = 1234;
	Write.dwHighDateTime = 5;
	g_wszLsScannedFile[0] = 0;
	g_LsScannedWrite.dwLowDateTime = 0;
	g_LsScannedWrite.dwHighDateTime = 0;
	Check(!IsLsFileUnchanged(aPath, Write), "first scan is not gated");

	wcsncpy_s(g_wszLsScannedFile, aPath, _TRUNCATE);
	g_LsScannedWrite = Write;
	Check(IsLsFileUnchanged(aPath, Write), "same file and mtime is gated");

	Write.dwLowDateTime += 1;
	Check(!IsLsFileUnchanged(aPath, Write), "mtime change bypasses gate");
	DeleteFileW(aPath);
}

static void TestAtomicFrameReset()
{
	SLoopStream Stream;
	InterlockedExchangeAdd64(&Stream.ActiveFrames, 48000);
	InterlockedExchangeAdd64(&Stream.ActiveFrames, 48000);
	Check(Stream.ActiveFrames == 96000, "atomic frame accumulation");
	InterlockedExchange64(&Stream.ActiveFrames, 0);
	Check(Stream.ActiveFrames == 0, "atomic frame reset after new track");
}

static void TestHandlerRefCount()
{
	CActHandler *pHandler = new CActHandler();
	Check(pHandler->AddRef() == 2, "handler AddRef");
	Check(pHandler->Release() == 1, "handler Release keeps object alive");
	Check(pHandler->Release() == 0, "handler final Release destroys object");
}

static void TestWatcherPauseTiming()
{
	Check(!ResolveFallbackPlaying(true, false, false, true, true), "watcher pause stops fallback playback");
	Check(ResolveFallbackPlaying(false, false, false, true, false), "watcher resume restores fallback playback");
	Check(!ResolveFallbackPlaying(false, false, false, false, false), "missing watcher preserves prior paused state");
	Check(!ResolveFallbackPlaying(true, true, false, true, false), "LS pause retains priority over watcher");
	Check(!ResolveFallbackPlaying(true, false, true, true, false), "LS stalled position retains priority over watcher");
	SWatcherPauseTracker Tracker;
	bool PausedWhilePlaying = false;
	for(int i = 0; i < 200; ++i)
	{
		const double SampleAt = 100.0 + (i / 6) * 0.3;
		PausedWhilePlaying |= Tracker.Update((i / 6) * 300.0, SampleAt, 100.0 + i * 0.05);
	}
	Check(!PausedWhilePlaying, "50ms main loop does not mistake repeated 300ms samples for pause");
	Tracker = {};
	Check(!Tracker.Update(0.0, 100.0, 100.0), "zero position starts a valid pause baseline");
	Check(!Tracker.Update(0.0, 102.9, 102.9), "pause needs three elapsed seconds");
	Check(Tracker.Update(0.0, 103.0, 103.0), "three seconds without progress detects pause");
	Check(!Tracker.Update(500.0, 103.1, 103.1), "seek or resume clears pause immediately");
	Check(!Tracker.Update(500.0, 103.1, 109.0), "stale watcher does not assert pause");
	g_vPidBackoff.clear();
	for(int i = 0; i < 3; ++i)
		BumpPidBackoff(12345, 100.0 + i);
	Check(PidBackedOff(12345, 110.0), "three failed activations trigger backoff");
	Check(!PidBackedOff(12345, 163.0), "backoff permits a retry after sixty seconds");
	g_vPidBackoff.clear();
}

static int s_SampledPidCount = 0;
static int s_PidSamples = 0;

static int SampleFixturePids(uint32_t *pPids, uint32_t *pParents)
{
	++s_PidSamples;
	// 失败前也写入局部缓冲，验证半次采样不会污染已发布表。
	pPids[0] = 12345;
	pParents[0] = 6789;
	return s_SampledPidCount;
}

static void TestPlayerGoneConfirmation()
{
	InitializeCriticalSection(&g_LoopCS);
	g_LoopCSReady = true;
	s_SampledPidCount = 1;
	Check(RefreshPids(SampleFixturePids) == 1 && IsTargetPid(12345), "successful sample publishes PID table");
	s_SampledPidCount = -1;
	Check(RefreshPids(SampleFixturePids) == -1 && TargetPidCount() == 1 && IsTargetPid(12345), "failed sample preserves published PID table");

	SPlayerGoneTracker Tracker;
	s_PidSamples = 0;
	Check(!Tracker.Update(false, 0, 100.75, SampleFixturePids), "empty cached table starts debounce");
	Check(!Tracker.Update(false, 0, 102.74, SampleFixturePids) && s_PidSamples == 0, "debounce waits two full seconds before sampling");
	s_SampledPidCount = 1;
	Check(!Tracker.Update(false, 0, 102.75, SampleFixturePids) && s_PidSamples == 1, "restart found by fresh sample prevents Gone despite empty cache");
	Check(!Tracker.Update(false, TargetPidCount(), 110.0, SampleFixturePids), "paused or tray player without title remains present");

	Tracker = {};
	s_SampledPidCount = -1;
	Tracker.Update(false, 0, 200.0, SampleFixturePids);
	Check(!Tracker.Update(false, 0, 202.0, SampleFixturePids), "confirmation sampling failure never means Gone");
	s_SampledPidCount = 0;
	Check(!Tracker.Update(false, 0, 202.05, SampleFixturePids), "failed confirmation restarts debounce");
	Check(Tracker.Update(false, 0, 204.05, SampleFixturePids), "successful empty fresh sample confirms actual exit");
	s_SampledPidCount = -1;
	Check(!Tracker.Update(false, 0, 204.55, SampleFixturePids), "failed recheck clears previously confirmed Gone");

	Tracker = {};
	s_SampledPidCount = 0;
	Tracker.Update(false, 0, 300.0, SampleFixturePids);
	Check(Tracker.Update(false, 0, 302.0, SampleFixturePids), "exit can be confirmed again");
	s_SampledPidCount = 1;
	Check(!Tracker.Update(false, 0, 302.5, SampleFixturePids), "fresh recheck detects restart before periodic PID refresh");
	Tracker.Update(false, 0, 400.0, SampleFixturePids);
	Check(!Tracker.Update(true, 0, 402.0, SampleFixturePids), "visible title cancels pending exit");

	s_SampledPidCount = 0;
	RefreshPids(SampleFixturePids);
	g_LoopCSReady = false;
	DeleteCriticalSection(&g_LoopCS);
}

static HANDLE s_SampleEntered, s_ReleaseSample;
static int BlockingEmptySample(uint32_t *, uint32_t *)
{
	SetEvent(s_SampleEntered);
	WaitForSingleObject(s_ReleaseSample, 10000);
	return 0;
}
static DWORD WINAPI RefreshEmptyThread(LPVOID)
{
	RefreshPids(BlockingEmptySample);
	return 0;
}
static DWORD WINAPI RefreshPresentThread(LPVOID)
{
	RefreshPids(SampleFixturePids);
	return 0;
}
static void TestRefreshOrdering()
{
	InitializeCriticalSection(&g_LoopCS);
	g_LoopCSReady = true;
	s_SampleEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	s_ReleaseSample = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE First = CreateThread(nullptr, 0, RefreshEmptyThread, nullptr, 0, nullptr);
	Check(WaitForSingleObject(s_SampleEntered, 10000) == WAIT_OBJECT_0, "first sampler reached deterministic gate");
	const bool Acquired = TryAcquireSRWLockExclusive(&g_PidRefreshLock) != FALSE;
	Check(!Acquired, "production refresh owns serialization lock while sampling");
	if(Acquired) ReleaseSRWLockExclusive(&g_PidRefreshLock);
	s_SampledPidCount = 1;
	HANDLE Second = CreateThread(nullptr, 0, RefreshPresentThread, nullptr, 0, nullptr);
	SetEvent(s_ReleaseSample);
	Check(WaitForSingleObject(First, 10000) == WAIT_OBJECT_0 && WaitForSingleObject(Second, 10000) == WAIT_OBJECT_0, "both interleaved production refreshes finish");
	Check(TargetPidCount() == 1 && IsTargetPid(12345), "later present sample cannot be overwritten by earlier empty sample");
	CloseHandle(First); CloseHandle(Second); CloseHandle(s_SampleEntered); CloseHandle(s_ReleaseSample);

	SPlayerGoneTracker Tracker;
	s_SampledPidCount = 0;
	Tracker.Update(false, 0, 100, SampleFixturePids);
	Tracker.Update(false, 0, 102, SampleFixturePids);
	Check(Tracker.ConfirmedForPublication(), "fresh empty confirmation is publishable");
	s_SampledPidCount = 1; RefreshPids(SampleFixturePids);
	Check(!Tracker.ConfirmedForPublication(), "restart between confirmation and publication invalidates Gone");
	s_SampledPidCount = 0;
	Tracker = {}; Tracker.Update(false, 0, 200, SampleFixturePids); Tracker.Update(false, 0, 202, SampleFixturePids);
	s_SampledPidCount = -1; RefreshPids(SampleFixturePids);
	Check(!Tracker.ConfirmedForPublication(), "failed sample between confirmation and publication invalidates Gone");
	g_LoopCSReady = false;
	DeleteCriticalSection(&g_LoopCS);
}

static void TestProductionPlaybackDecision()
{
	SPlaybackDecision Decision;
	auto Tick = [&](bool Changed, bool Wnd, bool Audio, bool Active, bool Previous, bool Paused, bool Stalled, bool LsEnd, double Pos, double Dur, double At, double Now) {
		return Decision.Update(Changed, Wnd, Audio, Active, Previous, Paused, Stalled, LsEnd, Pos, Dur, At, Now);
	};
	Check(Tick(true, true, false, false, true, false, false, false, 10000, 60000, 100, 100), "production new song starts playing");
	Check(Tick(false, false, false, false, true, false, false, false, 10300, 60000, 100.3, 100.3), "title loss while watcher advances remains playing");
	Check(Tick(false, true, false, false, true, false, false, false, 10600, 60000, 100.6, 100.6), "same title reappearance remains playing");
	Check(!Tick(false, true, false, false, true, true, false, false, 10600, 60000, 101, 101), "real LS pause retains priority");
	Check(!Tick(false, true, false, false, true, false, false, false, 10600, 60000, 104, 104), "watcher frozen normal pause stops playback");
	Check(Tick(false, false, false, false, false, false, false, false, 11000, 60000, 104.3, 104.3), "watcher resume without title restores playback");
	Check(!Tick(false, false, false, false, true, false, false, false, 60000, 60000, 110, 110), "actual endpoint stops before watcher freeze timeout");
	Check(!Tick(false, true, false, false, true, false, false, false, 60000, 60000, 110.3, 110.3), "endpoint cannot flash back when old title reappears");
	Check(Tick(false, true, false, false, false, false, false, false, 0, 60000, 111, 111), "same song restart clears endpoint latch");
	Check(Tick(false, false, true, true, false, true, true, true, 60000, 60000, 112, 112), "fresh audio overrides stale endpoint and pause");
	Check(Tick(true, true, false, false, true, false, false, false, 0, 120000, 113, 113), "new song discards old endpoint state");
	Check(!Tick(false, false, false, false, false, false, false, false, -1, 120000, 113, 120), "failed watcher sampling cannot invent playback");
	Decision = {};
	Check(!Decision.Update(false, false, false, false, true, false, false, true, -1, 60000, 0, 121), "real LS endpoint also stops playback");
	Check(Decision.Update(false, false, false, false, false, false, false, false, -1, 60000, 0, 122, true), "fresh LS progress restores same-song playback");
}

static void TestWatcherStartRetry()
{
	SWatcherStartBackoff Backoff;
	int Attempts = 0;
	bool StartSucceeded = false;
	const auto Start = [&]() {
		++Attempts;
		return StartSucceeded;
	};
	Check(Backoff.TryStart(0.0, Start) && Attempts == 1, "watcher first failed start is attempted even at time zero");
	Check(!Backoff.TryStart(0.3, Start) && !Backoff.TryStart(29.99, Start) && Attempts == 1, "failed watcher start enters thirty second backoff");
	Check(Backoff.TryStart(30.0, Start) && Attempts == 2, "failed watcher start retries at deadline");
	StartSucceeded = true;
	Check(Backoff.TryStart(60.0, Start) && Attempts == 3, "watcher retry can recover successfully");
	Check(!Backoff.TryStart(60.3, Start) && Attempts == 3, "successful but immediately dead watcher also stays throttled");
}

int main()
{
	TestPlayerGoneConfirmation();
	TestRefreshOrdering();
	TestProductionPlaybackDecision();
	TestWatcherStartRetry();
	TestWatcherPauseTiming();
	TestParseJsonNumberAtBufferEnd();
	TestParseJsonNumberDelimiters();
	TestParseLSFileDurationAtEnd();
	TestMtimeGate();
	TestAtomicFrameReset();
	TestHandlerRefCount();
	printf(s_Failures ? "FAILED: %d\n" : "OK: all fixture tests passed\n", s_Failures);
	return s_Failures ? 1 : 0;
}
