#include <game/client/components/tclient/optimizer.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>

// 2026-11-08 v1.7.8.63：FPS 雾整体移除。
// 雾（tc_optimizer_fps_fog 及其 mode/radius/zoom_percent/render_rect 四个参数）
// 会在清晰区外压黑画面并按半径剔除实体与粒子，玩家反馈"雾挡视野"比省下的
// 帧数更亏，且它在回放（STATE_DEMOPLAYBACK）里同样生效，是打开回放异常时
// 最可疑的一条渲染路径。粒子总开关独立保留：它不改变画面可见性，只跳过
// 粒子绘制，收益明确、副作用小。

bool COptimizer::ParticlesDisabled() const
{
	if(!g_Config.m_TcOptimizer || !g_Config.m_TcOptimizerDisableParticles)
		return false;
	return Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK;
}
