#ifndef GAME_CLIENT_AUTOMARGIN_H
#define GAME_CLIENT_AUTOMARGIN_H

// TClient 自动余量（对齐 Nova bc_auto_margin 0x14020ead0/0x14020eb92-0x14020ee04
// 机器码，并与 BestClient-Light automargin.cpp 公开源码逐常数核对）。
// 逐常数证据表：D:\kehuduan\NovaClient-2.2.9.1-分析\modes-15对拍-20261119.md §9.5。
// 纯数值状态机：样本收集（每快照）与评估（0.5s 一步）分离，输出写回动作，
// cl_prediction_margin 的实际读写由 gameclient 接线完成。
// 互斥（协调者 2026-11-19 指令 + Nova 提示串 0x140cb0ad0）：
//   - cl_antiping_players > 0 期间暂停（恢复用户原值）；
//   - tc_prediction_margin_smooth 会平滑 margin 过渡、拖慢保护性抬升，帮助串注明不建议同开。
#include <algorithm>
#include <cmath>

namespace AutoMargin
{
// Nova 实读常数（0x140cxxxx 区 .rdata / 0x140b94f90 round-half-away）
constexpr int BaseMargin = 9;                    // 0x140c83a70 = 9.0
constexpr int MaxMargin = 300;                   // clamp 上限 0x12c
constexpr float CheckIntervalSeconds = 0.5f;     // 0x140c7adb4 = 0.5
constexpr float PingSmoothRise = 0.45f;          // 0x140c83a68
constexpr float PingSmoothFall = 0.20f;          // 0x140c7b428
constexpr float JitterSmoothRise = 0.35f;        // 0x140c83a64
constexpr float JitterSmoothFall = 0.15f;        // 0x140c83a60
constexpr float HighPingEnterMs = 50.0f;         // 0x140c78590
constexpr float HighPingExitMs = 42.0f;          // 0x140c83a74
constexpr float JitterEnterMs = 4.0f;            // 0x140c83a6c
constexpr float JitterExitMs = 2.0f;             // 0x140c707d0
constexpr float PingFraction = 0.25f;            // 0x140c78560
constexpr float BandRawFraction = 0.35f;         // 0x140c83a64
constexpr float BandEmaFraction = 0.94f;         // 0x140c88570（Nova=0.94；BC-Light 源码为 0.90f，按 Nova 取值）
constexpr int MarginDeadzone = 1;                // |target-current| <= 1 不写
constexpr int MarginStepDown = 5;                // 下调每步最多 5ms

// 每快照累积的延迟样本窗（Nova +0x14/+0x18/+0x1c/+0x20/+0x24）。
struct CSampleWindow
{
	float Latest = -1.0f;
	float Sum = 0.0f;
	int Count = 0;
	float Min = -1.0f;
	float Peak = -1.0f;

	void Reset()
	{
		Latest = -1.0f;
		Sum = 0.0f;
		Count = 0;
		Min = -1.0f;
		Peak = -1.0f;
	}

	// 样本 = m_Snap.m_pLocalInfo->m_Latency（快照本地延迟），max(0,·)（Nova
	// cmovg + BC-Light OnNewSnapshot 同款；负值按 0 计）。
	void AddSample(float Ping)
	{
		const float Sample = std::max(Ping, 0.0f);
		Latest = Sample;
		Sum += Sample;
		++Count;
		if(Min < 0.0f || Sample < Min)
			Min = Sample;
		if(Peak < 0.0f || Sample > Peak)
			Peak = Sample;
	}
};

struct CState
{
	float CheckTimer = 0.0f;       // Nova +0x10 / BC-Light m_CheckTimer
	CSampleWindow Window;          // Nova +0x14..+0x24
	float SmoothedPing = -1.0f;    // Nova +0x28（EMA 升 0.45/降 0.2，首样本取均值）
	float SmoothedJitter = 0.0f;   // Nova +0x2c（band EMA 升 0.35/降 0.15）
	bool Latched = false;          // Nova +0x30（50/4 进、42/2 出）
	bool WasEnabled = false;       // BC-Light m_WasEnabled
	int SavedMargin = -1;          // Nova +0x34

	void ResetEstimates()
	{
		CheckTimer = 0.0f;
		Window.Reset();
		SmoothedPing = -1.0f;
		SmoothedJitter = 0.0f;
		Latched = false;
	}
};

struct CUpdateInput
{
	bool Enabled = false;              // tc_auto_margin
	bool Online = false;               // STATE_ONLINE 且 m_Snap.m_pLocalInfo 可用
	bool AntipingPlayersPause = false; // cl_antiping_players > 0 → 暂停（恢复用户原值）
	float DeltaTime = 0.0f;            // Client()->RenderFrameTime()
	int CurrentMargin = 0;             // cl_prediction_margin 活值
};

struct CStep
{
	bool SaveMargin = false;    // 刚生效：外层把 cl_prediction_margin 存进 State.SavedMargin
	bool RestoreMargin = false; // 刚失效：外层写回 RestoreValue（>=0 才写）
	int RestoreValue = 0;
	bool AdjustMargin = false;  // 本步产生新目标：外层写 MarginTarget
	int MarginTarget = 0;
};

inline CStep Update(CState &State, const CUpdateInput &In)
{
	CStep Step;
	const bool Active = In.Enabled && In.Online && !In.AntipingPlayersPause;
	if(!Active)
	{
		if(State.WasEnabled)
		{
			Step.RestoreMargin = State.SavedMargin >= 0;
			Step.RestoreValue = State.SavedMargin;
			State.WasEnabled = false;
			State.SavedMargin = -1;
		}
		State.ResetEstimates();
		return Step;
	}

	if(!State.WasEnabled)
	{
		Step.SaveMargin = true;
		State.WasEnabled = true;
	}

	// 节流：0.5s 一步（Nova timer：+0x10 -= dt，>0 即返回，否则 += 0.5）。
	State.CheckTimer -= In.DeltaTime;
	if(State.CheckTimer > 0.0f)
		return Step;
	State.CheckTimer += CheckIntervalSeconds;
	if(State.CheckTimer < 0.0f)
		State.CheckTimer = 0.0f;

	// 消费样本窗（Nova 在评估尾部清零 +0x18/+0x20/+0x24）。
	const float CurrentPing = State.Window.Latest >= 0.0f ? State.Window.Latest : 0.0f;
	const float IntervalAveragePing = State.Window.Count > 0 ? State.Window.Sum / State.Window.Count : CurrentPing;
	const float IntervalMinPing = State.Window.Min >= 0.0f ? State.Window.Min : CurrentPing;
	const float SamplePing = State.Window.Peak >= 0.0f ? State.Window.Peak : CurrentPing;
	const float BandRaw = std::max(0.0f, SamplePing - IntervalMinPing);
	State.Window.Reset();

	if(State.SmoothedPing < 0.0f)
		State.SmoothedPing = IntervalAveragePing;
	else
	{
		const float SmoothFactor = IntervalAveragePing > State.SmoothedPing ? PingSmoothRise : PingSmoothFall;
		State.SmoothedPing += (IntervalAveragePing - State.SmoothedPing) * SmoothFactor;
	}

	const float JitterSmoothFactor = BandRaw > State.SmoothedJitter ? JitterSmoothRise : JitterSmoothFall;
	State.SmoothedJitter += (BandRaw - State.SmoothedJitter) * JitterSmoothFactor;

	const float Estimate = std::max(std::max(State.SmoothedPing, CurrentPing),
		IntervalAveragePing + State.SmoothedJitter * PingFraction);

	// 闩锁：进入 estimate≥50 ‖ peak≥50 ‖ band_ema≥4；退出 estimate<42 && band_ema≤2。
	const bool WantsDynamicMargin = Estimate >= HighPingEnterMs || SamplePing >= HighPingEnterMs || State.SmoothedJitter >= JitterEnterMs;
	if(State.Latched)
	{
		if(Estimate < HighPingExitMs && State.SmoothedJitter <= JitterExitMs)
			State.Latched = false;
	}
	else if(WantsDynamicMargin)
	{
		State.Latched = true;
	}

	int TargetMargin = BaseMargin;
	if(State.Latched)
	{
		const float StableMargin = std::max(BaseMargin * 1.0f, Estimate * PingFraction);
		const float JitterMargin = std::max(State.SmoothedJitter * BandEmaFraction, BandRaw * BandRawFraction);
		TargetMargin = std::clamp(static_cast<int>(std::round(StableMargin + JitterMargin)), BaseMargin, MaxMargin);
	}

	int CurrentMargin = std::clamp(In.CurrentMargin, BaseMargin, MaxMargin);
	const int MarginDelta = TargetMargin - CurrentMargin;
	if(std::abs(MarginDelta) <= MarginDeadzone)
		return Step;

	if(MarginDelta > 0)
		CurrentMargin = TargetMargin;
	else
		CurrentMargin -= std::min(-MarginDelta, MarginStepDown);
	CurrentMargin = std::clamp(CurrentMargin, BaseMargin, MaxMargin);

	// 变化才写（Nova：cmp r9d, ecx; je done → write）。
	if(In.CurrentMargin != CurrentMargin)
	{
		Step.AdjustMargin = true;
		Step.MarginTarget = CurrentMargin;
	}
	return Step;
}
}

#endif
