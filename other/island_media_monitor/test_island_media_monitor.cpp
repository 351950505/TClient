// 音乐监控助手的离线夹具测试。
//
// 直接包含 monitor 源文件，复用其中的 static 解析/门控函数；wWinMain 改名
// 避免与测试 main 冲突。构建：
//   cl /nologo /O2 /EHsc /W0 /utf-8 /DUNICODE /D_UNICODE test_island_media_monitor.cpp
//      /Fe:test_island_media_monitor.exe /link /SUBSYSTEM:CONSOLE
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

int main()
{
	TestParseJsonNumberAtBufferEnd();
	TestParseJsonNumberDelimiters();
	TestParseLSFileDurationAtEnd();
	TestMtimeGate();
	TestAtomicFrameReset();
	TestHandlerRefCount();
	printf(s_Failures ? "FAILED: %d\n" : "OK: all fixture tests passed\n", s_Failures);
	return s_Failures ? 1 : 0;
}
