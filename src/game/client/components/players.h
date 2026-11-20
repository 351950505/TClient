/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_PLAYERS_H
#define GAME_CLIENT_COMPONENTS_PLAYERS_H
#include <generated/protocol.h>

#include <base/vmath.h>
#include <game/client/component.h>
#include <game/client/render.h>

#include <optional>

class CPlayers : public CComponent
{
	friend class CGhost;

	void RenderHand6(const CTeeRenderInfo *pInfo, vec2 HandPos, float HandAngle, float Alpha);
	void RenderHand7(const CTeeRenderInfo *pInfo, vec2 HandPos, float HandAngle, float Alpha);

	void RenderHand(const CTeeRenderInfo *pInfo, vec2 CenterPos, vec2 Dir, float AngleOffset, vec2 PostRotOffset, float Alpha);
	void RenderPlayer(
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		const CTeeRenderInfo *pRenderInfo,
		int ClientId,
		float Intra = 0.f,
		bool RenderGhost = false);

	void RenderHook(
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		const CTeeRenderInfo *pRenderInfo,
		int ClientId,
		float Intra = 0.f);
	void RenderHookCollLine(
		const CScreenRect &ScreenRect,
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		int ClientId);
	bool IsPlayerInfoAvailable(int ClientId) const;

	int m_WeaponEmoteQuadContainerIndex;
	int m_aWeaponSpriteMuzzleQuadContainerIndex[NUM_WEAPONS];
	float m_aWaterHammerHighlightAlpha[MAX_CLIENTS]{};
	CTeeRenderInfo m_aWaterHammerBaseRenderInfo[MAX_CLIENTS];

	void CreateNinjaTeeRenderInfo();
	void CreateSpectatorTeeRenderInfo();

	std::shared_ptr<CManagedTeeRenderInfo> m_pNinjaTeeRenderInfo;
	std::shared_ptr<CManagedTeeRenderInfo> m_pSpectatorTeeRenderInfo;

public:
	float GetPlayerTargetAngle(
		const CNetObj_Character *pPrevChar,
		const CNetObj_Character *pPlayerChar,
		int ClientId,
		float Intra = 0.0f);

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnRender() override;
	void RenderHookTileHighlight(int ClientId);
	void HookTileHighlightReset();
	// 预判模式：未出钩时按准星方向模拟一次钩子飞行，返回将勾住的钩点
	// （round_to_int 对齐网络整数钩点）。
	std::optional<vec2> PredictHookLanding(int ClientId);
	// 钩住砖块高亮缓存容器：抓住期间零重建，只在选择砖块/钩点/配置变化时重建
	// （详见 RenderHookTileHighlight 的注释）。
	int m_HookTileHighlightContainer = -1;
	int m_HookTileHighlightTileCount = 0;
	int m_HookTileHighlightTileX[2] = {};
	int m_HookTileHighlightTileY[2] = {};
	int m_HookTileHighlightHookX = -1;
	int m_HookTileHighlightHookY = -1;
	unsigned long long m_HookTileHighlightKey = 0;

	const std::shared_ptr<CManagedTeeRenderInfo> &NinjaTeeRenderInfo() const { return m_pNinjaTeeRenderInfo; }
	const std::shared_ptr<CManagedTeeRenderInfo> &SpectatorTeeRenderInfo() const { return m_pSpectatorTeeRenderInfo; }
};

#endif
