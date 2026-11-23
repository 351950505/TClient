#include "player_status.h"

#include <base/color.h>
#include <base/math.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/components/tclient/stack_tee_hint.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/gamecore.h>

#include <algorithm>
#include <vector>

bool CPlayerStatus::SettingsOpen(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	// 原版协议标志位：服务器把每个玩家的输入 PlayerFlags 原样塞进 Character 快照
	const CGameClient::CSnapState::CCharacterInfo &Character = GameClient()->m_Snap.m_aCharacters[ClientId];
	if(!Character.m_Active)
		return false;
	return (Character.m_Cur.m_PlayerFlags & PLAYERFLAG_IN_MENU) != 0;
}

void CPlayerStatus::OnRender()
{
	if(g_Config.m_TcShowSettingsGear == 0 && g_Config.m_MeStackTeeHint == 0)
		return;
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	if(g_Config.m_TcShowSettingsGear != 0)
		RenderGears();
	if(g_Config.m_MeStackTeeHint != 0)
		RenderStackTeeHint();
}

void CPlayerStatus::RenderGears()
{
	// 自己开着菜单时全屏被盖住，画了也看不见；聊天/控制台同理
	if(GameClient()->m_Menus.IsActive() || GameClient()->m_Chat.IsActive() || GameClient()->m_GameConsole.IsActive())
		return;

	// 世界坐标投影（与 pingwheel 的 RenderMarkers 同一套做法），画完必须还原，
	// 否则会把世界投影漏给后面按屏幕坐标绘制的组件。
	const vec2 Center = GameClient()->m_Camera.m_Center;
	const CScreenRect WorldRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom);
	const CScreenRect PrevRect = Graphics()->GetScreen();

	vec2 aGearPixels[MAX_CLIENTS];
	int NumGears = 0;
	bool Any = false;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[i];
		if(!pInfo || pInfo->m_Local)
			continue;
		if(!SettingsOpen(i))
			continue;
		const vec2 Pos = GameClient()->m_aClients[i].m_RenderPos + vec2(0.0f, -46.0f);
		// 只画当前屏幕内的玩家：屏幕外的齿轮没有意义，还会在镜头边缘堆一片
		if(!WorldRect.Inside(Pos))
			continue;
		if(!Any)
		{
			Graphics()->MapScreen(WorldRect);
			Any = true;
		}
		// 半透明深色圆底（世界空间，随缩放跟随 tee）
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.45f);
		Graphics()->DrawCircle(Pos.x, Pos.y, 13.0f, 32);
		Graphics()->QuadsEnd();

		// 齿轮投影到屏幕像素：字形按最终像素尺寸栅格化才是 1:1 渲染，
		// 世界空间直接画会被变换拉伸出锯齿（2026-11-06 玩家反馈）。
		const float Scale = Graphics()->ScreenHeight() / WorldRect.Height();
		aGearPixels[NumGears++] = vec2(
			(Pos.x - WorldRect.m_TopLeft.x) * Graphics()->ScreenWidth() / WorldRect.Width(),
			(Pos.y - WorldRect.m_TopLeft.y) * Scale);
	}
	if(Any)
	{
		// 齿轮在像素空间 1:1 渲染
		Graphics()->MapScreen(CScreenRect(0.0f, 0.0f, (float)Graphics()->ScreenWidth(), (float)Graphics()->ScreenHeight()));
		const float GearSize = std::clamp(20.0f * Graphics()->ScreenHeight() / WorldRect.Height(), 10.0f, 96.0f);
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcSettingsGearColor)));
		const float GearWidth = TextRender()->TextWidth(GearSize, FontIcon::GEAR, -1, -1.0f);
		for(int GearIndex = 0; GearIndex < NumGears; ++GearIndex)
		{
			const vec2 &Gear = aGearPixels[GearIndex];
			TextRender()->Text(Gear.x - GearWidth / 2.0f, Gear.y - GearSize / 2.0f, GearSize, FontIcon::GEAR, -1.0f);
		}
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		Graphics()->MapScreen(WorldRect);
	}
	if(Any)
		Graphics()->MapScreen(PrevRect);
}

void CPlayerStatus::RenderStackTeeHint()
{
	if(GameClient()->m_Menus.IsActive() || GameClient()->m_Chat.IsActive() || GameClient()->m_GameConsole.IsActive() ||
		GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS || !GameClient()->m_Snap.m_aCharacters[LocalId].m_Active ||
		!GameClient()->m_Snap.m_apPlayerInfos[LocalId])
		return;

	const auto &Local = GameClient()->m_aClients[LocalId];
	const CCharacterCore &LocalCore = g_Config.m_TcFastInput != 0 ? Local.m_RegularPredicted : Local.m_Predicted;
	const bool LocalFrozen = LocalCore.m_FreezeEnd != 0 || LocalCore.m_DeepFrozen || LocalCore.m_LiveFrozen;
	if(LocalFrozen)
		return;
	const vec2 LocalPos = Local.m_RenderPos;
	const int LocalTeam = GameClient()->m_Teams.Team(LocalId);
	const int LocalX = round_to_int(LocalPos.x);
	const float HalfSize = CCharacterCore::PhysicalSize() * 0.5f;
	int TargetId = -1;
	float TargetY = 0.0f;

	const tclient_stack_tee::CStackTeeCollisionState LocalCollisionState{
		LocalCore.m_Super, LocalCore.m_Solo, LocalCore.m_CollisionDisabled};
	const auto CanCollideWithClient = [&](int ClientId, const CCharacterCore &OtherCore) {
		const tclient_stack_tee::CStackTeeCollisionState OtherCollisionState{
			OtherCore.m_Super, OtherCore.m_Solo, OtherCore.m_CollisionDisabled};
		return tclient_stack_tee::CanPlayersCollide(GameClient()->m_Teams.CanCollide(LocalId, ClientId),
			LocalCore.m_Tuning.m_PlayerCollision, LocalCollisionState, OtherCollisionState);
	};
	// Only consider tees the local character can physically collide with. The
	// nearest such player below occupies the intended stack position.
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == LocalId || !GameClient()->m_aClients[ClientId].m_Active ||
			!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active || !GameClient()->m_Snap.m_apPlayerInfos[ClientId])
			continue;
		const auto &Candidate = GameClient()->m_aClients[ClientId];
		const vec2 Pos = Candidate.m_RenderPos;
		if(round_to_int(Pos.x) != LocalX || Pos.y <= LocalPos.y || (TargetId >= 0 && Pos.y >= TargetY))
			continue;
		const CCharacterCore &CandidateCore = g_Config.m_TcFastInput != 0 ? Candidate.m_RegularPredicted : Candidate.m_Predicted;
		if(!CanCollideWithClient(ClientId, CandidateCore))
			continue;
		TargetId = ClientId;
		TargetY = Pos.y;
	}

	if(TargetId < 0)
		return;

	const auto &Target = GameClient()->m_aClients[TargetId];
	const bool TargetFrozen = Target.m_FreezeEnd != 0 || Target.m_DeepFrozen || Target.m_LiveFrozen;
	const vec2 TargetPos = Target.m_RenderPos;
	const CCollision *pCollision = Collision();
	class CStackTeeSwitchQuery
	{
	public:
		const std::vector<SSwitchers> *m_pSwitchers;
		int m_Team;
		int m_SuperTeam;
	};
	const auto &Switchers = GameClient()->PredSwitchers();
	const CStackTeeSwitchQuery SwitchQuery{&Switchers, LocalTeam, GameClient()->m_Teams.TeamSuper()};
	const auto IsSwitchActive = [](unsigned char Number, void *pUser) {
		const auto &Query = *static_cast<const CStackTeeSwitchQuery *>(pUser);
		return Query.m_Team >= 0 && Query.m_Team < NUM_DDRACE_TEAMS && Query.m_Team != Query.m_SuperTeam &&
		       Number < Query.m_pSwitchers->size() && (*Query.m_pSwitchers)[Number].m_aStatus[Query.m_Team];
	};
	const tclient_stack_tee::CStackTeeHintCharacter LocalHint(true, LocalFrozen, LocalId, LocalX, LocalPos.y);
	const tclient_stack_tee::CStackTeeHintCharacter TargetHint(true, TargetFrozen, TargetId, round_to_int(TargetPos.x), TargetPos.y);
	const bool ShowHint = tclient_stack_tee::ShouldShow(g_Config.m_MeStackTeeHint != 0, LocalHint, TargetHint, [&] {
		const float MapWidth = pCollision->GetWidth() * 32.0f;
		const float MapHeight = pCollision->GetHeight() * 32.0f;
		if(LocalPos.x - HalfSize < 0.0f || LocalPos.y < 0.0f || TargetPos.y < 0.0f ||
			LocalPos.x + HalfSize >= MapWidth || TargetPos.x < 0.0f || TargetPos.x >= MapWidth ||
			LocalPos.y >= MapHeight || TargetPos.y >= MapHeight)
			return false;
		if((pCollision->GetMoveRestrictions(IsSwitchActive, (void *)&SwitchQuery, LocalPos, 0.0f) & CANTMOVE_DOWN) != 0)
			return false;

		// Simulate the local tee center falling until its bottom touches the frozen tee.
		const vec2 Start = LocalPos;
		const vec2 End(LocalPos.x, TargetPos.y - 2.0f * HalfSize);
		const float PlayerCollisionRadius = CCharacterCore::PhysicalSize() * 1.25f;
		for(int OtherId = 0; OtherId < MAX_CLIENTS; ++OtherId)
		{
			if(OtherId == LocalId || OtherId == TargetId || !GameClient()->m_aClients[OtherId].m_Active ||
				!GameClient()->m_Snap.m_aCharacters[OtherId].m_Active || !GameClient()->m_Snap.m_apPlayerInfos[OtherId])
				continue;
			const auto &Other = GameClient()->m_aClients[OtherId];
			const CCharacterCore &OtherCore = g_Config.m_TcFastInput != 0 ? Other.m_RegularPredicted : Other.m_Predicted;
			if(!CanCollideWithClient(OtherId, OtherCore))
				continue;
			if(!tclient_stack_tee::DropPathClearOfTee(Start, End, Other.m_RenderPos, PlayerCollisionRadius))
				return false;
		}

		return tclient_stack_tee::ClearDropPath(Start, End, [&](vec2 Point) {
			if(pCollision->TestBox(Point, CCharacterCore::PhysicalSizeVec2()))
				return false;

			const int MapIndex = pCollision->GetMapIndex(Point);
			if(tclient_stack_tee::TileStopsDrop(pCollision->GetTileIndex(MapIndex)) ||
				tclient_stack_tee::TileStopsDrop(pCollision->GetFrontTileIndex(MapIndex)))
				return false;

			if(tclient_stack_tee::HasSpeedupAtIndex(MapIndex, [&](int Index) { return pCollision->IsSpeedup(Index); }) ||
				pCollision->IsTeleport(MapIndex) || pCollision->IsEvilTeleport(MapIndex) ||
				pCollision->IsCheckTeleport(MapIndex) || pCollision->IsCheckEvilTeleport(MapIndex))
				return false;

			return (pCollision->GetMoveRestrictions(IsSwitchActive, (void *)&SwitchQuery, Point, HalfSize + 4.0f) & CANTMOVE_DOWN) == 0;
		});
	});
	if(!ShowHint)
		return;

	const vec2 HintPos = LocalPos + vec2(0.0f, -46.0f);
	const vec2 Center = GameClient()->m_Camera.m_Center;
	const CScreenRect WorldRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom);
	if(!WorldRect.Inside(HintPos))
		return;
	const CScreenRect PrevRect = Graphics()->GetScreen();
	Graphics()->MapScreen(WorldRect);
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.45f);
	Graphics()->DrawCircle(HintPos.x, HintPos.y, 13.0f, 32);
	Graphics()->QuadsEnd();

	const float Scale = Graphics()->ScreenHeight() / WorldRect.Height();
	const vec2 HintPixels((HintPos.x - WorldRect.m_TopLeft.x) * Graphics()->ScreenWidth() / WorldRect.Width(),
		(HintPos.y - WorldRect.m_TopLeft.y) * Scale);
	Graphics()->MapScreen(CScreenRect(0.0f, 0.0f, (float)Graphics()->ScreenWidth(), (float)Graphics()->ScreenHeight()));
	const float IconSize = std::clamp(20.0f * Graphics()->ScreenHeight() / WorldRect.Height(), 10.0f, 96.0f);
	TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
	TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcSettingsGearColor)));
	const float Width = TextRender()->TextWidth(IconSize, FontIcon::CHEVRON_DOWN, -1, -1.0f);
	TextRender()->Text(HintPixels.x - Width / 2.0f, HintPixels.y - IconSize / 2.0f, IconSize, FontIcon::CHEVRON_DOWN, -1.0f);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	Graphics()->MapScreen(WorldRect);
	Graphics()->MapScreen(PrevRect);
}
