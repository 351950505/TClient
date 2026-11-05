#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_TCLIENT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_TCLIENT_H

#include <engine/client/enums.h>
#include <engine/external/regex.h>
#include <base/io.h>
#include <engine/http.h>
#include <engine/shared/console.h>

#include <game/client/component.h>

#include <deque>
#include <vector>

class CTClient : public CComponent
{
public:
	// Client-side auto-update (independent release version, see TCLIENT_APP_VERSION).
	// Flow: Idle -> Checking -> (UpToDate | Available -> Downloading -> Downloaded) | Failed.
	// Nothing is downloaded or installed without an explicit user click, and the
	// client never quits on its own.
	enum class EUpdateState
	{
		IDLE,
		CHECKING,
		UP_TO_DATE,
		AVAILABLE,
		PROBING,
		DOWNLOADING,
		DOWNLOADED,
		FAILED,
	};

	// A verified release candidate. Only filled in after the manifest of the
	// release passed all validation checks.
	struct SUpdateRelease
	{
		bool m_Valid = false;
		char m_aTag[64] = "";
		char m_aVersion[32] = "";
		char m_aAsset[64] = "";
		char m_aSha256[65] = "";
		int64_t m_Size = 0;
		char m_aHelperSha256[65] = "";
		int64_t m_HelperSize = 0;
		char m_aZipUrl[512] = "";
	};

private:
	std::deque<vec2> m_aAirRescuePositions[NUM_DUMMIES];
	void AirRescue();
	static void ConAirRescue(IConsole::IResult *pResult, void *pUserData);

	static void ConCalc(IConsole::IResult *pResult, void *pUserData);
	static void ConRandomTee(IConsole::IResult *pResult, void *pUserData);
	static void ConchainRandomColor(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void RandomBodyColor();
	static void RandomFeetColor();
	static void RandomSkin(void *pUserData);
	static void RandomFlag(void *pUserData);

	static void ConSpecId(IConsole::IResult *pResult, void *pUserData);
	void SpecId(int ClientId);

	static void ConUpdateCheck(IConsole::IResult *pResult, void *pUserData);

	int m_EmoteCycle = 0;
	static void ConEmoteCycle(IConsole::IResult *pResult, void *pUserData);

	class IEngineGraphics *m_pGraphics = nullptr;

	char m_PreviousOwnMessage[2048] = {};

	bool SendNonDuplicateMessage(int Team, const char *pLine);

	float m_FinishTextTimeout = 0.0f;
	void DoFinishCheck();

	bool ServerCommandExists(const char *pCommand);

	// ----- Client-side auto-update internals -----
	EUpdateState m_UpdateState = EUpdateState::IDLE;
	SUpdateRelease m_UpdateRelease;
	char m_aUpdateStatus[256] = "";
	int64_t m_LastUpdateCheck = 0; // unix timestamp of the last automatic check
	bool m_UpdateCheckDone = false;

	std::shared_ptr<IHttpRequest> m_pUpdateManifestTask = nullptr;
	std::shared_ptr<IHttpRequest> m_pUpdateDownloadTask = nullptr;

	char m_aUpdateReleaseTag[64] = "";
	char m_aUpdateManifestUrl[512] = "";

	void SetUpdateStatus(const char *pStatus);
	void UpdateFail(const char *pFormat, ...) [[gnu::format(printf, 2, 3)]];
	void PollUpdateTasks();
	void FetchUpdateManifest();
	void FinishUpdateManifest(std::shared_ptr<IHttpRequest> pSigTask, std::shared_ptr<IHttpRequest> pManifestTask);
	void FinishUpdateDownload();
	void StartDownloadTask();
	void FinishUpdateBootstrap();
	void ResetUpdateTasks();
	bool UpdateShouldAutoCheck() const;
	bool VerifyUpdateManifestSignature(const unsigned char *pManifest, size_t ManifestSize, const unsigned char *pSignature, size_t SignatureSize);

	// 签名链：清单 Ed25519 验签（公钥内嵌）+ 自举（从已验证包里提取新 helper 运行）
	std::shared_ptr<IHttpRequest> m_pUpdateManifestSigTask = nullptr;
	char m_aUpdateManifestSigUrl[512] = "";
	char m_aUpdateHelperUrl[512] = "";
	char m_aUpdateBootstrapPath[IO_MAX_PATH_LENGTH] = "";
	// 已完整下载（zip+helper 验证通过）的版本；用于自动检查时跳过同版本重复
	// 下载，以及退出弹窗的"同一版本只问一次"。
	char m_aUpdateDownloadedVersion[32] = "";
	char m_aUpdateDeclinedVersion[32] = "";
	// ME 页或退出弹窗已经启动过安装流（helper 已在跑），OnShutdown 不再弹窗。
	bool m_UpdateInstallLaunched = false;
	bool BootstrapHelperPending = false;
	int m_UpdateManifestSource = 0;
	std::shared_ptr<IHttpRequest> m_pUpdateHelperTask = nullptr;
	int m_UpdateDownloadAttempt = 0;
	float m_UpdateRetryAt = -1.0f;

	// 下载前对大文件候选源并发测速（TCLIENT_UPDATE_ZIP_PROXY_BASES），取实测
	// 最快的来源；全部颗粒无收时回退 releases 直链。
	std::vector<std::shared_ptr<IHttpRequest>> m_vpUpdateProbeTasks;
	float m_UpdateProbeEndAt = -1.0f;
	int m_UpdateProbeSource = -1;
	void StartProbeTasks();
	void FinishProbeTasks();

	// 游戏内中键玩家菜单(2026-11-01):中键点玩家弹出「偷取皮肤 / 加入队伍」
	int m_PlayerMenuCid = -1;
	vec2 m_PlayerMenuPos = vec2(0.0f, 0.0f);
	void RenderPlayerMiddleMenu();

public:
	CTClient();
	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	void OnConsoleInit() override;
	void OnRender() override;

	void OnStateChange(int OldState, int NewState) override;
	void OnNewSnapshot() override;
	void OnShutdown() override;
	void SetForcedAspect();

	std::shared_ptr<IHttpRequest> m_pTClientInfoTask = nullptr;
	void FetchTClientInfo();
	void FinishTClientInfo();
	void ResetTClientInfoTask();
	bool NeedUpdate();

	// ----- Client-side auto-update (v1.4) -----
	// These are called from the menu UI and from the console command.
	void UpdateCheck(bool Force);
	void UpdateStartDownload();
	void UpdateInstallAndRestart();
	bool UpdateHelperExists() const;

	EUpdateState GetUpdateState() const { return m_UpdateState; }
	const SUpdateRelease &GetUpdateRelease() const { return m_UpdateRelease; }
	const char *UpdateStateText() const;
	const char *UpdateStatusText() const { return m_aUpdateStatus; }
	int UpdateDownloadPercent() const;

	void RenderMiniVoteHud();
	void RenderCenterLines();
	void RenderCtfFlag(vec2 Pos, float Alpha);

	bool ChatDoSpecId(const char *pInput);
	bool InfoTaskDone() { return m_pTClientInfoTask && m_pTClientInfoTask->State() == EHttpState::DONE; }
	bool m_FetchedTClientInfo = false;
	char m_aVersionStr[10] = "0";

	Regex m_RegexChatIgnore;
};

#endif
