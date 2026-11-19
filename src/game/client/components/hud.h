/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_HUD_H
#define GAME_CLIENT_COMPONENTS_HUD_H
#include <engine/client.h>
#include <engine/shared/protocol.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/components/tclient/frozen_hud_logic.h>
#include <game/client/ui.h>

#include <map>

struct SScoreInfo
{
	SScoreInfo()
	{
		Reset();
	}

	void Reset()
	{
		m_TextRankContainerIndex.Reset();
		m_TextScoreContainerIndex.Reset();
		m_RoundRectQuadContainerIndex = -1;
		m_OptionalNameTextContainerIndex.Reset();
		m_aScoreText[0] = 0;
		m_aRankText[0] = 0;
		m_aPlayerNameText[0] = 0;
		m_ScoreTextWidth = 0.f;
		m_Initialized = false;
	}

	STextContainerIndex m_TextRankContainerIndex;
	STextContainerIndex m_TextScoreContainerIndex;
	float m_ScoreTextWidth;
	char m_aScoreText[16];
	char m_aRankText[16];
	char m_aPlayerNameText[MAX_NAME_LENGTH];
	int m_RoundRectQuadContainerIndex;
	STextContainerIndex m_OptionalNameTextContainerIndex;

	bool m_Initialized;
};

class CHud : public CComponent
{
	float m_Width, m_Height;

	// Only the frozen count/tee row wait for the island's current-frame bounds.
	bool m_FrozenHudPending = false;
	int m_FrozenHudTeam = 0;
	STcFrozenHudFrame<MAX_CLIENTS> m_FrozenHudFrame;
	ColorRGBA m_FrozenHudTextColor;
	ColorRGBA m_FrozenHudTextOutlineColor;
	unsigned m_FrozenHudTextFlags = 0;

	int m_HudQuadContainerIndex;
	SScoreInfo m_aScoreInfo[2];
	// 稳定文本只在内容或布局变化时重建，颜色仍逐帧应用。
	CCachedText m_GameTimerText;
	CCachedText m_PauseNotificationText;
	CCachedText m_SuddenDeathText;
	STextContainerIndex m_FPSTextContainerIndex;
	STextContainerIndex m_DDRaceEffectsTextContainerIndex;
	STextContainerIndex m_PlayerAngleTextContainerIndex;
	float m_PlayerPrevAngle;
	STextContainerIndex m_aPlayerSpeedTextContainers[2];
	float m_aPlayerPrevSpeed[2];
	int m_aPlayerSpeed[2];
	enum class ESpeedChange
	{
		NONE,
		INCREASE,
		DECREASE
	};
	ESpeedChange m_aLastPlayerSpeedChange[2];
	STextContainerIndex m_aPlayerPositionContainers[2];
	float m_aPlayerPrevPosition[2];

	void RenderCursor();

	void RenderTextInfo();
	void RenderConnectionWarning();
	void RenderTeambalanceWarning();

	void PrepareAmmoHealthAndArmorQuads();
	void RenderAmmoHealthAndArmor(const CNetObj_Character *pCharacter);

	void PreparePlayerStateQuads();
	void RenderPlayerState(int ClientId);

	int m_LastSpectatorCountTick;
	// 本帧右下角观战名字框是否绘制了（供落水榜贴在其上方）
	bool m_SpectatorCountBoxShown = false;
	float m_SpectatorCountBoxHeight = 0.0f;
	void RenderSpectatorCount();

	// 冻结胶囊预烘焙（学灵动岛 EnsureShapeTexture）：面板/高光各烤一张 alpha 遮罩
	// 纹理按 (宽,高,圆角,变体) 缓存，每帧只是一次带色 blit，省掉 DrawRectExt 的
	// 72 次三角函数与 48 个自由三角形。遮罩是白 RGB + 覆盖率 alpha，顶点色直接相乘。
	struct SCapsuleMaskKey
	{
		int m_W;
		int m_H;
		int m_Rounding;
		bool m_Sheen;
		bool operator<(const SCapsuleMaskKey &Other) const
		{
			if(m_W != Other.m_W)
				return m_W < Other.m_W;
			if(m_H != Other.m_H)
				return m_H < Other.m_H;
			if(m_Rounding != Other.m_Rounding)
				return m_Rounding < Other.m_Rounding;
			return m_Sheen < Other.m_Sheen;
		}
	};
	// 缓存值带插入 tick：全量冲刷时跳过本 tick 插入的条目——调用方同帧先后取
	// Plate/Sheen 两张（hud.cpp EnsureCapsuleMask 调用点），若第二张触发冲刷，
	// 第一张刚返回、尚未上屏，卸载即悬垂句柄（2026-11-20 审查 P3-1）。
	struct SCapsuleMaskEntry
	{
		IGraphics::CTextureHandle m_Tex;
		int m_Tick;
	};
	std::map<SCapsuleMaskKey, SCapsuleMaskEntry> m_aCapsuleMaskCache;
	IGraphics::CTextureHandle EnsureCapsuleMask(int W, int H, float Rounding, bool Sheen);
	void RenderDummyActions();
	void RenderMovementInformation();

	void UpdateMovementInformationTextContainer(STextContainerIndex &TextContainer, float FontSize, float Value, float &PrevValue);
	void RenderMovementInformationTextContainer(STextContainerIndex &TextContainer, const ColorRGBA &Color, float X, float Y);

	class CMovementInformation
	{
	public:
		vec2 m_Pos;
		vec2 m_Speed;
		float m_Angle = 0.0f;
	};
	class CMovementInformation GetMovementInformation(int ClientId, int Conn) const;

	void RenderGameTimer();
	void RenderPauseNotification();
	void RenderSuddenDeath();

	void RenderScoreHud();
	int m_LastLocalClientId = -1;

	void RenderSpectatorHud();
	void RenderWarmupTimer();
	void RenderLocalTime(float x);

	static constexpr float MOVEMENT_INFORMATION_LINE_HEIGHT = 8.0f;

public:
	CHud();
	int Sizeof() const override { return sizeof(*this); }

	void ResetHudContainers();
	void OnWindowResize() override;
	void OnReset() override;
	void OnRender() override;
	void OnInit() override;
	void OnNewSnapshot() override;

	// Called after island rendering, with its occupied rectangle in HUD units.
	void RenderFrozenHud(const CUIRect &Island);
	static float FrozenHudRowY(const CUIRect &Row, const CUIRect &Island, float ScreenHeight);
	static CUIRect FrozenHudCapsulePlacement(CUIRect Capsule, const CUIRect &Island, float ScreenWidth, float ScreenHeight, float OffsetX, float OffsetY);

	// DDRace

	void OnMessage(int MsgType, void *pRawMsg) override;
	void RenderNinjaBarPos(float x, float y, float Width, float Height, float Progress, float Alpha = 1.0f);

private:
	void RenderRecord();
	void RenderDDRaceEffects();
	// Gores 组队落水榜（QmClient 移植，默认关闭）
	void RenderDrownBoard();
	float m_TimeCpDiff;
	float m_aPlayerRecord[NUM_DUMMIES];
	float m_FinishTimeDiff;
	int m_DDRaceTime;
	int m_FinishTimeLastReceivedTick;
	int m_TimeCpLastReceivedTick;
	bool m_ShowFinishTime;

	inline float GetMovementInformationBoxHeight();
	inline int GetDigitsIndex(int Value, int Max);

	// Quad Offsets
	int m_aAmmoOffset[NUM_WEAPONS];
	int m_HealthOffset;
	int m_EmptyHealthOffset;
	int m_ArmorOffset;
	int m_EmptyArmorOffset;
	int m_aCursorOffset[NUM_WEAPONS];
	int m_FlagOffset;
	int m_AirjumpOffset;
	int m_AirjumpEmptyOffset;
	int m_ModernAirjumpOffset;
	int m_ModernAirjumpEmptyOffset;
	int m_aWeaponOffset[NUM_WEAPONS];
	int m_EndlessJumpOffset;
	int m_EndlessHookOffset;
	int m_JetpackOffset;
	int m_TeleportGrenadeOffset;
	int m_TeleportGunOffset;
	int m_TeleportLaserOffset;
	int m_SoloOffset;
	int m_CollisionDisabledOffset;
	int m_HookHitDisabledOffset;
	int m_HammerHitDisabledOffset;
	int m_GunHitDisabledOffset;
	int m_ShotgunHitDisabledOffset;
	int m_GrenadeHitDisabledOffset;
	int m_LaserHitDisabledOffset;
	int m_DeepFrozenOffset;
	int m_LiveFrozenOffset;
	int m_DummyHammerOffset;
	int m_DummyCopyOffset;
	int m_PracticeModeOffset;
	int m_Team0ModeOffset;
	int m_LockModeOffset;
};

#endif
