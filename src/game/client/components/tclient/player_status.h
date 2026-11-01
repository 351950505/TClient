/* TClient: 玩家状态标记——在打开设置菜单的玩家头顶画一个齿轮。

   数据来源是原版协议自带的 PLAYERFLAG_IN_MENU：客户端在 CControls::SnapInput
   里把菜单状态写进输入包的 PlayerFlags，服务器再原样放进 Character 快照广播给
   所有人。所以这个功能对**所有玩家**都生效（原版客户端也一样会发这个标志），
   不需要 TClient 信令，也不需要对方装我们的客户端。

   注意 0.7 协议移除了 PLAYERFLAG_IN_MENU（见 server/player.cpp FakeSnap 注释），
   所以只有 0.6 协议下的玩家能看到。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PLAYER_STATUS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PLAYER_STATUS_H

#include <game/client/component.h>

class CPlayerStatus : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;

	// 该玩家是否打开了设置菜单（读服务器广播的 PLAYERFLAG_IN_MENU）
	bool SettingsOpen(int ClientId) const;

private:
	void RenderGears();
};

#endif
