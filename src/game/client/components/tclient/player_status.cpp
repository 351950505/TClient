#include "player_status.h"

#include <algorithm>
#include <vector>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/gameclient.h>

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
	if(!g_Config.m_TcShowSettingsGear)
		return;
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	RenderGears();
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

	std::vector<vec2> vGearPixels;
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
		vGearPixels.push_back(vec2(
			(Pos.x - WorldRect.m_TopLeft.x) * Graphics()->ScreenWidth() / WorldRect.Width(),
			(Pos.y - WorldRect.m_TopLeft.y) * Scale));
	}
	if(Any)
	{
		// 齿轮在像素空间 1:1 渲染
		Graphics()->MapScreen(CScreenRect(0.0f, 0.0f, (float)Graphics()->ScreenWidth(), (float)Graphics()->ScreenHeight()));
		const float GearSize = std::clamp(20.0f * Graphics()->ScreenHeight() / WorldRect.Height(), 10.0f, 96.0f);
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->TextColor(1.0f, 0.85f, 0.35f, 1.0f);
		for(const vec2 &Gear : vGearPixels)
		{
			const float Width = TextRender()->TextWidth(GearSize, FontIcon::GEAR, -1, -1.0f);
			TextRender()->Text(Gear.x - Width / 2.0f, Gear.y - GearSize / 2.0f, GearSize, FontIcon::GEAR, -1.0f);
		}
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		Graphics()->MapScreen(WorldRect);
	}
	if(Any)
		Graphics()->MapScreen(PrevRect);
}
