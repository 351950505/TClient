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
#include <cstddef>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cmath>
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
// 与 SHM 分开的 8 字节序号：monitor 每次整块发布前 +1（奇=写中）、写完后 +1（偶=稳定），
// 客户端读前读后各取一次，避免整块 memcpy 撞上多字段更新读到撕裂状态。
// 独立映射而不是加进 IslandMediaShared：旧 monitor 没有该映射时客户端自动退回单次 memcpy，
// 新 monitor 配旧客户端也只是多建一个没人读的映射，双方都不因 SHM_VERSION 不符而断链。
static const wchar_t SHM_SEQ_NAME[] = L"Local\\TClientIslandMediaSeq";
// P2-2（2026-11-20 审查）：网易云"进程已退出"信号。独立小映射而不是扩
// IslandMediaShared——旧客户端不开它也不受影响，新旧版本结构尺寸永不冲突。
static const wchar_t SHM_GONE_NAME[] = L"Local\\TClientIslandMediaGone";
// 客户端判定实例僵死后先 SetEvent 让 monitor 走正常退出；2 秒不退才 TerminateProcess。
static const wchar_t STOP_EVENT_NAME[] = L"Local\\TClientIslandMonitorStop";
static const wchar_t MUTEX_NAME[] = L"Local\\TClientIslandMonitorMutex";
static const uint32_t SHM_MAGIC = 0x4D53494E; // "NISM"
static const uint32_t SHM_VERSION = 7;

#pragma pack(push, 8)
struct IslandMediaShared
{
	uint32_t Magic;      // SHM_MAGIC
	uint32_t Version;    // SHM_VERSION
	uint32_t Playing;    // 1 = 累计判断为播放
	uint32_t Producer;   // v7 原有对齐填充，v2 Gone 的实例标记；不改变布局
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

// Gone v2 不更改 v7 主结构大小。实例标记及精确心跳绑定当前发布；
// 旧 v7 正常启动清空填充，新心跳也不会匹配已停止的 side 发布。
struct alignas(8) SIslandGoneV2
{
	volatile LONG64 Seq;
	uint32_t Magic;
	uint32_t WriterPid;
	uint64_t Instance;
	uint64_t UpdatedTick;
	double MainHeartbeat;
	uint32_t Producer;
	uint32_t Gone;
};
static_assert(sizeof(SIslandGoneV2) == 48 && offsetof(SIslandGoneV2, MainHeartbeat) == 32, "Gone v2 layout");
static_assert(offsetof(IslandMediaShared, ElapsedMs) == 16, "v7 layout");
static_assert(offsetof(IslandMediaShared, LastUpdate) == 24, "v7 layout");
static_assert(sizeof(IslandMediaShared) == 624, "v7 layout");
static HANDLE g_hGoneV2 = NULL;
static SIslandGoneV2 *g_pGoneV2 = nullptr;
static uint64_t g_MonitorInstance = 0;
static uint32_t g_Producer = 0;

static void PublishGoneV2(SIslandGoneV2 *pOut, uint32_t WriterPid, uint64_t Instance, uint32_t Producer, uint64_t Tick, double Heartbeat, bool Gone)
{
	if(!pOut)
		return;
	InterlockedIncrement64(&pOut->Seq);
	pOut->Magic = 0x324E4F47; // GON2
	pOut->WriterPid = WriterPid;
	pOut->Instance = Instance;
	pOut->UpdatedTick = Tick;
	pOut->MainHeartbeat = Heartbeat;
	pOut->Producer = Producer;
	pOut->Gone = Gone ? 1u : 0u;
	InterlockedIncrement64(&pOut->Seq);
}

static HANDLE g_hShm = NULL;
static IslandMediaShared *g_pShm = nullptr;
static HANDLE g_hSeq = NULL;
static volatile LONG64 *g_pSeq = nullptr;
static HANDLE g_hGone = NULL;
static volatile uint32_t *g_pGone = nullptr;
static HANDLE g_hStop = NULL; // 客户端判死时置位，主循环看到后走正常清理退出

// ---------- 进程 PID 收集（cloudmusic.exe） ----------
#define MAX_PIDS 32
static int g_NumPids = 0;
static uint32_t g_aPids[MAX_PIDS];
static uint32_t g_aPidParents[MAX_PIDS]; // 父进程 PID，用于识别根进程建环回流
// 主线程每 3 秒 RefreshPids，CdpThread 也要刷新；两边并发写同一张表会让
// FindCdpPorts 读到"数量是新的、内容是旧的"这种错绑组合。表更新与所有读取
// 都走 g_LoopCS（与环回流共用），刷新先写局部数组再整块发布。
static CRITICAL_SECTION g_LoopCS;
static bool g_LoopCSReady = false;
// 整个采样串行化；g_LoopCS 只保护发布，不能阻止旧快照晚发布。
static SRWLOCK g_PidRefreshLock = SRWLOCK_INIT;
static uint64_t g_PidGeneration = 0; // g_LoopCS 保护；失败也推进代次
static bool g_PidSampleValid = false;

// 返回本次采样计数；失败不能发布为空表。
static int SampleTargetPids(uint32_t *pPids, uint32_t *pParents)
{
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(hSnap == INVALID_HANDLE_VALUE)
		return -1;
	int NumPids = 0;
	PROCESSENTRY32W Pe;
	Pe.dwSize = sizeof(Pe);
	BOOL HaveEntry = Process32FirstW(hSnap, &Pe);
	while(HaveEntry)
	{
		if(_wcsicmp(Pe.szExeFile, L"cloudmusic.exe") == 0 && NumPids < MAX_PIDS)
		{
			pPids[NumPids] = Pe.th32ProcessID;
			pParents[NumPids] = Pe.th32ParentProcessID;
			++NumPids;
		}
		HaveEntry = Process32NextW(hSnap, &Pe);
	}
	const DWORD Error = GetLastError();
	CloseHandle(hSnap);
	return Error == ERROR_NO_MORE_FILES ? NumPids : -1;
}

static int RefreshPids(int (*pSample)(uint32_t *, uint32_t *) = SampleTargetPids, uint64_t *pGeneration = nullptr)
{
	AcquireSRWLockExclusive(&g_PidRefreshLock);
	uint32_t aPids[MAX_PIDS];
	uint32_t aParents[MAX_PIDS];
	const int NumPids = pSample(aPids, aParents);
	if(g_LoopCSReady)
		EnterCriticalSection(&g_LoopCS);
	++g_PidGeneration;
	g_PidSampleValid = NumPids >= 0 && NumPids <= MAX_PIDS;
	if(g_PidSampleValid)
	{
		memcpy(g_aPids, aPids, sizeof(uint32_t) * (size_t)NumPids);
		memcpy(g_aPidParents, aParents, sizeof(uint32_t) * (size_t)NumPids);
		g_NumPids = NumPids;
	}
	if(pGeneration)
		*pGeneration = g_PidGeneration;
	const int Result = g_PidSampleValid ? NumPids : -1;
	if(g_LoopCSReady)
		LeaveCriticalSection(&g_LoopCS);
	ReleaseSRWLockExclusive(&g_PidRefreshLock);
	return Result;
}

static bool IsTargetPid(uint32_t Pid)
{
	bool Found = false;
	if(g_LoopCSReady)
		EnterCriticalSection(&g_LoopCS);
	for(int i = 0; i < g_NumPids; ++i)
		if(g_aPids[i] == Pid)
		{
			Found = true;
			break;
		}
	if(g_LoopCSReady)
		LeaveCriticalSection(&g_LoopCS);
	return Found;
}

static int TargetPidCount()
{
	int Count = 0;
	if(g_LoopCSReady)
		EnterCriticalSection(&g_LoopCS);
	Count = g_NumPids;
	if(g_LoopCSReady)
		LeaveCriticalSection(&g_LoopCS);
	return Count;
}

struct SPlayerGoneTracker
{
	double m_ConfirmAt = -1.0;
	bool m_Gone = false;
	uint64_t m_Generation = 0;

	// 调用者在发布期间持有 g_LoopCS，确认结果不能跨过新的成功/失败采样。
	bool ConfirmedForPublication() const
	{
		return m_Gone && g_PidSampleValid && g_NumPids == 0 && m_Generation == g_PidGeneration;
	}

	bool Update(bool HaveWnd, int CachedPidCount, double Now, int (*pSample)(uint32_t *, uint32_t *) = SampleTargetPids)
	{
		if(HaveWnd || CachedPidCount > 0)
		{
			m_ConfirmAt = -1.0;
			m_Gone = false;
			return false;
		}
		if(m_ConfirmAt < 0.0)
			m_ConfirmAt = Now + 2.0;
		if(Now >= m_ConfirmAt)
		{
			const int FreshPidCount = RefreshPids(pSample, &m_Generation);
			m_Gone = FreshPidCount == 0;
			m_ConfirmAt = FreshPidCount == 0 ? Now + 0.5 : -1.0;
		}
		return m_Gone;
	}
};

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
	// 环回线程按音频包频率累加，主线程切歌时清零；原来 double 的 += 与 =0.0
	// 在临界区外并发，清零后旧增量会把进度"复活"（残留）。改用 64 位原子计数。
	volatile LONG64 Frames = 0;       // 累计帧数（含静音，渲染时间轴诊断用）
	volatile LONG64 ActiveFrames = 0; // 有效播放帧（暂停冻结，见下）
	volatile double Rate = 0.0;      // 采样率（帧/秒）
	volatile double LastFrameAt = 0.0; // 最近一包帧的 epoch 秒（含静音，诊断）
	volatile double LastNonSilentAt = 0.0; // 最近一个"有声"包的 epoch 秒
	bool Started = false; // guarded by g_LoopCS
	volatile bool Dead = false;
};

static std::vector<SLoopStream *> g_LoopStreams;
static bool g_HaveAudio = false; // 至少一条环回流激活成功（失败则回退电表/LS 判定）

class CActHandler final : public IActivateAudioInterfaceCompletionHandler
{
public:
	HANDLE m_hDone = NULL;
	IAudioClient *m_pClient = NULL;
	HRESULT m_hrOp = E_FAIL;
	// 创建方持 1 个引用；ActivateAudioInterfaceAsync 也持 1 个。
	// 8 秒超时后创建方先 Release，若系统回调迟到，对象仍由异步侧引用保活，
	// 回调写的是活对象而不是已析构的栈对象（原实现是栈上 Handler + 假计数 → UAF）。
	volatile LONG m_Refs = 1;
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
	STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_Refs); }
	STDMETHODIMP_(ULONG) Release() override
	{
		const LONG Refs = InterlockedDecrement(&m_Refs);
		if(Refs == 0)
			delete this;
		return (ULONG)(Refs < 0 ? 0 : Refs);
	}
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

	CActHandler *pHandler = new (std::nothrow) CActHandler();
	IActivateAudioInterfaceAsyncOperation *pOp = NULL;
	HRESULT hr = E_OUTOFMEMORY;
	if(pHandler && pHandler->m_hDone)
		hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &Pv, pHandler, &pOp);
	if(pOp)
		pOp->Release();
	{
		char aMsg[160];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu ActivateAudioInterfaceAsync hr=0x%08lX", (unsigned long)pS->Pid, (unsigned long)hr);
		LsDbgLog("lb-act", aMsg);
	}
	if(SUCCEEDED(hr) && pHandler)
	{
		const DWORD Wr = WaitForSingleObject(pHandler->m_hDone, 8000);
		hr = (Wr == WAIT_OBJECT_0) ? pHandler->m_hrOp : E_FAIL;
		if(SUCCEEDED(hr) && !pHandler->m_pClient)
			hr = E_FAIL;
		if(SUCCEEDED(hr))
		{
			pClient = pHandler->m_pClient;
			pHandler->m_pClient = NULL; // 所有权转移
		}
	}
	// 创建方引用到此释放。超时后对象可能还活着（异步侧引用），回调迟到也不会写已回收内存。
	if(pHandler)
		pHandler->Release();
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
		pS->Started = true;
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
					InterlockedExchangeAdd64(&pS->Frames, (LONG64)Frames);
					pS->LastFrameAt = NowEp;
					StatFrames += Frames;
					// 有声包：推进有效播放帧 ActiveFrames（= 精确"播到哪一秒"），刷新 LastNonSilentAt；
					// 静音包：仅当距上一个有声包 <0.3s（播放中的静音片段）才推进 ActiveFrames；
					// 超过 0.3s 的静音 = 暂停，ActiveFrames 冻结（AudioMs 随之冻结）。
					if(!Silent)
					{
						InterlockedExchangeAdd64(&pS->ActiveFrames, (LONG64)Frames);
						pS->LastNonSilentAt = NowEp;
						StatActiveFrames += Frames;
					}
					else if(NowEp - pS->LastNonSilentAt < 0.3)
					{
						InterlockedExchangeAdd64(&pS->ActiveFrames, (LONG64)Frames);
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
// 环回激活连续失败的退避（2026-11-18）：某些机器上进程环回不可用（实测 Initialize
// 返回 E_INVALIDARG），失败流每 50ms 主循环立刻重生 → 每秒 20 次完整激活尝试
// （线程创建销毁 + COM/ActivateAudioInterfaceAsync + 日志写盘），纯烧 CPU —— 玩家报
// "比其他客户端卡"的元凶之一。连续 3 次失败后该 PID 退避 60s，之后每 60s 探一次；
// 网易云换 PID 会清记录，流存活时失败计数清零。
struct SPidBackoff
{
	uint32_t m_Pid = 0;
	int m_Fails = 0;
	double m_NextTryAt = 0.0; // NowEpochQpc 秒
};
static std::vector<SPidBackoff> g_vPidBackoff;

static int FindPidBackoff(uint32_t Pid)
{
	for(size_t i = 0; i < g_vPidBackoff.size(); ++i)
		if(g_vPidBackoff[i].m_Pid == Pid)
			return (int)i;
	return -1;
}

static bool PidBackedOff(uint32_t Pid, double NowEp)
{
	const int i = FindPidBackoff(Pid);
	return i >= 0 && NowEp < g_vPidBackoff[i].m_NextTryAt;
}

static void ResetPidBackoff(uint32_t Pid)
{
	const int i = FindPidBackoff(Pid);
	if(i >= 0)
		g_vPidBackoff[i].m_Fails = 0;
}

static void BumpPidBackoff(uint32_t Pid, double NowEp)
{
	int i = FindPidBackoff(Pid);
	if(i < 0)
	{
		SPidBackoff B;
		B.m_Pid = Pid;
		g_vPidBackoff.push_back(B);
		i = (int)g_vPidBackoff.size() - 1;
	}
	SPidBackoff &B = g_vPidBackoff[i];
	++B.m_Fails;
	if(B.m_Fails >= 3)
	{
		B.m_NextTryAt = NowEp + 60.0;
		B.m_Fails = 0;
		char aMsg[128];
		_snprintf(aMsg, sizeof(aMsg) - 1, "[loopback] pid=%lu failing; back off 60s", (unsigned long)Pid);
		LsDbgLog("lb-backoff", aMsg);
	}
}

// 依据根进程（父进程不是 cloudmusic 的）建流；网易云重启换 PID 时自动重建
static void ManageLoopStreams()
{
	if(!g_LoopCSReady)
		return;
	const double NowEp = NowEpochQpc();
	EnterCriticalSection(&g_LoopCS);
	g_HaveAudio = false;
	for(size_t i = 0; i < g_LoopStreams.size();)
	{
		if(g_LoopStreams[i]->Dead)
		{
			BumpPidBackoff(g_LoopStreams[i]->Pid, NowEp);
			if(g_LoopStreams[i]->hThread)
				CloseHandle(g_LoopStreams[i]->hThread);
			delete g_LoopStreams[i];
			g_LoopStreams.erase(g_LoopStreams.begin() + i);
		}
		else
		{
			if(g_LoopStreams[i]->Started)
			{
				ResetPidBackoff(g_LoopStreams[i]->Pid);
				g_HaveAudio = true;
			}
			++i;
		}
	}
	// 网易云已退出的 PID：退避记录一并清掉
	for(size_t i = 0; i < g_vPidBackoff.size();)
	{
		bool Exists = false;
		for(int k = 0; k < g_NumPids; ++k)
			if(g_aPids[k] == g_vPidBackoff[i].m_Pid)
			{
				Exists = true;
				break;
			}
		if(!Exists)
			g_vPidBackoff.erase(g_vPidBackoff.begin() + i);
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
		if(PidBackedOff(Pid, NowEp))
			continue; // 激活连续失败，退避中
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
// 扫描门控记录的是"上次真正尝试解析的文件+mtime"（不管解析成不成功），
// 与 g_LsLastWrite（最近一次成功解析的写入时间）分开；否则没有 lastPlaying
// 的文件会每 100ms 被整份重读（上限 32MB）。
static wchar_t g_wszLsScannedFile[MAX_PATH * 2] = L"";
static FILETIME g_LsScannedWrite = {0, 0};
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

// 把 JSON 数字从原始字节里截到 NUL 结尾的小缓冲再交给 strtod。
// 原来直接 strtod((const char *)pVal)，而 leveldb 日志缓冲不保证 NUL 结尾，
// 数字串正好顶到文件尾时 strtod 会继续读缓冲之后（堆越界读）。
static double ParseJsonNumber(const uint8_t *pVal, const uint8_t *pEnd)
{
	char aNum[32];
	size_t Len = 0;
	while(pVal < pEnd && Len < sizeof(aNum) - 1)
	{
		const char C = (char)*pVal;
		if(C == ',' || C == '}' || C == ']' || C == '\r' || C == '\n' || C == ' ')
			break;
		aNum[Len++] = C;
		++pVal;
	}
	aNum[Len] = '\0';
	return Len ? strtod(aNum, nullptr) : 0.0;
}

static bool IsLsFileUnchanged(const wchar_t *pPath, const FILETIME &Write)
{
	return g_wszLsScannedFile[0] && wcscmp(pPath, g_wszLsScannedFile) == 0 &&
		Write.dwLowDateTime == g_LsScannedWrite.dwLowDateTime &&
		Write.dwHighDateTime == g_LsScannedWrite.dwHighDateTime;
}

static void ParseLSFile(const wchar_t *pPath, const FILETIME *pWrite)
{
	const double ReadingNow = (double)time(nullptr);
	HANDLE h = CreateFileW(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if(h == INVALID_HANDLE_VALUE)
		return;
	DWORD Hi = 0;
	DWORD Size = GetFileSize(h, &Hi);
	// [TClient] 审计修复（2026-11-20 P3）：32MB 门控按 64 位总大小判断——
	// 原条件 (Hi!=0 && Size>32MB) 对 <4GB 文件恒假，33MB~4GB 会整份读入
	const int64_t TotalSize = (((int64_t)Hi) << 32) | (int64_t)Size;
	if(Size == INVALID_FILE_SIZE || TotalSize > (32ll << 20))
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
			const double Frac = ParseJsonNumber(pVal + 1, pBuf + RecEnd);
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
						const double Secs = ParseJsonNumber(pVal + 1, pEnd);
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
		// mtime 门控：同一文件、同一写入时间就跳过整份读盘/扫描。
		// 网易云播放时约每秒写一次 playPostion，正常播放不受影响；
		// 暂停/停写期间不再 10Hz 重读同一份最多 32MB 的日志。
		if(IsLsFileUnchanged(wBestFile, BestWrite))
			continue;
		wcsncpy(g_wszLsScannedFile, wBestFile, MAX_PATH * 2 - 1);
		g_wszLsScannedFile[MAX_PATH * 2 - 1] = 0;
		g_LsScannedWrite = BestWrite;
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
		EnterCriticalSection(&g_LoopCS);
		for(int k = 0; k < g_NumPids; ++k)
			if(g_aPids[k] == r.dwOwningPid)
			{
				IsCloud = true;
				break;
			}
		LeaveCriticalSection(&g_LoopCS);
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
				_snprintf(aMsg, sizeof(aMsg) - 1, "[cdp] no cdp ports (pids=%d)", TargetPidCount());
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
// monitor 作为其宿主：3574 不通时自动拉起同目录 netease-watcher.exe；约 300ms 轮询喂重锚。
// （原 1s：watcher 采样间隔就是 seek 后位置收敛的延迟上限——2026-11-18 用户报"拖动进度
// 歌词有时不对"，把轮询加密到 300ms，收敛从最长 1s 降到最长 300ms。值本身仍是 hook 实时读，
// 加密轮询只会更新鲜，不会更旧。）
static std::atomic<double> g_WatcherTimeMs{-1.0};
static std::atomic<double> g_WatcherDurMs{-1.0};
static std::atomic<double> g_WatcherAt{0.0};
static std::atomic<uint32_t> g_WatcherStale{0};   // time 冻结+有声 = hook 失效（重锚停用） // 最近成功轮询（epoch 秒）
struct SWatcherPauseTracker
{
	double m_Position = -1.0;
	double m_LastAdvanceAt = 0.0;

	bool Update(double Position, double SampleAt, double Now)
	{
		if(!std::isfinite(Position) || Position < 0.0 || SampleAt <= 0.0 || Now - SampleAt >= 5.0 || Now < SampleAt)
		{
			m_Position = -1.0;
			m_LastAdvanceAt = 0.0;
			return false;
		}
		if(m_Position < 0.0 || std::fabs(Position - m_Position) >= 150.0 || Now < m_LastAdvanceAt)
		{
			m_Position = Position;
			m_LastAdvanceAt = Now;
		}
		return Now - m_LastAdvanceAt >= 3.0;
	}
};

static bool ResolveFallbackPlaying(bool Playing, bool LsPaused, bool LsStalled, bool WatcherValid, bool WatcherFrozen)
{
	if(LsPaused || LsStalled)
		return false;
	return WatcherValid ? !WatcherFrozen : Playing;
}

// 生产播放决策：标题缺失只是弱证据；只有可信位置到终点才锁住曲终。
struct SPlaybackDecision
{
	SWatcherPauseTracker m_Pause;
	bool m_Ended = false;
	double m_EndPosition = -1.0;

	bool Update(bool TitleChanged, bool HaveWnd, bool HaveAudio, bool AudioActive, bool PreviousPlaying,
		bool LsPaused, bool LsStalled, bool LsAtEnd, double Position, double Duration, double SampleAt, double Now, bool LsAdvancing = false)
	{
		const bool Valid = std::isfinite(Position) && Position >= 0.0 && SampleAt > 0.0 && Now >= SampleAt && Now - SampleAt < 5.0;
		if(TitleChanged)
		{
			m_Pause = {};
			m_Ended = false;
		}
		const bool Frozen = m_Pause.Update(Position, SampleAt, Now);
		if(AudioActive || LsAdvancing || (Valid && m_Ended && std::fabs(Position - m_EndPosition) >= 150.0))
			m_Ended = false; // 音频恢复、seek 或同歌重播，均优先于旧曲终锁
		const bool AtEnd = Valid && std::isfinite(Duration) && Duration > 0.0 && Position >= Duration && Position <= Duration + 1500.0;
		if(!HaveWnd && !AudioActive && (AtEnd || (!Valid && LsAtEnd)))
		{
			m_Ended = true;
			m_EndPosition = Valid ? Position : -1.0;
		}
		if(HaveAudio)
			return AudioActive;
		if(m_Ended)
			return false;
		return ResolveFallbackPlaying(LsAdvancing ? true : PreviousPlaying, LsPaused, LsStalled, Valid, Frozen);
	}
};

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

struct SWatcherStartBackoff
{
	double m_NextTryAt = -1.0;

	template<typename TStart>
	bool TryStart(double Now, TStart Start)
	{
		if(Now < m_NextTryAt)
			return false;
		// 尝试前进入退避，路径检查或 CreateProcess 失败也不密集重试。
		m_NextTryAt = Now + 30.0;
		Start();
		return true;
	}
};

static void StartWatcherProcess()
{
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

static void EnsureWatcherProcess()
{
	std::string Dum;
	if(HttpGetPort(3574, "/", Dum))
		return;
	// 仅由 WatcherThread 调用；NowEpochQpc 的墙钟锚点固定，不受校时影响。
	static SWatcherStartBackoff s_Backoff;
	s_Backoff.TryStart(NowEpochQpc(), StartWatcherProcess);
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
		Sleep(300); // 轮询间隔 = seek 后位置收敛延迟上限（2026-11-18 由 1s 加密到 300ms）
	}
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
	HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
	if(GetLastError() == ERROR_ALREADY_EXISTS)
		return 0; // 已有实例
	g_hStop = CreateEventW(NULL, FALSE, FALSE, STOP_EVENT_NAME);

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

	// 可选序号映射：建不出来（极少数受限环境）就退化为无 seqlock，客户端同样兼容。
	g_hSeq = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(LONG64), SHM_SEQ_NAME);
	if(g_hSeq)
	{
		g_pSeq = (volatile LONG64 *)MapViewOfFile(g_hSeq, FILE_MAP_WRITE, 0, 0, sizeof(LONG64));
		if(g_pSeq)
			*g_pSeq = 0;
	}

	// 可选"网易云已退出"信号映射：建不出来就退化为无该信号，客户端维持旧行为。
	g_hGone = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(uint32_t), SHM_GONE_NAME);
	if(g_hGone)
		g_pGone = (volatile uint32_t *)MapViewOfFile(g_hGone, FILE_MAP_WRITE, 0, 0, sizeof(uint32_t));
	if(g_pGone)
		*g_pGone = 0;
	LARGE_INTEGER InstanceCounter;
	QueryPerformanceCounter(&InstanceCounter);
	g_MonitorInstance = (uint64_t)InstanceCounter.QuadPart;
	g_Producer = (uint32_t)(g_MonitorInstance ^ (g_MonitorInstance >> 32) ^ GetCurrentProcessId()) | 1u;
	g_hGoneV2 = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SIslandGoneV2), L"Local\\TClientIslandMediaGoneV2");
	if(g_hGoneV2)
		g_pGoneV2 = (SIslandGoneV2 *)MapViewOfFile(g_hGoneV2, FILE_MAP_WRITE, 0, 0, sizeof(SIslandGoneV2));
	if(g_pGoneV2)
	{
		// 已有视图可能被其它客户端持有；先使旧发布失效再初始化新实例。
		InterlockedExchange64(&g_pGoneV2->Seq, 1);
		g_pGoneV2->Magic = 0;
		g_pGoneV2->Gone = 0;
		g_pGoneV2->Producer = 0;
		InterlockedExchange64(&g_pGoneV2->Seq, 2);
	}

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
		if(g_hStop && WaitForSingleObject(g_hStop, 0) == WAIT_OBJECT_0)
			break; // 客户端判定本实例僵死，要求退出
		LARGE_INTEGER liNow;
		QueryPerformanceCounter(&liNow);
		const double DtMs = (double)(liNow.QuadPart - liLast.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
		liLast = liNow;

		// 刷新 pid（每 3 秒）
		static LARGE_INTEGER liLastPid = {0};
		if(liLastPid.QuadPart == 0 || (liNow.QuadPart - liLastPid.QuadPart) / (double)g_qpcFreq.QuadPart >= 3.0)
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
				InterlockedExchange64(&pS->Frames, 0);
				InterlockedExchange64(&pS->ActiveFrames, 0);
				pS->LastFrameAt = 0.0;
				pS->LastNonSilentAt = 0.0;
			}
			LeaveCriticalSection(&g_LoopCS);
			wcsncpy(wszCurTitle, wszLastTitle, 255); // 记住"当前歌"，歌放完窗口标题没了也能接着显示
			wszCurTitle[255] = 0;
			TitleChanged = true;
		}
		// 标题缺失保留上一首身份；不能修改 LS 的真实暂停、位置或采样时刻。

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
		static SPlaybackDecision s_Playback;
		const double LsAge = (double)time(NULL) - g_LsCapturedAt;
		Playing = s_Playback.Update(TitleChanged, HaveWnd, g_HaveAudio, AudioActive, Playing,
			g_LsHave && g_LsPaused,
			g_LsHave && g_LsLastProgressAt > 0.0 && (double)time(NULL) - g_LsLastProgressAt > 15.0,
			g_LsHave && LsAge >= 0.0 && LsAge < 5.0 && g_LsFraction >= 1.0,
			g_WatcherTimeMs.load(), g_WatcherDurMs.load(), g_WatcherAt.load(), NowEp,
			g_LsHave && !g_LsPaused && g_LsFraction < 1.0 &&
			(double)time(NULL) >= g_LsLastProgressAt && (double)time(NULL) - g_LsLastProgressAt < 2.0);
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

		// 采样可能较慢，放在共享内存写入区间之前。
		static SPlayerGoneTracker s_GoneTracker;
		s_GoneTracker.Update(HaveWnd, TargetPidCount(), NowEpochQpc());

		// 确认代次的核对和发布共用 PID 表锁，期间不能插入另一轮采样发布。
		EnterCriticalSection(&g_LoopCS);
		const bool PlayerGone = s_GoneTracker.ConfirmedForPublication();
		if(g_pSeq)
			InterlockedIncrement64(g_pSeq); // 奇 = 正在写，客户端据此重试
		g_pShm->Magic = SHM_MAGIC;
		g_pShm->Version = SHM_VERSION;
		// Liveness 用"是否有当前歌"判断，而不是"这一刻窗口在不在" —— 歌放完标题消失时岛应保持。
		const bool HasTrack = HaveWnd ? (wszLastTitle[0] != 0) : (wszCurTitle[0] != 0);
		g_pShm->Playing = Playing && HasTrack ? 1 : 0;
		// P2-2：窗口标题消失且 cloudmusic.exe 进程也没了 = 网易云真的退出
		//（歌放完只撤标题、进程还在，不置位）。单字段对齐写，无需 seqlock。
		if(g_pGone)
			*g_pGone = PlayerGone ? 1u : 0u;
		g_pShm->ElapsedMs = ElapsedMs;
		g_pShm->Producer = g_Producer;
		g_pShm->LastUpdate = NowEpochQpc();
		g_pShm->Peak = g_deafPeak;
		g_pShm->Meters = (uint32_t)g_NumMeters;
		// LS 进度的可信期：只在最近一次写入后 LS_WRITE_GRACE 秒内相信 playPostion；
		// 网易云一旦暂停/停写，LS 会冻在旧值，此时改用标题计时（monitor 自己的 elapsed）才对得上。
		// 若当前检测到播放、但 LS 已是旧快照，则对齐单调时钟：Pos = Fraction*Dur + (Now - FractionAt)。
		// 这里 FractionAt 用当前时间回填，使得客户端外推 Age≈0，Pos 起始就落在真实位置并随本地时钟前进。
		// 客户端优先用 LS 锚点，仅在锚点缺失/不可信时回退到标题 elapsed；两者都在下面分别给出。
		g_pShm->Fraction = g_LsFraction;
		g_pShm->FractionAt = g_LsCapturedAt;
		g_pShm->Paused = (!Playing && g_LsHave && g_LsPaused) ? 1 : 0;
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
		PublishGoneV2(g_pGoneV2, GetCurrentProcessId(), g_MonitorInstance, g_Producer, GetTickCount64(), g_pShm->LastUpdate, PlayerGone);
		if(g_pSeq)
			InterlockedIncrement64(g_pSeq); // 偶 = 稳定，整块更新结束
		LeaveCriticalSection(&g_LoopCS);

		(void)TitleChanged;
		Sleep(50);
	}

	if(g_pGoneV2)
	{
		PublishGoneV2(g_pGoneV2, 0, 0, 0, 0, 0.0, false);
		UnmapViewOfFile(g_pGoneV2);
		g_pGoneV2 = nullptr;
	}
	if(g_hGoneV2)
	{
		CloseHandle(g_hGoneV2);
		g_hGoneV2 = NULL;
	}
	// 正常退出（收到 STOP_EVENT）：清 SHM 标记并释放句柄，让下一实例能立刻接管。
	if(g_pSeq)
		*g_pSeq = 0;
	if(g_pShm)
	{
		g_pShm->Magic = 0;
		UnmapViewOfFile(g_pShm);
		g_pShm = nullptr;
	}
	if(g_hShm)
	{
		CloseHandle(g_hShm);
		g_hShm = NULL;
	}
	if(g_pSeq)
	{
		UnmapViewOfFile((LPCVOID)g_pSeq);
		g_pSeq = nullptr;
	}
	if(g_hSeq)
	{
		CloseHandle(g_hSeq);
		g_hSeq = NULL;
	}
	if(g_pGone)
	{
		UnmapViewOfFile((LPCVOID)g_pGone);
		g_pGone = nullptr;
	}
	if(g_hGone)
	{
		CloseHandle(g_hGone);
		g_hGone = NULL;
	}
	if(g_hStop)
	{
		CloseHandle(g_hStop);
		g_hStop = NULL;
	}
	ReleaseMutex(hMutex);
	CloseHandle(hMutex);
	return 0;
}
