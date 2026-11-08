#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_OPTIMIZER_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_OPTIMIZER_H

#include <base/vmath.h>

#include <game/client/component.h>

// TClient: 渲染优化开关容器（移植 MeowClient meow_optimizer* 的参数语义）。
//
// 历史：2026-11-07 引入 FPS 雾（清晰区外压黑并按半径剔除实体/粒子），2026-11-08
// v1.7.8.63 整体移除——玩家反馈雾挡视野，且它在回放态同样生效，是回放异常时
// 最可疑的渲染路径。现在只剩粒子总开关，它不改变画面可见性。
class COptimizer : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	// 粒子渲染总开关（tc_optimizer + tc_optimizer_disable_particles）
	bool ParticlesDisabled() const;
};

#endif
