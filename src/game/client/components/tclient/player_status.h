/* TClient: 玩家状态标记——设置菜单齿轮与叠 Tee 向下提示。 */
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
	void RenderStackTeeHint();
};

#endif
