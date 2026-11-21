/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_PLAYERS_H
#define GAME_CLIENT_COMPONENTS_PLAYERS_H
#include <generated/protocol.h>

#include <base/vmath.h>
#include <game/client/component.h>
#include <game/client/render.h>
#include <game/client/components/tclient/frozen_entry_highlight.h>

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
	tclient_frozen_entry::CHighlight m_aLocalFreezeEntryHighlight[NUM_DUMMIES];
	int m_aFreezeEntryClientId[NUM_DUMMIES] = {-1, -1};

	void CreateNinjaTeeRenderInfo();
	void CreateSpectatorTeeRenderInfo();
	void UpdateFreezeEntryTileHighlight();

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
	void OnStateChange(int NewState, int OldState) override;
	void OnMapLoad() override;
	void OnUpdate() override;
	void OnRender() override;
	void RenderFreezeEntryTileHighlight();
	void RenderHookTileHighlight(int ClientId);
	void HookTileHighlightReset();
	// 预判模式：未出钩时按准星方向模拟一次钩子飞行。命中砖块返回钩点
	// （round_to_int 对齐网络整数钩点）；命中可钩玩家返回玩家 id（m_Pos 为
	// 命中位置）——冻结 tee 的染色高亮与"钩玩家时不报砖块"都基于此。
	struct SHookLandingPrediction
	{
		vec2 m_Pos;
		int m_PlayerId = -1;
	};
	std::optional<SHookLandingPrediction> PredictHookLanding(int ClientId);
	// 预判结果帧缓存：染色路径（OnRender）与砖块高亮路径（maplayers）同帧
	// 共用一次模拟，返回 nullptr 表示本帧无落点。
	const SHookLandingPrediction *HookLandingPrediction(int ClientId);
	// 钩砖高亮动态颜色（模式：固定/彩虹/呼吸/脉冲/摆动），渲染时整体调制。
	ColorRGBA HookTileHighlightColor(float Time);
	// 钩住砖块高亮缓存容器：抓住期间零重建，只在选择砖块/钩点/配置变化时重建
	// （详见 RenderHookTileHighlight 的注释）。
	int m_HookTileHighlightContainer = -1;
	int m_HookTileHighlightTileCount = 0;
	int m_HookTileHighlightTileX[2] = {};
	int m_HookTileHighlightTileY[2] = {};
	int m_HookTileHighlightHookX = -1;
	int m_HookTileHighlightHookY = -1;
	unsigned long long m_HookTileHighlightKey = 0;
	// 重建节流（tc_hook_tile_highlight_throttle）上一次重建的 LocalTime。
	float m_HookTileHighlightLastRebuild = -1.0f;
	// 预判冻结 tee 染色高亮（water hammer 同款机制）：alpha 快速渐入渐出，
	// base 保存染色前的 RenderInfo 用于双绘制交叉淡化。
	float m_aHookPredictHighlightAlpha[MAX_CLIENTS]{};
	CTeeRenderInfo m_aHookPredictBaseRenderInfo[MAX_CLIENTS];
	// 预判结果帧缓存（见 HookLandingPrediction）。
	uint64_t m_HookPredictCacheFrame = 0;
	int m_HookPredictCacheClientId = -1;
	bool m_HookPredictCacheValid = false;
	SHookLandingPrediction m_HookPredictCache;

	const std::shared_ptr<CManagedTeeRenderInfo> &NinjaTeeRenderInfo() const { return m_pNinjaTeeRenderInfo; }
	const std::shared_ptr<CManagedTeeRenderInfo> &SpectatorTeeRenderInfo() const { return m_pSpectatorTeeRenderInfo; }
};

#endif
