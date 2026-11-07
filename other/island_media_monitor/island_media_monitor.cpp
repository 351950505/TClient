// island_media_monitor.cpp
// 网易云音乐桌面版媒体状态监控 — 独立小程序
// 原理：
//   1. 监视 cloudmusic.exe 的隐藏窗口标题（网易云播放器窗口标题 = "歌名(带 with) - 歌手"，托盘最小化也实时更新）
//   2. WASAPI 音频电表探测该进程是否真的在出声（区分暂停/播放）
//   3. 把 {标题, 累计播放秒数, 播放中标志} 写入命名共享内存 Local\TClientIslandMedia
// TClient 灵动岛读取该共享内存显示歌曲/歌词/进度。
// 编译: cl /O2 /EHsc /DUNICODE /D_UNICODE island_media_monitor.cpp /link ole32.lib user32.lib kernel32.lib /SUBSYSTEM:WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tcpmib.h>
#include <iphlpapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <vector>
#include <atomic>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <functiondiscoverykeys_devpkey.h>
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

static const wchar_t SHM_NAME[] = L"Local\\TClientIslandMedia";
static const wchar_t MUTEX_NAME[] = L"Local\\TClientIslandMonitorMutex";
static const uint32_t SHM_MAGIC = 0x4D53494E; // "NISM"
static const uint32_t SHM_VERSION = 7;

#pragma pack(push, 8)
struct IslandMediaShared
{
	uint32_t Magic;      // SHM_MAGIC
	uint32_t Version;    // SHM_VERSION
	uint32_t Playing;    // 1 = 累计判断为播放
	int64_t ElapsedMs;   // 当前标题开始以来累计播放毫秒（暂停冻结，切歌归零）
	double LastUpdate;   // 最近写入时间（epoch 秒，探活用）
	wchar_t Title[256];  // 最近一次窗口标题 "歌名 - 歌手"
	float Peak;          // 最近一次电表峰值（诊断）
	uint32_t Meters;     // 匹配到 cloudmusic 的电表会话数（诊断）
	double Fraction;     // netease LS playPostion 0..1（seek 后的真实进度）
	double FractionAt;   // 采集时刻（epoch 秒）
	uint32_t Paused;     // netease LS pauseStatus
	double DurationMs;   // netease lastPlayInfoForRestore 里的 duration（毫秒）；0=未知（新版网易云不写）
	double AudioMs;      // WASAPI 进程环回累计的已渲染音频毫秒（暂停自动冻结、切歌清零）＝精确"播到哪一秒"
	uint32_t AudioActive;// 最近 300ms 内有音频帧 = 1（比电表/LS 都可靠的播放/暂停判定）
	double ClientDurationMs; // 【客户端写】网易云 API resolve 出的当前歌曲长（毫秒）
	double WatcherMs;    // 【monitor 写】netease-watcher 的精确播放毫秒（歌曲内真实位置，暂停冻结）＝位置本体
	double WatcherDurMs; // 【monitor 写】netease-watcher 的曲长毫秒
};
#pragma pack(pop)

static HANDLE g_hShm = NULL;
static IslandMediaShared *g_pShm = nullptr;

// ---------- 进程 PID 收集（cloudmusic.exe） ----------
#define MAX_PIDS 32
static int g_NumPids = 0;
static uint32_t g_aPids[MAX_PIDS];
static uint32_t g_aPidParents[MAX_PIDS]; // 父进程 PID，用于识别根进程建环回流

static void RefreshPids()
{
	g_NumPids = 0;
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(hSnap == INVALID_HANDLE_VALUE)
		return;
	PROCESSENTRY32W Pe;
	Pe.dwSize = sizeof(Pe);
	if(Process32FirstW(hSnap, &Pe))
	{
		do
		{
			if(_wcsicmp(Pe.szExeFile, L"cloudmusic.exe") == 0 && g_NumPids < MAX_PIDS)
			{
				g_aPids[g_NumPids] = Pe.th32ProcessID;
				g_aPidParents[g_NumPids] = Pe.th32ParentProcessID;
				++g_NumPids;
			}
		} while(Process32NextW(hSnap, &Pe));
	}
	CloseHandle(hSnap);
}

static bool IsTargetPid(uint32_t Pid)
{
	for(int i = 0; i < g_NumPids; ++i)
		if(g_aPids[i] == Pid)
			return true;
	return false;
}

// ---------- 窗口标题探测 ----------
static wchar_t g_wszBestTitle[256];
static bool g_HaveTitle = false;

static const wchar_t *const s_apJunkPrefix[] = {
	L"GDI+ Window", L"Default IME", L"MSCTFIME UI", L"桌面歌词", L"迷你播放器", L"环绕声", L"桌面歌词解锁",
};

// 标题选择策略：网易云后台会残留旧窗口（含上一首的标题），EnumWindows 顺序又不稳定，
// 之前"取第一个含 - 的"会被陈旧窗口抢标题 → 灵动岛显示切不回当前歌。
// 改成收集所有候选后按"标题变化时间最新"挑：GUITHREADINFO 拿前台窗口最优先，
// 其余候选用-lastinput/tick 近似。多个窗口标题一样时无所谓，随便取。
struct SWndCand
{
	wchar_t Title[256];
	BOOL Foreground;
	DWORD LastChangeTick; // 标题内容变化检测用的 tick
};

static SWndCand g_aCand[16];
static int g_nCand = 0;
static DWORD g_FgThread = 0;
static HWND g_hFgHwnd = nullptr;

static BOOL CALLBACK EnumWndProc(HWND hWnd, LPARAM)
{
	DWORD EnumPid = 0;
	GetWindowThreadProcessId(hWnd, &EnumPid);
	if(!IsTargetPid((uint32_t)EnumPid))
		return TRUE;

	wchar_t awTitle[256];
	int Len = GetWindowTextW(hWnd, awTitle, 256);
	if(Len < 5 || !wcschr(awTitle + 3, L'-'))
		return TRUE;

	bool Junk = false;
	for(const wchar_t *pJunk : s_apJunkPrefix)
		if(wcsncmp(awTitle, pJunk, wcslen(pJunk)) == 0)
		{
			Junk = true;
			break;
		}
	if(Junk)
		return TRUE;

	if(g_nCand < 16)
	{
		SWndCand &C = g_aCand[g_nCand++];
		wcsncpy(C.Title, awTitle, 255);
		C.Title[255] = 0;
		// 前台窗口（含其子窗口链）最优先
		C.Foreground = (g_FgThread != 0 && GetWindowThreadProcessId(hWnd, nullptr) == g_FgThread) || hWnd == g_hFgHwnd;
		// 标题变化检测：用窗口的 GetProp 不行（是外部进程），直接用 cloaked/visible + tick 近似：
		// 不可见窗口必然是陈旧后台窗，变化时间按 0 处理（永远排在可见窗口后面）
		C.LastChangeTick = IsWindowVisible(hWnd) ? GetTickCount() : 0;
	}
	return TRUE;
}


static bool PollWindowTitle()
{
	g_nCand = 0;
	g_hFgHwnd = GetForegroundWindow();
	g_FgThread = g_hFgHwnd ? GetWindowThreadProcessId(g_hFgHwnd, nullptr) : 0;
	EnumWindows(EnumWndProc, 0);
	if(g_nCand == 0)
		return false;
	// 选优先级最高的：前台 > 可见(新tick) > 其余
	int Best = -1;
	for(int i = 0; i < g_nCand; ++i)
	{
		if(Best < 0)
		{
			Best = i;
			continue;
		}
		const SWndCand &A = g_aCand[i];
		const SWndCand &B = g_aCand[Best];
		bool ABetter =
			(A.Foreground != B.Foreground) ? A.Foreground : (A.LastChangeTick > B.LastChangeTick);
		if(ABetter)
			Best = i;
	}
	if(Best < 0)
		return false;
	if(!g_HaveTitle || wcscmp(g_wszBestTitle, g_aCand[Best].Title) != 0)
	{
		wcsncpy(g_wszBestTitle, g_aCand[Best].Title, 255);
		g_wszBestTitle[255] = 0;
		g_HaveTitle = true;
	}
	// structured as "song(with x) - artist"
	return true;
}

// ---------- WASAPI 电表（cloudmusic 会话峰值） ----------
static IAudioSessionEnumerator *g_pSessEnum = nullptr;
static LARGE_INTEGER g_qpcFreq;

struct SSessionMeter
{
	uint32_t Pid;
	IAudioMeterInformation *pMeter;
};
static SSessionMeter g_aMeters[16];
static int g_NumMeters = 0;
static LARGE_INTEGER g_liLastSessScan;

static void ScanSessions()
{
	// 释放旧的
	for(int i = 0; i < g_NumMeters; ++i)
	{
		if(g_aMeters[i].pMeter)
			g_aMeters[i].pMeter->Release();
	}
	g_NumMeters = 0;
	if(!g_pSessEnum)
		return;
	int Count = 0;
	if(FAILED(g_pSessEnum->GetCount(&Count)))
		return;
	for(int i = 0; i < Count && g_NumMeters < 16; ++i)
	{
		IAudioSessionControl *pCtrl = nullptr;
		if(FAILED(g_pSessEnum->GetSession(i, &pCtrl)) || !pCtrl)
			continue;
		IAudioSessionControl2 *pCtrl2 = nullptr;
		if(SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&pCtrl2)) && pCtrl2)
		{
			DWORD Pid = 0;
			pCtrl2->GetProcessId(&Pid);
			if(IsTargetPid(Pid))
			{
				IAudioMeterInformation *pMeter = nullptr;
				if(SUCCEEDED(pCtrl2->QueryInterface(__uuidof(IAudioMeterInformation), (void **)&pMeter)) && pMeter)
				{
					g_aMeters[g_NumMeters].Pid = Pid;
					g_aMeters[g_NumMeters].pMeter = pMeter;
					++g_NumMeters;
				}
			}
			pCtrl2->Release();
		}
		pCtrl->Release();
	}
}

// 播放状态全局（EverLoud 策略）
static bool g_EverLoud = false;
static int g_DeafTicks = 0;
static int g_PeakLowStreak = 0;
static float g_deafPeak = 0.0f;

// ---------- WASAPI 进程环回采集（Win11 API）：精确"播放到哪一秒" ----------
static void LsDbgLog(const char *pTag, const char *pMsg); // 前向声明，实现见 LS 调试日志节
// 思路：ActivateAudioInterfaceAsync(PROCESS_LOOPBACK) 直接环回采集 cloudmusic 进程树渲染的音频帧。
// 帧计数÷采样率 = 真实已播放毫秒；暂停 = 渲染停止 = 帧流断（<300ms 无帧即暂停，亚秒级、零误判）。
// 静音帧也计入（flags=SILENT 仍推进渲染时钟），所以静音片段/音量0不影响位置推进。
static double g_EpochBase = 0.0;          // time(NULL) 锚点
static LARGE_INTEGER g_liEpochCounter = {0}; // 对应的 QPC 计数

static double NowEpochQpc()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	return g_EpochBase + (double)(li.QuadPart - g_liEpochCounter.QuadPart) / (double)g_qpcFreq.QuadPart;
}

struct SLoopStream
{
	DWORD Pid = 0;
	HANDLE hThread = NULL;
	volatile double Frames = 0.0;    // 累计帧数（含静音，渲染时间轴诊断用）
	volatile double ActiveFrames = 0.0; // 有效播放帧（暂停冻结，见下）
	volatile double Rate = 0.0;      // 采样率（帧/秒）
	volatile double LastFrameAt = 0.0; // 最近一包帧的 epoch 秒（含静音，诊断）
	volatile double LastNonSilentAt = 0.0; // 最近一个"有声"包的 epoch 秒
	volatile bool Dead = false;
};

static CRITICAL_SECTION g_LoopCS;
static bool g_LoopCSReady = false;
static std::vector<SLoopStream *> g_LoopStreams;
static bool g_HaveAudio = false; // 至少一条环回流激活成功（失败则回退电表/LS 判定）

class CActHandler final : public IActivateAudioInterfaceCompletionHandler
{
public:
	HANDLE m_hDone = NULL;
	IAudioClient *m_pClient = NULL;
	HRESULT m_hrOp = E_FAIL;
	CActHandler() { m_hDone = CreateEventW(NULL, TRUE, FALSE, NULL); }
	~CActHandler()
	{
		if(m_pClient)
			m_pClient->Release();
		if(m_hDone)
			CloseHandle(m_hDone);
	}
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
	{
		if(!ppv)
			return E_POINTER;
		if(IsEqualIID(riid, __uuidof(IUnknown)) ||
			IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler)) ||
			IsEqualIID(riid, __uuidof(IAgileObject)))
		{
			*ppv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }
	STDMETHODIMP_(ULONG) Release() override { return 1; }
	STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *pOperation) override
	{
		if(pOperation)
		{
			IUnknown *pUnk = NULL;
			if(SUCCEEDED(pOperation->GetActivateResult(&m_hrOp, &pUnk)) && pUnk)
			{
				pUnk->QueryInterface(__uuidof(IAudioClient), (void **)&m_pClient);
				pUnk->Release();
			}
		}
		SetEvent(m_hDone);
		return S_OK;
	}
};

static DWORD WINAPI LoopThread(void *pParam)
{
	SLoopStream *pS = (SLoopStream *)pParam;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IAudioClient *pClient = NULL;
	IAudioCaptureClient *pCap = NULL;
	WAVEFORMATEX *pwfx = NULL;
	HANDLE hEvent = NULL;

	AUDIOCLIENT_ACTIVATION_PARAMS Params;
	ZeroMemory(&Params, sizeof(Params));
	Params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	Params.ProcessLoopbackParams.TargetProcessId = pS->Pid;
	Params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
	PROPVARIANT Pv;
	PropVariantInit(&Pv);
	Pv.vt = VT_BLOB;
	Pv.blob.cbSize = sizeof(Params);
	Pv.blob.pBlobData = (BYTE *)&Params;

	CActHandler Handler;
	IActivateAudioInterfaceAsyncOperation *pOp = NULL;
	HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &Pv, &Handler, &pOp);
	if(pOp)
		pOp->Release();
	{
		char aMsg[160];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu ActivateAudioInterfaceAsync hr=0x%08lX", (unsigned long)pS->Pid, (unsigned long)hr);
		LsDbgLog("lb-act", aMsg);
	}
	if(SUCCEEDED(hr))
	{
		const DWORD Wr = WaitForSingleObject(Handler.m_hDone, 8000);
		hr = (Wr == WAIT_OBJECT_0) ? Handler.m_hrOp : E_FAIL;
		if(SUCCEEDED(hr) && !Handler.m_pClient)
			hr = E_FAIL;
		if(SUCCEEDED(hr))
		{
			pClient = Handler.m_pClient;
			Handler.m_pClient = NULL; // 所有权转移
		}
	}
	// 进程环回激活的 IAudioClient 不支持 GetMixFormat（实测 E_NOTIMPL）——
	// 必须显式给格式；系统会把目标进程音频混到该格式。用 float32 立体声 48kHz。
	if(SUCCEEDED(hr))
	{
		WAVEFORMATEX Fmt;
		ZeroMemory(&Fmt, sizeof(Fmt));
		Fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		Fmt.nChannels = 2;
		Fmt.nSamplesPerSec = 48000;
		Fmt.wBitsPerSample = 32;
		Fmt.nBlockAlign = Fmt.nChannels * Fmt.wBitsPerSample / 8;
		Fmt.nAvgBytesPerSec = Fmt.nSamplesPerSec * Fmt.nBlockAlign;
		hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
		hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			2000000, 0, &Fmt, NULL);
		{
			char aMsg[160];
			_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu Initialize hr=0x%08lX rate=%lu ch=%u", (unsigned long)pS->Pid, (unsigned long)hr, (unsigned long)Fmt.nSamplesPerSec, (unsigned)Fmt.nChannels);
			LsDbgLog("lb-init", aMsg);
		}
		if(SUCCEEDED(hr))
		{
			pS->Rate = (double)Fmt.nSamplesPerSec;
			hr = pClient->SetEventHandle(hEvent);
		}
		if(SUCCEEDED(hr))
			hr = pClient->GetService(__uuidof(IAudioCaptureClient), (void **)&pCap);
		{
			char aMsg[160];
			_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu SetEvent+GetService hr=0x%08lX", (unsigned long)pS->Pid, (unsigned long)hr);
			LsDbgLog("lb-svc", aMsg);
		}
		if(SUCCEEDED(hr))
			hr = pClient->Start();
		{
			char aMsg[160];
			_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu Start hr=0x%08lX", (unsigned long)pS->Pid, (unsigned long)hr);
			LsDbgLog("lb-start", aMsg);
		}
	}

	if(SUCCEEDED(hr) && pCap)
	{
		EnterCriticalSection(&g_LoopCS);
		g_HaveAudio = true;
		LeaveCriticalSection(&g_LoopCS);
		HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, pS->Pid);
		double StatsAt = NowEpochQpc();
		double StatEvents = 0, StatPolls = 0, StatPackets = 0, StatFrames = 0, StatGetBufFail = 0;
		double StatActiveFrames = 0, StatSilentDuringPlay = 0;
		for(;;)
		{
			// loopback 捕获的事件回调不可靠（实测不触发）—— 用 100ms 轮询拉包兜底
			const DWORD Wr = WaitForSingleObject(hEvent, 100);
			if(Wr == WAIT_OBJECT_0)
				++StatEvents;
			++StatPolls;
			for(;;)
			{
				UINT32 Packets = 0;
				if(FAILED(pCap->GetNextPacketSize(&Packets)))
					break;
				if(Packets == 0)
					break;
				++StatPackets;
				BYTE *pData = NULL;
				UINT32 Frames = 0;
				DWORD Flags = 0;
				if(FAILED(pCap->GetBuffer(&pData, &Frames, &Flags, NULL, NULL)))
				{
					++StatGetBufFail;
					break;
				}
				if(Frames)
				{
					const double NowEp = NowEpochQpc();
					// 网易云暂停时仍投递采样帧,且**不带 SILENT 标志**(实测:暂停中 nonSilent=全部帧,
					// 电表 Peak=0)。Flags 不可靠 —— 直接算样本峰值判"有声":float32 立体声 48k 是
					// 我们 Initialize 指定的格式,pData 即 float 数组。阈值 1e-4(约 -80dBFS)。
					bool Silent;
					if(Flags & AUDCLNT_BUFFERFLAGS_SILENT)
						Silent = true;
					else
					{
						const float *pF = (const float *)pData;
						const UINT32 N = Frames * 2u; // 2 channels
						float Peak = 0.0f;
						for(UINT32 k = 0; k < N; ++k)
						{
							const float a = pF[k] < 0.0f ? -pF[k] : pF[k];
							if(a > Peak)
								Peak = a;
						}
						Silent = Peak < 1e-4f;
					}
					pS->Frames += (double)Frames;
					pS->LastFrameAt = NowEp;
					StatFrames += Frames;
					// 有声包：推进有效播放帧 ActiveFrames（= 精确"播到哪一秒"），刷新 LastNonSilentAt；
					// 静音包：仅当距上一个有声包 <0.3s（播放中的静音片段）才推进 ActiveFrames；
					// 超过 0.3s 的静音 = 暂停，ActiveFrames 冻结（AudioMs 随之冻结）。
					if(!Silent)
					{
						pS->ActiveFrames += (double)Frames;
						pS->LastNonSilentAt = NowEp;
						StatActiveFrames += Frames;
					}
					else if(NowEp - pS->LastNonSilentAt < 0.3)
					{
						pS->ActiveFrames += (double)Frames;
						StatSilentDuringPlay += Frames;
					}
				}
				pCap->ReleaseBuffer(Frames);
			}
			// 每 5 秒打一条采集统计（tag 节流同样生效）
			const double NowEp = NowEpochQpc();
			if(NowEp - StatsAt > 5.0)
			{
				StatsAt = NowEp;
				char aMsg[220];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu stats: events=%.0f polls=%.0f pkts=%.0f frames=%.0f activeMs=%.0f nonSilent=%.0f silentInPlay=%.0f totalMs=%.0f getbufFail=%.0f",
					(unsigned long)pS->Pid, StatEvents, StatPolls, StatPackets, StatFrames,
					pS->ActiveFrames * 1000.0 / pS->Rate, StatActiveFrames, StatSilentDuringPlay,
					pS->Frames * 1000.0 / pS->Rate, StatGetBufFail);
				LsDbgLog("lb-stat", aMsg);
				StatEvents = StatPolls = StatPackets = StatFrames = 0;
				StatActiveFrames = StatSilentDuringPlay = 0;
			}
			// 进程退出 → 结束采集
			if(hProc)
			{
				if(WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0)
					break;
			}
			else
			{
				HANDLE Chk = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pS->Pid);
				if(!Chk)
					break;
				CloseHandle(Chk);
			}
			Sleep(20);
		}
	}

	if(SUCCEEDED(hr))
	{
		char aMsg[160];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu capture RUNNING rate=%.0f", (unsigned long)pS->Pid, pS->Rate);
		LsDbgLog("lb-run", aMsg);
	}
	else
	{
		char aMsg[160];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu FAILED hr=0x%08lX", (unsigned long)pS->Pid, (unsigned long)hr);
		LsDbgLog("lb-fail", aMsg);
	}

	if(pCap)
		pCap->Release();
	if(pClient)
		pClient->Release();
	if(pwfx)
		CoTaskMemFree(pwfx);
	if(hEvent)
		CloseHandle(hEvent);
	EnterCriticalSection(&g_LoopCS);
	pS->Dead = true;
	LeaveCriticalSection(&g_LoopCS);
	CoUninitialize();
	return 0;
}

// 依据根进程（父进程不是 cloudmusic 的）建流；网易云重启换 PID 时自动重建
static void ManageLoopStreams()
{
	if(!g_LoopCSReady)
		return;
	EnterCriticalSection(&g_LoopCS);
	for(size_t i = 0; i < g_LoopStreams.size();)
	{
		if(g_LoopStreams[i]->Dead)
		{
			if(g_LoopStreams[i]->hThread)
				CloseHandle(g_LoopStreams[i]->hThread);
			delete g_LoopStreams[i];
			g_LoopStreams.erase(g_LoopStreams.begin() + i);
		}
		else
			++i;
	}
	for(int i = 0; i < g_NumPids; ++i)
	{
		const DWORD Pid = g_aPids[i];
		const DWORD Parent = g_aPidParents[i];
		bool IsRoot = true;
		for(int k = 0; k < g_NumPids; ++k)
			if(g_aPids[k] == Parent)
			{
				IsRoot = false;
				break;
			}
		if(!IsRoot)
			continue;
		bool Exists = false;
		for(SLoopStream *pS : g_LoopStreams)
			if(pS->Pid == Pid)
			{
				Exists = true;
				break;
			}
		if(Exists)
			continue;
		SLoopStream *pS = new SLoopStream();
		pS->Pid = Pid;
		pS->hThread = CreateThread(NULL, 0, LoopThread, pS, 0, NULL);
		if(pS->hThread)
		{
			g_LoopStreams.push_back(pS);
			char aMsg[96];
			_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] spawn thread for pid=%lu", (unsigned long)Pid);
			LsDbgLog("lb-spawn", aMsg);
		}
		else
			delete pS;
	}
	LeaveCriticalSection(&g_LoopCS);
}

// ---------- 网易云 LS leveldb 直读（playPostion / pauseStatus，seek 立即同步） ----------
static wchar_t g_wszLSFile[MAX_PATH * 2] = L"";
static FILETIME g_LsLastWrite = {0, 0};
static double g_LsFraction = 0.0;   // playPostion 0..1
static double g_LsCapturedAt = 0.0; // epoch seconds
static bool g_LsPaused = false;
static bool g_LsHave = false;
static double g_LsDurationMs = 0.0; // from lastPlayInfoForRestore duration (seconds->ms)
static double g_AudioCorrMs = 0.0;  // TClient: 音频钟的 LS 锚定校正值（环回漏帧/中途激活导致音频钟落后时拉回）
// 进度活性锚点：playPostion 值最近一次"确实变了"的时刻。播放中网易云秒级持续写，
// 暂停/停止后 LS 冻结 —— 这是电表失灵场景下唯一可靠的暂停信号（新版网易云 LS 里没有 pauseStatus）。
static double g_LsLastProgressAt = 0.0;
static double g_LsLastFraction = -1.0;
static double g_LsLastReanchorAt = 0.0; // 上次音频钟重锚时刻（8 秒冷却防循环）

// ---------- 诊断日志（带尺寸上限 + 节流）----------
// 主循环 50ms 一轮，原来每轮都往日志里追加两行，实测约 177MB/天，
// C:\TClientLS_dbg.txt 长到 124MB 就是这么来的。现在：
//   1) 同类消息每 LS_DBG_MIN_INTERVAL_SEC 秒最多写一条（按 tag 节流）；
//   2) 文件超过 LS_DBG_MAX_BYTES 直接删掉重来，绝不无限增长。
#define LS_DBG_MAX_BYTES (8 * 1024 * 1024)
#define LS_DBG_MIN_INTERVAL_SEC 30

static void LsDbgLog(const char *pTag, const char *pMsg)
{
	static const char *s_apTags[8] = {0};
	static time_t s_aLast[8] = {0};
	static int s_NumTags = 0;
	const time_t Now = time(NULL);

	int Slot = -1;
	for(int i = 0; i < s_NumTags; ++i)
	{
		if(s_apTags[i] == pTag || (pTag && s_apTags[i] && strcmp(s_apTags[i], pTag) == 0))
		{
			Slot = i;
			break;
		}
	}
	if(Slot < 0 && s_NumTags < 8)
	{
		Slot = s_NumTags++;
		s_apTags[Slot] = pTag;
	}
	if(Slot >= 0)
	{
		if(Now - s_aLast[Slot] < LS_DBG_MIN_INTERVAL_SEC)
			return;
		s_aLast[Slot] = Now;
	}

	WIN32_FILE_ATTRIBUTE_DATA Fad;
	if(GetFileAttributesExA("C:\\TClientLS_dbg.txt", GetFileExInfoStandard, &Fad))
	{
		const ULONGLONG Size = ((ULONGLONG)Fad.nFileSizeHigh << 32) | Fad.nFileSizeLow;
		if(Size > LS_DBG_MAX_BYTES)
			DeleteFileA("C:\\TClientLS_dbg.txt");
	}

	FILE *pDbg = fopen("C:\\TClientLS_dbg.txt", "a");
	if(!pDbg)
		return;
	fprintf(pDbg, "%s\n", pMsg);
	fclose(pDbg);
}

// 在原始字节里找最后一次出现的 needle
static bool FindLast(const uint8_t *p, size_t n, const char *pNeedle, size_t nNeedle, size_t &out)
{
	if(n < nNeedle || nNeedle == 0)
		return false;	for(size_t i = n - nNeedle + 1; i-- > 0;)
	{
		if(memcmp(p + i, pNeedle, nNeedle) == 0)
		{
			out = i;
			return true;
		}
	}
	return false;
}

// 从一段原始字节里找第一次出现的 needle
static bool FindFirst(const uint8_t *p, size_t n, const char *pNeedle, size_t nNeedle, size_t &out)
{
	if(n < nNeedle || nNeedle == 0)
		return false;
	for(size_t i = 0; i + nNeedle <= n; ++i)
	{
		if(memcmp(p + i, pNeedle, nNeedle) == 0)
		{
			out = i;
			return true;
		}
	}
	return false;
}

static double FileTimeToEpochDouble(const FILETIME &Ft)
{
	// 100ns units since 1601; epoch = -116444736000000000
	const ULONGLONG Parts = ((ULONGLONG)Ft.dwHighDateTime << 32) | Ft.dwLowDateTime;
	return (double)((__int64)Parts - 116444736000000000i64) / 10000000.0;
}

static void ParseLSFile(const wchar_t *pPath, const FILETIME *pWrite)
{
	const double ReadingNow = (double)time(nullptr);
	HANDLE h = CreateFileW(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if(h == INVALID_HANDLE_VALUE)
		return;
	DWORD Hi = 0;
	DWORD Size = GetFileSize(h, &Hi);
	if(Size == INVALID_FILE_SIZE || (Hi != 0 && Size > (32u << 20)))
	{
		CloseHandle(h);
		return;
	}
	uint8_t *pBuf = (uint8_t *)malloc(Size ? Size : 1);
	if(!pBuf)
	{
		CloseHandle(h);
		return;
	}
	DWORD Got = 0;
	BOOL Ok = ReadFile(h, pBuf, Size, &Got, NULL);
	CloseHandle(h);
	if(!Ok || Got == 0)
	{
		free(pBuf);
		return;
	}

	size_t Pos = 0;
	// 诊断：这条文件里到底有没有 lastPlaying / playPostion（节流后写入，见 LsDbgLog）
	{
		const int HasLp = FindLast(pBuf, Got, "lastPlaying", 11, Pos) ? 1 : 0;
		size_t TmpFp = 0;
		const int HasPp = FindLast(pBuf, Got, "playPostion", 11, TmpFp) ? 1 : 0;
		char aMsg[1024];
		_snprintf(aMsg, sizeof(aMsg), "parse file=%S size=%u g_LsHave=%d frac=%.4f lpHit=%d ppHit=%d",
			pPath, (unsigned)Got, g_LsHave ? 1 : 0, g_LsFraction, HasLp, HasPp);
		LsDbgLog("parse", aMsg);
	}
	// 在整份文件里找"最近一条真正记录了播放位置的 lastPlaying"。
	// 坑：网易云切歌/暂停/停止时会写 {"lastPlaying":{"songId":"...","playPostion":0}} 这种 0 值记录
	// （还有的记录压根不带 playPostion）。如果无脑取最后一个 lastPlaying，就会拿到 playPostion=0，
	// 于是拖进度/切歌后显示 0 —— 这正是用户反馈的"拖动没反应/一直回退"的主因。
	// 做法：从后往前遍历每一条 lastPlaying，取第一个带 playPostion 且值在 (0,1] 的记录，
	// 但只允许它比已选记录新（按字符串 offset）；若全是 0 则回退到 0。
	size_t BestRec = (size_t)-1;
	double BestFrac = 0.0;
	{
		// 收集所有 lastPlaying 的位置
		std::vector<size_t> vPos;
		for(size_t i = 0; i + 11 <= Got; ++i)
			if(memcmp(pBuf + i, "lastPlaying", 11) == 0)
				vPos.push_back(i);
		for(size_t k = 0; k < vPos.size(); ++k)
		{
			const size_t p0 = vPos[k];
			const size_t p1 = (k + 1 < vPos.size()) ? vPos[k + 1] : Got;
			const size_t RecEnd = std::min<size_t>(Got, p0 + 400);
			size_t Fp;
			if(!FindFirst(pBuf + p0, RecEnd - p0, "playPostion", 11, Fp))
				continue; // 这条没带 playPostion，跳过
			const uint8_t *pVal = pBuf + p0 + Fp + 11;
			while(pVal < pBuf + RecEnd && *pVal != ':')
				++pVal;
			if(pVal >= pBuf + RecEnd || *pVal != ':')
				continue;
			const double Frac = strtod((const char *)(pVal + 1), nullptr);
			if(Frac > 0.0 && Frac <= 1.0)
			{
				BestRec = p0;
				BestFrac = Frac;
			}
			else if(BestRec == (size_t)-1)
			{
				// 先记住一个 0 值兜底，但后面若有 >0 的会被覆盖
				BestRec = p0;
				BestFrac = Frac;
			}
			(void)p1;
		}
	}
	if(BestRec != (size_t)-1)
	{
		Pos = BestRec;
		const size_t RecEnd = std::min<size_t>(Got, Pos + 400);
		g_LsFraction = BestFrac;
		size_t Fs;
		if(FindFirst(pBuf + Pos, RecEnd - Pos, "pauseStatus", 11, Fs))
		{
			const uint8_t *pVal = pBuf + Pos + Fs + 11;
			const char *pChk = (const char *)pVal;
			for(size_t k = 0; k < 16 && (pVal + k) < (pBuf + RecEnd); ++k)
			{
				if(pChk[k] == 't' || pChk[k] == 'T')
				{
					g_LsPaused = true;
					break;
				}
				if(pChk[k] == 'f' || pChk[k] == 'F')
				{
					g_LsPaused = false;
					break;
				}
			}
		}
		g_LsHave = true;
		// duration 来自同文件的 lastPlayInfoForRestore：{"playInfo":{...,"duration":219.86,...}}（秒）。
		// 有了它客户端才能用 playPostion（0..1）换算出真实秒数，否则拖动/切歌后位置全是 0。
		{
			size_t Dr = 0;
			if(FindLast(pBuf, Got, "lastPlayInfoForRestore", 22, Dr))
			{
				size_t Fd;
				if(FindFirst(pBuf + Dr, std::min<size_t>(Got - Dr, 400), "duration", 8, Fd))
				{
					const uint8_t *pVal = pBuf + Dr + Fd + 8;
					const uint8_t *pEnd = pBuf + Got;
					while(pVal < pEnd && *pVal != ':')
						++pVal;
					if(pVal < pEnd && *pVal == ':')
					{
						const double Secs = strtod((const char *)(pVal + 1), nullptr);
						if(Secs > 1.0 && Secs < 3600.0 * 6.0)
							g_LsDurationMs = Secs * 1000.0;
					}
				}
			}
		}
		// 用"读到这条记录的墙钟时间"作为锚点时刻，而不是 lodb 文件的 mtime。
		// leveldb 会压缩/滚动日志，旧 mtime 可能比实际写入晚好几秒，导致客户端算出的 Age 偏小、
		// 进度整体落后（用户反馈的"从头开始都有 3 秒延迟"就是这里）。读到即现在，误差只有一个扫描周期。
		// 进度活性：只认"playPostion 值真的变了"。暂停时网易云仍会以同值重写 LS（mtime 会动），
		// 所以文件写入变化不算进度 —— 否则活性判定被同值重写骗过，暂停后岛照走（§174 失败根因）。
		if(g_LsFraction != g_LsLastFraction)
			g_LsLastProgressAt = ReadingNow;
		g_LsLastFraction = g_LsFraction;
		g_LsCapturedAt = ReadingNow;
		g_LsLastWrite = *pWrite;
		wcsncpy(g_wszLSFile, pPath, MAX_PATH * 2 - 1);
	}
	free(pBuf);
}

static void ScanLS()
{
	// 候选目录：云音乐安装目录/webapp91 与 其父一级的 NetEase 路径，以及 %LOCALAPPDATA% 同路径
	wchar_t awDirs[8][MAX_PATH * 2];
	int nDirs = 0;

	// 通配 webapp*：网易云新版(3.1.42+)把 web 目录改名成 webapp91x64，旧版仍是 webapp91。
	// 不能写死目录名，直接按前缀枚举父目录下所有 webapp* 子目录。
	auto AddDir = [&awDirs, &nDirs](const wchar_t *pBase) {
		// 形式1: pBase\webapp*\Local Storage\leveldb
		if(nDirs < 8)
		{
			wchar_t wGlob[MAX_PATH * 2];
			_snwprintf(wGlob, MAX_PATH * 2 - 1, L"%s\\webapp*", pBase);
			WIN32_FIND_DATAW Fd;
			HANDLE hFind = FindFirstFileW(wGlob, &Fd);
			if(hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					if((Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nDirs < 8)
					{
						_snwprintf(awDirs[nDirs], MAX_PATH * 2 - 1, L"%s\\%s\\Local Storage\\leveldb", pBase, Fd.cFileName);
						++nDirs;
					}
				} while(FindNextFileW(hFind, &Fd));
				FindClose(hFind);
			}
		}
		// 形式2: pBase\NetEase\CloudMusic\webapp*\Local Storage\leveldb
		if(nDirs < 8)
		{
			wchar_t wSub[MAX_PATH * 2];
			_snwprintf(wSub, MAX_PATH * 2 - 1, L"%s\\NetEase\\CloudMusic", pBase);
			wchar_t wGlob[MAX_PATH * 2];
			_snwprintf(wGlob, MAX_PATH * 2 - 1, L"%s\\webapp*", wSub);
			WIN32_FIND_DATAW Fd;
			HANDLE hFind = FindFirstFileW(wGlob, &Fd);
			if(hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					if((Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nDirs < 8)
					{
						_snwprintf(awDirs[nDirs], MAX_PATH * 2 - 1, L"%s\\%s\\Local Storage\\leveldb", wSub, Fd.cFileName);
						++nDirs;
					}
				} while(FindNextFileW(hFind, &Fd));
				FindClose(hFind);
			}
		}
	};

	// 进程安装目录（多级尝试，装在世界各地都可能）
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(hSnap != INVALID_HANDLE_VALUE)
	{
		PROCESSENTRY32W Pe;
		Pe.dwSize = sizeof(Pe);
		if(Process32FirstW(hSnap, &Pe))
		{
			do
			{
				if(_wcsicmp(Pe.szExeFile, L"cloudmusic.exe") == 0 && nDirs < 4)
				{
					HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, Pe.th32ProcessID);
					if(hProc)
					{
						wchar_t wPath[MAX_PATH * 2] = L"";
						DWORD Sz = MAX_PATH * 2;
						if(QueryFullProcessImageNameW(hProc, 0, wPath, &Sz))
						{
							wchar_t *pSlash = wcsrchr(wPath, L'\\');
							if(pSlash)
							{
								*pSlash = 0;
								AddDir(wPath); // exe 目录 + 其 NetEase 子目录
							}
						}
						CloseHandle(hProc);
					}
				}
			} while(Process32NextW(hSnap, &Pe));
		}
		CloseHandle(hSnap);
	}
	wchar_t wLocal[MAX_PATH];
	if(SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, wLocal)))
		AddDir(wLocal);

	for(int d = 0; d < nDirs; ++d)
	{
		const wchar_t *pDir = awDirs[d];
		wchar_t wGlob[MAX_PATH * 2];
		_snwprintf(wGlob, MAX_PATH * 2 - 1, L"%s\\*.log", pDir);
		WIN32_FIND_DATAW Fd;
		HANDLE hFind = FindFirstFileW(wGlob, &Fd);
		{
			char aMsg[1024];
			_snprintf(aMsg, sizeof(aMsg), "scan dir=%S find=%d", pDir, hFind != INVALID_HANDLE_VALUE ? 1 : 0);
			LsDbgLog("scan", aMsg);
		}
		if(hFind == INVALID_HANDLE_VALUE)
			continue;
		// 先扫一遍目录，找 mtime 最新的那个文件（网易云会滚动日志 / 压缩 .ldb，
		// 最新的 playPostion 一定在 mtime 最新的文件里；之前只按"比上次新就解析"会读到旧的 .ldb，
		// 结果 frac 是上一首的旧进度 → 用户拖进度/切歌对不上就是这个）。只看最新文件，省事又准。
		wchar_t wBestFile[MAX_PATH * 2] = L"";
		FILETIME BestWrite = {0, 0};
		__int64 BestTicks = -1;
		do
		{
			if(Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const ULONGLONG Size = ((ULONGLONG)Fd.nFileSizeHigh << 32) | Fd.nFileSizeLow;
			if(Size == 0 || Size > (32u << 20))
				continue;
			const __int64 ThisWrite = ((__int64)Fd.ftLastWriteTime.dwHighDateTime << 32) | Fd.ftLastWriteTime.dwLowDateTime;
			if(ThisWrite > BestTicks)
			{
				BestTicks = ThisWrite;
				BestWrite = Fd.ftLastWriteTime;
				_snwprintf(wBestFile, MAX_PATH * 2 - 1, L"%s\\%s", pDir, Fd.cFileName);
			}
		} while(FindNextFileW(hFind, &Fd));
		FindClose(hFind);
		if(!wBestFile[0])
			continue;
		// 最新文件里如果有 lastPlaying 就解析（一次性取到最新进度）；没有就跳过，等下次刷写
		ParseLSFile(wBestFile, &BestWrite);
	}
}

static float PollAudioPeak()
{
	float Peak = 0.0f;
	for(int i = 0; i < g_NumMeters; ++i)
	{
		float f = 0.0f;
		if(SUCCEEDED(g_aMeters[i].pMeter->GetPeakValue(&f)) && f > Peak)
			Peak = f;
	}
	return Peak;
}

// ---------- CDP（网易云 CEF 远程调试）：读界面播放时间，seek 立即反映 ----------
// 新版网易云 LS 值全加密（lastPlaying/playingInfo base64+二次加密），playPostion 不再可读；
// SMTC Timeline 全零。唯一可靠的外部锚点是网易云自身的 CEF 远程调试端口（默认开）：
// GET /json/list 拿页面 WS → Runtime.evaluate 读界面时间元素文本 "MM:SS / MM:SS"。
// 不依赖窗口可见（托盘/最小化都读得到），桌面歌词常开场景实测可用。
static std::atomic<double> g_CdpCurSec{-1.0};
static std::atomic<double> g_CdpTotalSec{-1.0};
static std::atomic<double> g_CdpLastOkAt{0.0};   // 最近一次成功读取(epoch 秒)
static std::atomic<uint32_t> g_CdpHasData{0};

// 枚举 TCP 监听表，找 cloudmusic PID 们监听的 127.0.0.1 端口（CEF 调试端口随启动变化，须动态发现）
static bool FindCdpPorts(uint16_t *aOut, int &nOut)
{
	nOut = 0;
	DWORD Size = 0;
	if(GetExtendedTcpTable(NULL, &Size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
		return false;
	std::vector<uint8_t> Buf(Size);
	if(GetExtendedTcpTable(Buf.data(), &Size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
		return false;
	const MIB_TCPTABLE_OWNER_PID *pTbl = (const MIB_TCPTABLE_OWNER_PID *)Buf.data();
	for(DWORD i = 0; i < pTbl->dwNumEntries && nOut < 8; ++i)
	{
		const MIB_TCPROW_OWNER_PID &r = pTbl->table[i];
		const uint32_t Remote = ntohl(r.dwRemoteAddr);
		if(Remote != 0) // LISTEN 行 remote=0.0.0.0
			continue;
		bool IsCloud = false;
		for(int k = 0; k < g_NumPids; ++k)
			if(g_aPids[k] == r.dwOwningPid)
			{
				IsCloud = true;
				break;
			}
		if(!IsCloud)
			continue;
		const uint16_t Port = (uint16_t)ntohs((uint16_t)(r.dwLocalPort & 0xFFFF));
		bool Dup = false;
		for(int k = 0; k < nOut; ++k)
			if(aOut[k] == Port)
			{
				Dup = true;
				break;
			}
		if(!Dup)
			aOut[nOut++] = Port;
	}
	{
		char aMsg[128];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] found ports: n=%d %u %u %u", nOut,
			nOut > 0 ? aOut[0] : 0, nOut > 1 ? aOut[1] : 0, nOut > 2 ? aOut[2] : 0);
		LsDbgLog("cdp-dp", aMsg);
	}
	return nOut > 0;
}

// 非阻塞 connect + select 超时（CEF backlog 满时阻塞 connect 会卡 21 秒）
static bool ConnectLocal(SOCKET s, uint16_t Port, int TimeoutMs)
{
	u_long Nb = 1;
	ioctlsocket(s, FIONBIO, &Nb);
	sockaddr_in sa;
	ZeroMemory(&sa, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(Port);
	sa.sin_addr.s_addr = htonl(0x7F000001);
	const int Cr = connect(s, (sockaddr *)&sa, sizeof(sa));
	bool Ok = Cr == 0;
	if(!Ok)
	{
		const int Err = WSAGetLastError();
		if(Err == WSAEWOULDBLOCK)
		{
			fd_set W;
			FD_ZERO(&W);
			FD_SET(s, &W);
			timeval Tv = {TimeoutMs / 1000, (TimeoutMs % 1000) * 1000};
			Ok = select(0, NULL, &W, NULL, &Tv) > 0;
			if(Ok)
			{
				int Serr = 0;
				int Sl = sizeof(Serr);
				getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&Serr, &Sl);
				Ok = Serr == 0;
			}
		}
	}
	Nb = 0;
	ioctlsocket(s, FIONBIO, &Nb);
	if(!Ok)
	{
		closesocket(s);
		return false;
	}
	DWORD Tv2 = 3000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&Tv2, sizeof(Tv2));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&Tv2, sizeof(Tv2));
	return true;
}

// 极简 HTTP GET（HTTP/1.0 短连接），返回 body
static bool HttpGetPort(uint16_t Port, const char *pPath, std::string &Out)
{
	const double __t0 = NowEpochQpc();
	LsDbgLog("cdp-hg", "[cdp] HttpGet enter");

	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if(s == INVALID_SOCKET)
		return false;
	// 2 秒收发超时：CEF 对 HTTP/1.0 请求也可能保持连接不断开，
	// 无超时会卡死在 recv 等 EOF（body 其实早已收完）——超时后返回已收内容即可
	if(!ConnectLocal(s, Port, 2000))
		return false;
	char Req[256];
	_snprintf(Req, sizeof(Req) - 1, "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", pPath);
	if(send(s, Req, (int)strlen(Req), 0) <= 0)
	{
		closesocket(s);
		return false;
	}
	Out.clear();
	char Rbuf[4096];
	for(;;)
	{
		if(NowEpochQpc() - __t0 > 3.0)
		{
			LsDbgLog("cdp-hg", "[cdp] HttpGet hard timeout");
			break;
		}
		const int N = recv(s, Rbuf, sizeof(Rbuf), 0);
		if(N <= 0)
			break;
		Out.append(Rbuf, N);
		if(Out.size() > (1u << 20))
			break;
	}
	closesocket(s);
	const size_t Pos = Out.find("\r\n\r\n");
	if(Pos == std::string::npos)
		return false;
	Out.erase(0, Pos + 4);
	{
		char aMsg[96];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] HttpGet done %.3fs len=%u", NowEpochQpc() - __t0, (unsigned)Out.size());
		LsDbgLog("cdp-hg", aMsg);
	}
	return Out.size() > 0;
}

static bool WsSendAll(SOCKET s, const char *pData, int Len)
{
	int Off = 0;
	while(Off < Len)
	{
		const int N = send(s, pData + Off, Len - Off, 0);
		if(N <= 0)
			return false;
		Off += N;
	}
	return true;
}

static bool WsRecvAll(SOCKET s, uint8_t *pBuf, int Len)
{
	int Off = 0;
	while(Off < Len)
	{
		const int N = recv(s, (char *)pBuf + Off, Len - Off, 0);
		if(N <= 0)
			return false;
		Off += N;
	}
	return true;
}

// 读一个 WS 帧（文本；ping/pong 忽略返回空串；evaluate 响应单帧足够）
static bool WsReadTextFrame(SOCKET s, std::string &Out)
{
	uint8_t Hdr[2];
	if(!WsRecvAll(s, Hdr, 2))
		return false;
	const uint8_t Opcode = Hdr[0] & 0x0F;
	uint64_t Len = Hdr[1] & 0x7F;
	if(Len == 126)
	{
		uint8_t Ex[2];
		if(!WsRecvAll(s, Ex, 2))
			return false;
		Len = ((uint64_t)Ex[0] << 8) | Ex[1];
	}
	else if(Len == 127)
	{
		uint8_t Ex[8];
		if(!WsRecvAll(s, Ex, 8))
			return false;
		Len = 0;
		for(int k = 0; k < 8; ++k)
			Len = (Len << 8) | Ex[k];
	}
	if(Len > (4u << 20))
		return false;
	std::vector<uint8_t> Payload((size_t)Len);
	if(Len && !WsRecvAll(s, Payload.data(), (int)Len))
		return false;
	if(Opcode == 0x9 || Opcode == 0xA)
	{
		Out.clear();
		return true; // ping/pong：空消息，调用方循环继续
	}
	Out.assign((const char *)Payload.data(), Payload.size());
	return true;
}

// 连接 CDP 页面 WS 并执行一次 evaluate，返回 value 字符串
static bool CdpEvaluate(uint16_t Port, const char *pPageId, const char *pExpr, std::string &ValueOut)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if(s == INVALID_SOCKET)
		return false;
	if(!ConnectLocal(s, Port, 2000))
		return false;
	char Path[160];
	_snprintf(Path, sizeof(Path) - 1, "/devtools/page/%s", pPageId);
	char Key[40];
	{
		uint8_t Raw[16];
		for(int k = 0; k < 16; ++k)
			Raw[k] = (uint8_t)(rand() & 0xFF);
		static const char *pB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		int o = 0;
		for(int k = 0; k < 16; k += 3)
		{
			const uint32_t V = ((uint32_t)Raw[k] << 16) | ((k + 1 < 16) ? ((uint32_t)Raw[k + 1] << 8) : 0) | ((k + 2 < 16) ? Raw[k + 2] : 0);
			Key[o++] = pB64[(V >> 18) & 63];
			Key[o++] = pB64[(V >> 12) & 63];
			Key[o++] = (k + 1 < 16) ? pB64[(V >> 6) & 63] : '=';
			Key[o++] = (k + 2 < 16) ? pB64[V & 63] : '=';
		}
		Key[o] = 0;
	}
	char Req[512];
	_snprintf(Req, sizeof(Req) - 1,
		"GET %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", Path, Port, Key);
	if(!WsSendAll(s, Req, (int)strlen(Req)))
	{
		closesocket(s);
		return false;
	}
	std::string Hdr;
	char Rbuf[4096];
	size_t WsStart = std::string::npos; // 握手包里粘带的 WS 帧起点
	for(;;)
	{
		const int N = recv(s, Rbuf, sizeof(Rbuf), 0);
		if(N <= 0)
		{
			closesocket(s);
			return false;
		}
		Hdr.append(Rbuf, N);
		const size_t Eoh = Hdr.find("\r\n\r\n");
		if(Eoh != std::string::npos)
		{
			WsStart = Eoh + 4;
			break;
		}
		if(Hdr.size() > 65536)
			break;
	}
	if(Hdr.find(" 101 ") == std::string::npos)
	{
		closesocket(s);
		return false;
	}
	// 粘包：握手包里可能已带首个 WS 帧的开头，先缓存起来
	std::string WsBuf = (WsStart != std::string::npos) ? Hdr.substr(WsStart) : "";

	char Msg[1024];
	_snprintf(Msg, sizeof(Msg) - 1,
		"{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"%s\",\"returnByValue\":true}}", pExpr);
	std::string Frame;
	Frame.push_back((char)0x81); // FIN + text
	const size_t Len = strlen(Msg);
	if(Len < 126)
		Frame.push_back((char)(0x80 | Len));
	else
	{
		Frame.push_back((char)(0x80 | 126));
		Frame.push_back((char)((Len >> 8) & 0xFF));
		Frame.push_back((char)(Len & 0xFF));
	}
	uint8_t Mask[4] = {0x11, 0x22, 0x33, 0x44};
	Frame.append((const char *)Mask, 4);
	for(size_t k = 0; k < Len; ++k)
		Frame.push_back(Msg[k] ^ Mask[k % 4]);
	if(!WsSendAll(s, Frame.data(), (int)Frame.size()))
	{
		closesocket(s);
		return false;
	}
	const double EvalStart = NowEpochQpc();
	for(;;)
	{
		if(NowEpochQpc() - EvalStart > 5.0)
		{
			// 5 秒兜底：粘包错位/事件洪流时放弃本轮（下轮重连）
			LsDbgLog("cdp-to", "[cdp] evaluate timeout");
			closesocket(s);
			return false;
		}
		std::string Text;
		// 先消费粘包缓冲，空了再收
		while(!WsBuf.empty())
		{
			// 逐步在 WsBuf 里切帧：最少 2 字节头
			if(WsBuf.size() < 2)
				break;
			const uint8_t H0 = (uint8_t)WsBuf[0];
			const uint8_t H1 = (uint8_t)WsBuf[1];
			uint64_t L = H1 & 0x7F;
			size_t Hl = 2;
			if(L == 126)
			{
				if(WsBuf.size() < 4)
					break;
				L = ((uint64_t)(uint8_t)WsBuf[2] << 8) | (uint8_t)WsBuf[3];
				Hl = 4;
			}
			else if(L == 127)
			{
				if(WsBuf.size() < 10)
					break;
				L = 0;
				for(int k = 2; k < 10; ++k)
					L = (L << 8) | (uint8_t)WsBuf[k];
				Hl = 10;
			}
			if(L > (4u << 20))
			{
				WsBuf.clear();
				break;
			}
			if(WsBuf.size() < Hl + L)
				break; // 帧不完整，去收
			if((H0 & 0x0F) == 0x1 && L > 0)
				Text.append(WsBuf, Hl, (size_t)L);
			WsBuf.erase(0, Hl + (size_t)L);
			if((H0 & 0x80) && !Text.empty())
				break; // FIN 且有内容
		}
		if(Text.empty())
		{
			char Rb[8192];
			const int N = recv(s, Rb, sizeof(Rb), 0);
			if(N <= 0)
			{
				closesocket(s);
				return false;
			}
			WsBuf.append(Rb, N);
			continue;
		}
		const size_t Vp = Text.find("\"value\":\"");
		if(Vp == std::string::npos)
			continue; // 事件帧：继续读
		size_t B = Vp + 9;
		ValueOut.clear();
		for(; B < Text.size(); ++B)
		{
			if(Text[B] == '"' && Text[B - 1] != '\\')
				break;
			if(Text[B] == '\\' && B + 1 < Text.size())
			{
				++B;
				ValueOut.push_back(Text[B] == 'n' ? '\n' : Text[B]);
				continue;
			}
			ValueOut.push_back(Text[B]);
		}
		break;
	}
	closesocket(s);
	return true;
}

static bool ParseTimeText(const std::string &Text, double &CurSec, double &TotalSec)
{
	// "01:44 / 03:26"（去空格后找 '/'）
	std::string C;
	for(char Ch : Text)
		if(Ch != ' ' && Ch != '\t' && Ch != '\n' && Ch != '\r')
			C.push_back(Ch);
	const size_t Sp = C.find('/');
	if(Sp == std::string::npos || Sp == 0 || Sp + 1 >= C.size())
		return false;
	auto ToSec = [](const std::string &S, double &Out) {
		int Colon = -1;
		for(int k = (int)S.size() - 1; k >= 0; --k)
			if(S[k] == ':')
			{
				Colon = k;
				break;
			}
		if(Colon < 0)
			return false;
		const int M = atoi(S.substr(0, (size_t)Colon).c_str());
		const int Snd = atoi(S.substr((size_t)Colon + 1).c_str());
		if(M < 0 || Snd < 0 || Snd > 59)
			return false;
		Out = M * 60.0 + Snd;
		return true;
	};
	double C1 = -1, T1 = -1;
	if(!ToSec(C.substr(0, Sp), C1) || !ToSec(C.substr(Sp + 1), T1))
		return false;
	if(T1 <= 0.0 || C1 < 0.0 || C1 > T1 + 2.0)
		return false;
	CurSec = C1;
	TotalSec = T1;
	return true;
}

static DWORD WINAPI CdpThread(LPVOID)
{
	WSADATA Wsa;
	WSAStartup(MAKEWORD(2, 2), &Wsa);
	srand((unsigned)time(NULL));
	const char *pExpr =
		"(function(){var e=document.querySelector('[class*=\"curtime\"]');return e?e.textContent:''})()";
	for(;;)
	{
		uint16_t aPorts[8];
		int nPorts = 0;
		RefreshPids();
		if(!FindCdpPorts(aPorts, nPorts))
		{
			g_CdpHasData = 0;
			{
				char aMsg[80];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] no cdp ports (pids=%d)", g_NumPids);
				LsDbgLog("cdp-np", aMsg);
			}
			Sleep(5000);
			continue;
		}
		bool Found = false;
		for(int pi = 0; pi < nPorts && !Found; ++pi)
		{
			std::string List;
			if(!HttpGetPort(aPorts[pi], "/json/list", List))
			{
				char aMsg[80];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] port %u list fail", aPorts[pi]);
				LsDbgLog("cdp-lf", aMsg);
				continue;
			}
			{
				char aMsg[480];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] list43676 len=%u head=%.300s ws=%d", (unsigned)List.size(),
					List.c_str(), (int)(List.find("webSocketDebuggerUrl") != std::string::npos));
				LsDbgLog("cdp-list", aMsg);
			}
						if(List.find("orpheus") == std::string::npos)
			{
				char aMsg[160];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] port %u list no-orpheus len=%u head=%.40s", aPorts[pi], (unsigned)List.size(), List.c_str());
				LsDbgLog("cdp-no", aMsg);
				continue;
			}
			const size_t Up = List.find("webSocketDebuggerUrl");
			if(Up == std::string::npos)
			{
				char aMsg[160];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] port %u no-wsurl len=%u", aPorts[pi], (unsigned)List.size());
				LsDbgLog("cdp-no3", aMsg);
				continue;
			}
			const size_t Ps = List.find("/devtools/page/", Up);
			if(Ps == std::string::npos)
				continue;
			size_t Pe = Ps + 15;
			while(Pe < List.size() && List[Pe] != '"' && List[Pe] != '}')
				++Pe;
			std::string PageId = List.substr(Ps + 15, Pe - (Ps + 15));
			if(PageId.size() < 8)
			{
				char aMsg[160];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] port %u bad pageid", aPorts[pi]);
				LsDbgLog("cdp-no4", aMsg);
				continue;
			}
			std::string Value;
			if(!CdpEvaluate(aPorts[pi], PageId.c_str(), pExpr, Value))
			{
				char aMsg[80];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] port %u eval fail", aPorts[pi]);
				LsDbgLog("cdp-ef", aMsg);
				continue;
			}
			double Cur = -1, Total = -1;
			if(!ParseTimeText(Value, Cur, Total))
			{
				g_CdpHasData = 0;
				char aMsg[160];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] parse fail val=%.60s", Value.c_str());
				LsDbgLog("cdp-pf", aMsg);
				continue;
			}
			g_CdpCurSec = Cur;
			g_CdpTotalSec = Total;
			g_CdpLastOkAt = (double)time(NULL);
			g_CdpHasData = 1;
			Found = true;
			{
				char aMsg[96];
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] ok cur=%.0f total=%.0f", Cur, Total);
				LsDbgLog("cdp-ok", aMsg);
			}
		}
		if(!Found)
			g_CdpHasData = 0;
		Sleep(1000); // 1 秒轮询：界面时间文本秒级精度足够重锚
	}
}

// ---------- netease-watcher（HTTP 127.0.0.1:3574）：精确播放秒 ----------
// YUCLing/netease-watcher（Rust，hook 网易云内部状态）：GET / 返回
// {"music":{...,"duration":ms,...},"time":41.535} —— time 即精确播放秒，seek 立即反映。
// monitor 作为其宿主：3574 不通时自动拉起同目录 netease-watcher.exe；每秒轮询喂重锚。
static std::atomic<double> g_WatcherTimeMs{-1.0};
static std::atomic<double> g_WatcherDurMs{-1.0};
static std::atomic<double> g_WatcherAt{0.0};
static std::atomic<uint32_t> g_WatcherStale{0};   // time 冻结+有声 = hook 失效（重锚停用） // 最近成功轮询（epoch 秒）
static std::atomic<uint32_t> g_dwWatcherPid{0};   // 本 monitor 亲手拉起的 netease-watcher PID（0=没有）

// 取某进程的可执行文件全路径。失败返回 false。
static bool GetProcessImagePath(uint32_t Pid, wchar_t *pBuf, DWORD BufChars)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
	if(!h)
		return false;
	DWORD Len = BufChars;
	const BOOL Ok = QueryFullProcessImageNameW(h, 0, pBuf, &Len);
	CloseHandle(h);
	return Ok != FALSE;
}

// 只杀"我们自己目录下的 netease-watcher.exe"。
// 以前按镜像名扫全机器、见到就 TerminateProcess，会把用户自己开的同名进程一起打死。
// 现在先看追踪到的 PID；没有追踪（上个 monitor 实例拉起的）才按全路径匹配兜底。
static void RestartWatcherProcess()
{
	wchar_t aWant[MAX_PATH];
	if(GetModuleFileNameW(NULL, aWant, MAX_PATH) == 0)
		return;
	wchar_t *pSlash = wcsrchr(aWant, L'\\');
	if(!pSlash)
		return;
	wcscpy_s(pSlash + 1, MAX_PATH - (size_t)(pSlash + 1 - aWant), L"netease-watcher.exe");

	bool Killed = false;

	// 1) 优先：只杀我们追踪到的那个 PID，且必须确认它就是同一路径的 exe
	const uint32_t TrackedPid = g_dwWatcherPid.load();
	if(TrackedPid != 0)
	{
		wchar_t aPath[MAX_PATH];
		if(GetProcessImagePath(TrackedPid, aPath, MAX_PATH) && _wcsicmp(aPath, aWant) == 0)
		{
			HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, TrackedPid);
			if(h)
			{
				TerminateProcess(h, 0);
				WaitForSingleObject(h, 2000);
				CloseHandle(h);
				Killed = true;
			}
		}
		g_dwWatcherPid = 0;
	}

	// 2) 兜底：没有追踪信息时，按全路径匹配（绝不只看文件名）
	if(!Killed)
	{
		HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if(hSnap != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W Pe;
			Pe.dwSize = sizeof(Pe);
			if(Process32FirstW(hSnap, &Pe))
			{
				do
				{
					if(_wcsicmp(Pe.szExeFile, L"netease-watcher.exe") != 0)
						continue;
					wchar_t aPath[MAX_PATH];
					if(!GetProcessImagePath(Pe.th32ProcessID, aPath, MAX_PATH))
						continue;
					if(_wcsicmp(aPath, aWant) != 0)
						continue; // 不是我们目录下的，别碰
					HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Pe.th32ProcessID);
					if(h)
					{
						TerminateProcess(h, 0);
						WaitForSingleObject(h, 2000);
						CloseHandle(h);
					}
				} while(Process32NextW(hSnap, &Pe));
			}
			CloseHandle(hSnap);
		}
	}

	Sleep(500);
}

static void EnsureWatcherProcess()
{
	std::string Dum;
	if(HttpGetPort(3574, "/", Dum))
		return; // 已在跑
	wchar_t ExeDir[MAX_PATH];
	GetModuleFileNameW(NULL, ExeDir, MAX_PATH);
	wchar_t *pSlash = wcsrchr(ExeDir, L'\\');
	if(!pSlash)
		return;
	*pSlash = 0;

	// 目标 exe 必须真实存在，否则 CreateProcessW 会在 PATH/当前目录里
	// 找别的同名程序（经典的服务路径搜索顺序劫持）。
	wchar_t ExePath[MAX_PATH];
	_snwprintf(ExePath, MAX_PATH - 1, L"%s\\netease-watcher.exe", ExeDir);
	ExePath[MAX_PATH - 1] = 0;
	if(GetFileAttributesW(ExePath) == INVALID_FILE_ATTRIBUTES)
	{
		LsDbgLog("nw-nofile", "[nw] netease-watcher.exe not found next to monitor");
		return;
	}

	// 命令行整体加引号：目录含空格时（"D:\My Games\TClient"）不加引号会被
	// 拆成多个参数，Windows 还会从左往右猜可执行文件，等于给了路径劫持的口子。
	// lpApplicationName 显式传全路径，彻底消掉歧义。
	wchar_t Cmd[MAX_PATH + 8];
	_snwprintf(Cmd, MAX_PATH + 7, L"\"%s\"", ExePath);
	Cmd[MAX_PATH + 7] = 0;

	STARTUPINFOW Si;
	ZeroMemory(&Si, sizeof(Si));
	Si.cb = sizeof(Si);
	PROCESS_INFORMATION Pi;
	ZeroMemory(&Pi, sizeof(Pi));
	if(CreateProcessW(ExePath, Cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, ExeDir, &Si, &Pi))
	{
		g_dwWatcherPid = (uint32_t)Pi.dwProcessId;
		CloseHandle(Pi.hThread);
		CloseHandle(Pi.hProcess);
		Sleep(1500); // 等它的 HTTP 服务起来
	}
}

static DWORD WINAPI WatcherThread(LPVOID)
{
	WSADATA Wsa;
	WSAStartup(MAKEWORD(2, 2), &Wsa);
	for(;;)
	{
		EnsureWatcherProcess();
		std::string Body;
		bool Got = false;
		if(HttpGetPort(3574, "/", Body))
		{
			const size_t Tp = Body.find("\"time\":");
			const size_t Dp = Body.find("\"duration\":");
			if(Tp != std::string::npos && Dp != std::string::npos)
			{
				const double T = atof(Body.c_str() + Tp + 7); // '"time":' 共 7 字符,值从 +7 起(+6 会指到冒号,atof=0)
				const double D = atof(Body.c_str() + Dp + 11);
				if(T >= 0.0 && T < 3600.0 && D > 0.0 && D < 3600.0 * 1000.0)
				{
					g_WatcherTimeMs = T * 1000.0;
					g_WatcherDurMs = D;
					g_WatcherAt = NowEpochQpc(); // QPC 精确时基(1s 粒度会让外推抖 ±0.5s)
					Got = true;
				}
				{
					char aMsg[220];
					_snprintf(aMsg, sizeof(aMsg) - 1, "[nw] polled t=%.1f dur=%.0f body=%.120s", T, D, Body.c_str());
					LsDbgLog("nw-ok", aMsg);
				}
			}
		}
		if(!Got)
		{
			g_WatcherTimeMs = -1.0;
			LsDbgLog("nw-fail", "[nw] poll failed (http/parse)");
		}
		Sleep(1000);
	}
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
	if(GetLastError() == ERROR_ALREADY_EXISTS)
		return 0; // 已有实例

	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	QueryPerformanceFrequency(&g_qpcFreq);
	g_EpochBase = (double)time(NULL);
	QueryPerformanceCounter(&g_liEpochCounter);
	InitializeCriticalSection(&g_LoopCS);
	g_LoopCSReady = true;
	CreateThread(NULL, 0, CdpThread, NULL, 0, NULL); // 网易云 CDP 界面时间读取
	CreateThread(NULL, 0, WatcherThread, NULL, 0, NULL); // netease-watcher 精确播放秒

	g_hShm = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(IslandMediaShared), SHM_NAME);
	if(!g_hShm)
		return 1;
	g_pShm = (IslandMediaShared *)MapViewOfFile(g_hShm, FILE_MAP_WRITE, 0, 0, sizeof(IslandMediaShared));
	if(!g_pShm)
		return 1;
	memset(g_pShm, 0, sizeof(*g_pShm));
	g_pShm->Magic = SHM_MAGIC;
	g_pShm->Version = SHM_VERSION;

	// WASAPI 初始化（失败无所谓，没有电表就当一直“播放”）
	IMMDeviceEnumerator *pDevEnum = nullptr;
	CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&pDevEnum);
	IMMDevice *pDev = nullptr;
	if(pDevEnum)
		pDevEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
	IAudioSessionManager2 *pMgr2 = nullptr;
	if(pDev)
		pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void **)&pMgr2);
	if(pMgr2)
		pMgr2->GetSessionEnumerator(&g_pSessEnum);

	wchar_t wszLastTitle[256] = L"";
	wchar_t wszCurTitle[256] = L""; // 当前歌（窗口标题消失后仍保留，用于放完/暂停时继续显示）
	int64_t ElapsedMs = 0;
	LARGE_INTEGER liLast = {0};
	QueryPerformanceCounter(&liLast);
	LARGE_INTEGER liLastSess = {0};
	bool Playing = true; // 初始假定播放（EverLoud 兜底）

	for(;;)
	{
		LARGE_INTEGER liNow;
		QueryPerformanceCounter(&liNow);
		const double DtMs = (double)(liNow.QuadPart - liLast.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
		liLast = liNow;

		// 刷新 pid（每 3 秒）
		static LARGE_INTEGER liLastPid = {0};
		if(liLastPid.QuadPart == 0 || (liNow.QuadPart - liLastPid.QuadPart) / g_qpcFreq.QuadPart > 3)
		{
			liLastPid = liNow;
			RefreshPids();
			ManageLoopStreams(); // 网易云(重)启动 → 按根 PID 建进程环回流
			// pid 变 → 在新进程上重新找电表
			if((liNow.QuadPart - liLastSess.QuadPart) / g_qpcFreq.QuadPart > 1)
			{
				liLastSess = liNow;
				ScanSessions();
			}
		}

		// 电表扫描（每 1 秒）
		if((liNow.QuadPart - liLastSess.QuadPart) / g_qpcFreq.QuadPart > 1)
		{
			liLastSess = liNow;
			ScanSessions();
		}

		// LS leveldb 扫描（0.1 秒节流；mtime 变了才真正读；网易云只在播放/切歌/拖进度时写 playPostion，
		// 停写后客户端靠锚点+时钟外推，但锚点会随墙钟变旧 → 客户端会在 ~12s 后回退到标题计时）
		static LARGE_INTEGER liLastLS = {0};
		if((liNow.QuadPart - liLastLS.QuadPart) / (double)g_qpcFreq.QuadPart > 0.1)
		{
			liLastLS = liNow;
			ScanLS();
		}

		// 播放判定：曾有声音(EverLoud) → 信任电表；从未有声音 → 怀疑电表失灵(网易云常见) → 按播放处理
		if(g_NumMeters > 0)
		{
			const float Peak = PollAudioPeak();
			g_deafPeak = Peak;
			if(Peak > 0.01f)
			{
				g_EverLoud = true;
				g_DeafTicks = 0;
				g_PeakLowStreak = 0;
				Playing = true;
			}
			else if(!g_EverLoud)
			{
				// 电表在但一直没声音：30 秒（50ms * 600）都没动静就当“电表失灵”，视为播放
				if(++g_DeafTicks >= 600)
					Playing = true;
			}
			else
			{
				if(++g_PeakLowStreak >= 8) // >0.4s 低电表 → 暂停
					Playing = false;
			}
		}

		// 标题
		bool TitleChanged = false;
		const bool HaveWnd = PollWindowTitle();
		if(HaveWnd && wcscmp(g_wszBestTitle, wszLastTitle) != 0)
		{
			wcsncpy(wszLastTitle, g_wszBestTitle, 255);
			wszLastTitle[255] = 0;
			ElapsedMs = 0;
			Playing = true; // 新歌默认播放
			g_EverLoud = false;
			g_DeafTicks = 0;
			g_PeakLowStreak = 0;
			g_LsFraction = 0.0; // 旧歌的进度锚点作废，等 LS 重写
			g_LsHave = false;
			g_AudioCorrMs = 0.0; // 新歌音频钟从零起算，校正同步作废
			// 新歌：音频钟归零，等第一个音频帧重新累计
			EnterCriticalSection(&g_LoopCS);
			for(SLoopStream *pS : g_LoopStreams)
			{
				pS->Frames = 0.0;
				pS->ActiveFrames = 0.0;
				pS->LastFrameAt = 0.0;
				pS->LastNonSilentAt = 0.0;
			}
			LeaveCriticalSection(&g_LoopCS);
			wcsncpy(wszCurTitle, wszLastTitle, 255); // 记住"当前歌"，歌放完窗口标题没了也能接着显示
			wszCurTitle[255] = 0;
			TitleChanged = true;
		}
		else if(!HaveWnd && wszLastTitle[0])
		{
			// 网易云一首歌播完后会撤掉窗口标题（EnumWindows 找不到），此时不能把岛清空，
			// 继续用上一首标题、并把进度钉在末尾。等真切歌（标题变化）时再更新。
			ElapsedMs = 0;
			Playing = false;
			g_LsPaused = true;
			if(g_LsHave && g_LsFraction < 0.995)
				g_LsFraction = 1.0; // 钉到末尾
			g_LsCapturedAt = (double)time(NULL); // 刷新新鲜度，客户端才会用 100% 锚点
		}

		// 播放判定，三层优先：
		// 1) WASAPI 进程环回（最权威）：最近 300ms 有音频帧 = 在播。帧流断 = 暂停（亚秒级、零误判，
		//    静音片段的 SILENT 帧也推进位置）。AudioMs 即精确"播到哪一秒"。
		// 2) LS pauseStatus / 进度活性（环回激活失败的兜底）：老版网易云写 true 判暂停；
		//    playPostion 冻结 15 秒判暂停（新版网易云实测：暂停后仍会以同值重写 LS，mtime 会动，
		//    所以活性判定只看"值变了"，pWrite 变化不算进度 —— 见 ParseLSFile 的活性锚点）。
		// 3) 电表 EverLoud 策略（最老的兜底，保留）。
		bool AudioActive = false;
		double AudioMs = 0.0;
		const double NowEp = NowEpochQpc();
		EnterCriticalSection(&g_LoopCS);
		for(SLoopStream *pS : g_LoopStreams)
		{
			if(pS->Dead || pS->Rate <= 0.0)
				continue;
			// AudioMs = 有效播放毫秒（ActiveFrames：有声包推进、暂停冻结、切歌归零）
			const double Ms = pS->ActiveFrames * 1000.0 / pS->Rate;
			if(Ms > AudioMs)
				AudioMs = Ms;
			// 播放活性 = 最近 300ms 有**有声**包（网易云暂停时仍投静音帧，不算）
			if(Ms > 0.0 && NowEp - pS->LastNonSilentAt < 0.3)
				AudioActive = true;
		}
		LeaveCriticalSection(&g_LoopCS);
		if(g_HaveAudio)
		{
			Playing = AudioActive;
		}
		else
		{
			if(g_LsHave && g_LsPaused)
				Playing = false;
			if(g_LsHave && g_LsLastProgressAt > 0.0 && (double)time(NULL) - g_LsLastProgressAt > 15.0)
				Playing = false;
		}
		if(Playing && g_NumMeters > 0)
			ElapsedMs += (int64_t)DtMs;
		else if(g_NumMeters == 0 && HaveWnd && Playing && !g_LsPaused)
			ElapsedMs += (int64_t)DtMs; // 没电表就纯靠标题计时（暂停/进度冻结时不累计）

		// watcher 停更检测：time 冻结而音频有声 = hook 失效 → g_WatcherStale=1（位置回退音频钟）；
		// 失效 30 秒自动重启 netease-watcher.exe 重新 hook 自愈。
		{
			static double s_PrevVal = -1.0;
			static double s_FrozenSince = 0.0;
			static double s_StaleSince = 0.0;
			const double WVal = g_WatcherTimeMs;
			if(WVal != s_PrevVal)
			{
				s_PrevVal = WVal;
				s_FrozenSince = NowEpochQpc();
				s_StaleSince = 0.0;
			}
			else if(AudioActive && NowEpochQpc() - s_FrozenSince > 5.0)
			{
				if(s_StaleSince == 0.0)
					s_StaleSince = NowEpochQpc();
				if(NowEpochQpc() - s_StaleSince > 30.0)
				{
					s_StaleSince = NowEpochQpc(); // 节流：每 30 秒最多重启一次
					LsDbgLog("nw-restart", "[nw] stale 30s -> restart netease-watcher");
					RestartWatcherProcess();
				}
			}
			g_WatcherStale = s_StaleSince != 0.0;
		}

		g_pShm->Magic = SHM_MAGIC;
		g_pShm->Version = SHM_VERSION;
		// Liveness 用"是否有当前歌"判断，而不是"这一刻窗口在不在" —— 歌放完标题消失时岛应保持。
		const bool HasTrack = HaveWnd ? (wszLastTitle[0] != 0) : (wszCurTitle[0] != 0);
		g_pShm->Playing = Playing && HasTrack ? 1 : 0;
		g_pShm->ElapsedMs = ElapsedMs;
		g_pShm->LastUpdate = (double)time(NULL);
		g_pShm->Peak = g_deafPeak;
		g_pShm->Meters = (uint32_t)g_NumMeters;
		// LS 进度的可信期：只在最近一次写入后 LS_WRITE_GRACE 秒内相信 playPostion；
		// 网易云一旦暂停/停写，LS 会冻在旧值，此时改用标题计时（monitor 自己的 elapsed）才对得上。
		// 若当前检测到播放、但 LS 已是旧快照，则对齐单调时钟：Pos = Fraction*Dur + (Now - FractionAt)。
		// 这里 FractionAt 用当前时间回填，使得客户端外推 Age≈0，Pos 起始就落在真实位置并随本地时钟前进。
		// 客户端优先用 LS 锚点，仅在锚点缺失/不可信时回退到标题 elapsed；两者都在下面分别给出。
		g_pShm->Fraction = g_LsFraction;
		g_pShm->FractionAt = g_LsCapturedAt;
		g_pShm->Paused = (g_LsHave && g_LsPaused) ? 1 : 0;
		// 曲长兜底链：netease-watcher → LS → 客户端回传 → CDP 界面总长
		const double WDur = g_WatcherDurMs;
		const double CDur = g_CdpTotalSec;
		g_pShm->DurationMs = WDur > 0.0 ? WDur :
			(g_LsDurationMs > 0.0 ? g_LsDurationMs :
			(g_pShm->ClientDurationMs > 0.0 ? g_pShm->ClientDurationMs :
			(CDur > 0.0 ? CDur * 1000.0 : 0.0)));
		// 位置本体 = watcher（真实歌曲内位置）；失效（stale/hook 挂）时写 -1，客户端回退音频钟
		// TClient(2026-11-01):watcher 的 t 是上次轮询(约每秒一次)采的值,原样写
		// shm 会让客户端每秒重锚到"过去",位置周期性回跳(用户:"画面顿顿的")。
		// 在播(环回活跃)时按轮询年龄把 t 外推到"现在";轮询断供 >5s 停止外推。
		const double WT = g_WatcherTimeMs;
		const bool WStale = g_WatcherStale != 0;
		const double WD = g_WatcherDurMs;
		double WOut = WT;
		if(WT > 0.0 && !WStale && g_HaveAudio && AudioActive && g_WatcherAt > 0.0)
		{
			const double WAge = NowEp - g_WatcherAt;
			if(WAge > 0.0 && WAge < 5.0)
				WOut = WT + WAge * 1000.0;
		}
		g_pShm->WatcherMs = (WOut > 0.0 && !WStale) ? WOut : -1.0;
		g_pShm->WatcherDurMs = WD > 0.0 ? WD : -1.0;
		// 音频钟（环回激活成功才有效；AudioMs=已渲染毫秒，暂停自动冻结）
		// TClient(2026-11-01):LS 锚定校正 —— 环回中途激活/漏帧会让音频钟落后真实
		// 位置（实测差到 49 秒）。LS 有新鲜锚点时把音频钟慢慢拉回（每 tick ≤150ms,
		// 防 LS 秒级抖动跳变）；LS 断供后音频钟自由跑，恢复即重新对齐。暂停不校。
		if(g_HaveAudio && AudioActive && g_LsHave && g_LsFraction > 0.0 && g_LsFraction < 1.0 &&
			g_pShm->DurationMs > 0.0)
		{
			const double LsAge = (double)time(NULL) - g_LsCapturedAt;
			if(LsAge >= -1.0 && LsAge < 5.0)
			{
				const double Err = (g_LsFraction * g_pShm->DurationMs + LsAge * 1000.0) - AudioMs;
				const double Step = Err > 150.0 ? 150.0 : (Err < -150.0 ? -150.0 : Err);
				g_AudioCorrMs += Step;
			}
		}
		double OutAudioMs = AudioMs + g_AudioCorrMs;
		if(OutAudioMs < 0.0)
			OutAudioMs = 0.0;
		g_pShm->AudioMs = g_HaveAudio ? OutAudioMs : 0.0;
		g_pShm->AudioActive = g_HaveAudio && AudioActive ? 1 : 0;
		g_pShm->Title[0] = 0;
		if(HasTrack)
		{
			const wchar_t *pShown = HaveWnd && wszLastTitle[0] ? wszLastTitle : wszCurTitle;
			wcsncpy(g_pShm->Title, pShown, 255);
			g_pShm->Title[255] = 0;
		}

		(void)TitleChanged;
		Sleep(50);
	}

	// not reached
	return 0;
}
