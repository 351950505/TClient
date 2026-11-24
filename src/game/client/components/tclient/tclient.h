#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_TCLIENT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_TCLIENT_H

#include "drown_board.h"
#include "map_progress.h"

#include <base/io.h>

#include <engine/client/enums.h>
#include <engine/external/regex.h>
#include <engine/http.h>
#include <engine/shared/console.h>

#include <game/client/component.h>

#include <array>
#include <deque>
#include <vector>

class IMap;

class CTClient : public CComponent
{
public:
	// Client-side manual update (independent release version, see TCLIENT_APP_VERSION).
	// Flow: Idle -> Checking -> (UpToDate | Available -> Downloading -> Downloaded) | Failed.
	// Checking, downloading and installing each require an explicit user action.
	// Startup/connection/timers never check; ordinary quit never installs.
	enum class EUpdateState
	{
		IDLE,
		CHECKING,
		UP_TO_DATE,
		AVAILABLE,
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
		char m_aApiZipUrl[512] = ""; // api.github.com 源解析出的 asset url（octet-stream）
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

	// ----- Client-side manual update internals -----
	EUpdateState m_UpdateState = EUpdateState::IDLE;
	SUpdateRelease m_UpdateRelease;
	char m_aUpdateStatus[256] = "";

	std::shared_ptr<IHttpRequest> m_pUpdateManifestTask = nullptr;
	std::shared_ptr<IHttpRequest> m_pUpdateDownloadTask = nullptr;

	char m_aUpdateReleaseTag[64] = "";
	char m_aUpdateManifestUrl[512] = "";

	void SetUpdateStatus(const char *pStatus);
	void UpdateFail(const char *pFormat, ...) [[gnu::format(printf, 2, 3)]];
	void PollUpdateTasks();
	void FetchUpdateManifest();
	void NextUpdateManifestSource(const char *pReason);
	void FinishUpdateManifest(std::shared_ptr<IHttpRequest> pSigTask, std::shared_ptr<IHttpRequest> pManifestTask);
	void FinishUpdateDownload();
	void StartDownloadTask();
	void StartUpdateHelperTask();
	void StartApiResolveTask(bool Helper);
	void FinishApiResolve();
	void RetryUpdateDownload(bool Helper, const char *pReason);
	void FinishUpdateBootstrap();
	void ResetUpdateTasks();
	bool VerifyUpdateManifestSignature(const unsigned char *pManifest, size_t ManifestSize, const unsigned char *pSignature, size_t SignatureSize);

	// 签名链：清单 Ed25519 验签（公钥内嵌）+ 自举（从已验证包里提取新 helper 运行）
	std::shared_ptr<IHttpRequest> m_pUpdateManifestSigTask = nullptr;
	char m_aUpdateManifestSigUrl[512] = "";
	char m_aUpdateHelperUrl[512] = "";
	char m_aUpdateBootstrapPath[IO_MAX_PATH_LENGTH] = "";
	bool BootstrapHelperPending = false;
	int m_UpdateManifestSource = 0;
	bool m_UpdateFoundCurrentRelease = false;
	std::shared_ptr<IHttpRequest> m_pUpdateHelperTask = nullptr;
	// "@api" 源（api.github.com 直连）：先解析 release-by-tag JSON 再下载
	std::shared_ptr<IHttpRequest> m_pUpdateApiResolveTask = nullptr;
	bool m_ApiResolvePending = false;
	char m_aUpdateApiHelperUrl[512] = "";
	int m_UpdateDownloadSource = 0;
	int m_UpdateDownloadAttempt = 0;
	bool m_UpdateRetryHelper = false;
	float m_UpdateRetryAt = -1.0f;

	// 游戏内中键玩家菜单(2026-11-01):中键点玩家弹出「偷取皮肤 / 加入队伍」
	int m_PlayerMenuCid = -1;
	vec2 m_PlayerMenuPos = vec2(0.0f, 0.0f);
	void RenderPlayerMiddleMenu();

	// 落水榜（QmClient Gores 组队落水榜移植，默认关闭）：只统计同队玩家冻结次数。
	CTClientDrownBoard m_DrownBoard;
	bool m_DrownBoardModeActive = false;
	int m_DrownBoardLastTick = -1;
	char m_aDrownBoardMap[128] = "";
	void UpdateDrownBoard();
	void ResetDrownBoard();

	struct SMapProgressTarget
	{
		bool m_Valid = false;
		int m_ClientId = -1;
		vec2 m_Position = vec2(0.0f, 0.0f);
		int m_Index = -1;
		int m_TeleCheckpoint = 0;
		vec2 m_PreviousPosition = vec2(0.0f, 0.0f);
		bool m_HasPreviousPosition = false;
	};
	TClientMapProgress::CMap m_MapProgressMap;
	std::array<TClientMapProgress::CPlayer, NUM_DUMMIES + 1> m_aMapProgressPlayers;
	std::array<SMapProgressTarget, NUM_DUMMIES + 1> m_aMapProgressTargets;
	std::array<int, NUM_DUMMIES + 1> m_aMapProgressClientIds = [] {
		std::array<int, NUM_DUMMIES + 1> aClientIds{};
		aClientIds.fill(-1);
		return aClientIds;
	}();
	IMap *m_pMapProgressMap = nullptr;
	const CCollision *m_pMapProgressCollision = nullptr;
	const CTile *m_pMapProgressGameLayer = nullptr;
	const CTile *m_pMapProgressFrontLayer = nullptr;
	const CTeleTile *m_pMapProgressTeleLayer = nullptr;
	char m_aMapProgressMapName[128] = "";
	int m_MapProgressBaseScanCursor = 0;
	unsigned m_MapProgressMapCrc = 0;
	bool m_MapProgressGoresMode = false;
	std::vector<uint8_t> m_vMapProgressWaterImages;
	bool m_MapProgressWaterImagesLoaded = false;
	int m_MapProgressVisualGroup = 0;
	int m_MapProgressVisualLayer = 0;
	int m_MapProgressVisualTile = 0;
	bool m_MapProgressVisualLayersComplete = true;
	bool m_MapProgressFailedForMap = false;
	IMap *m_pMapProgressFailedMap = nullptr;
	const CCollision *m_pMapProgressFailedCollision = nullptr;
	unsigned m_MapProgressFailedMapCrc = 0;
	void FailMapProgressForMap(IMap *pMap, const CCollision *pCollision, unsigned MapCrc);
	void ResetMapProgress();

	void UpdateMapProgress(bool NewSnapshot);
	void ScanMapProgressVisualLayers(int Budget);
	int MapProgressSlot(int Conn) const;
	int ResolveMapProgressClientId(int Slot) const;

public:
	CTClient();
	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnUpdate() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	void OnConsoleInit() override;
	void OnRender() override;

	void OnStateChange(int OldState, int NewState) override;
	void OnNewSnapshot() override;
	void OnShutdown() override;
	// 快照/状态/配置变化只排队，OnUpdate 统一应用一次：避免在快照分发内
	// 同步触发 graphics 线程 flush + 全组件 resize 级联（Windows 闪烁 TODO 的根因）。
	void QueueForcedAspectUpdate();
	void ApplyForcedAspect();

	std::shared_ptr<IHttpRequest> m_pTClientInfoTask = nullptr;
	void FetchTClientInfo();
	void FinishTClientInfo();
	void ResetTClientInfoTask();
	bool NeedUpdate();

	// ----- Client-side manual update (v1.4) -----
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

	// 落水榜：服务器声明的 Gores 模式才生效，Team 0 是否计入由配置决定。
	bool IsGoresGameMode() const;
	bool IsDrownBoardMode() const { return IsGoresGameMode(); }
	// Conn is a dummy index; -1 uses the active dummy and resolves its followed target.
	TClientMapProgress::SEstimate MapProgressEstimate(int Conn = -1) const;
	bool HasMapProgress(int Conn = -1) const;
	int MapProgressClientId(int Conn = -1) const;
	int GetDrownCount(int ClientId) const { return m_DrownBoard.Count(ClientId); }
	CTClientDrownBoard::SPlayerStats GetDrownStats(int ClientId) const { return m_DrownBoard.Stats(ClientId); }

	bool ChatDoSpecId(const char *pInput);
	bool InfoTaskDone() { return m_pTClientInfoTask && m_pTClientInfoTask->State() == EHttpState::DONE; }
	bool m_FetchedTClientInfo = false;
	char m_aVersionStr[10] = "0";

	Regex m_RegexChatIgnore;
	// 强制视口策略的待应用标志； true 时 OnUpdate 应用一次并清零。
	bool m_ForcedAspectQueued = false;
	// TClient: 自定义宽高比确认对话框状态
	bool m_AspectConfirmSeeded = false;
	bool m_AspectConfirmActive = false;
	int64_t m_AspectConfirmDeadline = 0;
	int m_AspectConfirmPrevRatio = 0;
	int m_AspectConfirmLastRatio = 0;

	bool IsBlockedByFng() const;
	bool IsAspectBlocked() const;
	void ScaleIngameCursorDelta(float &X, float &Y) const;
	void UpdateAspectConfirm();
	void RenderAspectConfirmOverlay();
	void ResetToPreviousAspect();
};

#endif
