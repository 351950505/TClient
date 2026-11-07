#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_OPTIMIZER_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_OPTIMIZER_H

#include <base/vmath.h>

#include <game/client/component.h>

// TClient: 渲染优化（移植 MeowClient meow_optimizer* 的参数语义，默认值与边界取自
// alpha 二进制帮助串实测：radius 5-300 默认 40 图块、可视区 10-120% 默认 70、其余开关默认关；
// 总开关默认开是 TClient 的用户决策 2026-11-07）。
//
// FPS 雾：以相机为中心，清晰区（占屏幕 zoom_percent%）之外渐隐成雾，
// 雾外的粒子与非焦点玩家直接跳过渲染；矩形模式可再把世界渲染 scissor 到雾矩形内。
//
// 注册顺序：必须排在世界组件（地图层/玩家/粒子/名字牌/冰冻条）之后、HUD（m_Hud）之前，
// 雾才能盖住世界、又不遮 HUD；render_rect 的 scissor 也在这里解除。
class COptimizer : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;

	// 雾裁剪是否生效（tc_optimizer + tc_optimizer_fps_fog，且在游戏/回放中）
	bool FogEnabled() const;
	// 世界坐标是否在雾外（应跳过渲染）。Size 为物体半径，做保守外扩。
	bool CullWorldPos(const vec2 &Pos, float Size = 0.0f) const;
	// 粒子渲染总开关（tc_optimizer_disable_particles，独立于雾）
	bool ParticlesDisabled() const;
	// render_rect：把本帧世界渲染 scissor 到雾矩形内（矩形雾才有意义）。
	// 在组件循环开始前调用；COptimizer::OnRender 里解除。
	void ClipWorld();
	// 世界单位下的雾裁剪半径；至少覆盖可视区，保证清晰区内永不裁。
	float CullRadius() const;

private:
	void DrawFog();
	bool m_WorldClipped = false;
};

#endif
