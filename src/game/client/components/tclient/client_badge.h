/* TClient: 客户端身份标识（记分板企鹅徽标）。
   采用 M0REKZ/ddnet-custom-clients 识别标准的皮肤颜色法（LGPL）：
   把客户端 ID 编进自身皮肤颜色的 alpha 字节对，随 ClientInfo 广播给全服，
   原版服务端原样转发、原版客户端渲染忽略 alpha——零服务器、零额外消息。
   参考实现：Kaizo Client / Chillerbot-UX / Duck & Infclass / Rushie / Ascend。
   按用户要求不向上游标准仓库提 PR，TClient 的 ID 本地登记。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_CLIENT_BADGE_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_CLIENT_BADGE_H

#include <base/color.h>
#include <base/vmath.h>

#include <engine/graphics.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>

#include <cstdint>

// TClient 在 ddnet-custom-clients 标准里的登记号（本地登记，未回提上游）。
// 标准 ID 表 0-5 已被 Kaizo/Duck/Chillerbot/Rushie/Ascend 占用，顺延取 6。
enum
{
	CUSTOM_CLIENT_ID_TCLIENT = 16777223,
	CCID_COLOR_BODY_TCLIENT = 6,
	CCID_COLOR_FEET_TCLIENT = 0,
};

// 0.6 StartInfo/ChangeInfo 线上颜色插入：alpha 字节承载客户端标识。
// 仅改写发往服务器的整型颜色字段，本地渲染用 config 原值；渲染管线不读该 alpha。
inline void TcBadgeInsertColors(int &Body, int &Feet)
{
	Body = (Body & 0x00FFFFFF) | (CCID_COLOR_BODY_TCLIENT << 24);
	Feet = (Feet & 0x00FFFFFF) | (CCID_COLOR_FEET_TCLIENT << 24);
}

class CClientBadge : public CComponent
{
	std::uint64_t m_aIsTClient = 0; // 位图：cid → 是否 TClient 用户（MAX_CLIENTS=64）
	IGraphics::CTextureHandle m_Texture;
	bool m_TriedLoad = false;

	void TryLoadTexture();

public:
	int Sizeof() const override { return sizeof(*this); }

	// 快照 ClientInfo 处理时调用：检测 alpha 字节对并从存储颜色中剥掉，
	// 返回该玩家是否 TClient 用户（命中即置位，不会因后续快照丢失而清除）。
	bool DetectAndStrip(int ClientId, int &Body, int &Feet);

	// 玩家离开时清标记（CClientData::Reset 调用）
	void Clear(int ClientId) { m_aIsTClient &= ~(std::uint64_t(1) << ClientId); }
	bool IsTClientUser(int ClientId) const { return ClientId >= 0 && ClientId < MAX_CLIENTS && (m_aIsTClient >> ClientId) & 1; }
	void ResetMarks() { m_aIsTClient = 0; }

	// 记分板绘制（惰性加载纹理；data/tclient/penguin.png 可覆盖内置像素 Tux）
	void RenderBadge(vec2 Pos, float Size, float Alpha);
};

#endif
