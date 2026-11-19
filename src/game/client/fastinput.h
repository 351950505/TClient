#ifndef GAME_CLIENT_FASTINPUT_H
#define GAME_CLIENT_FASTINPUT_H

#include <base/vmath.h>

#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// 快速输入的纯数值部分；与预测世界分开，便于对拍原始机器码。
namespace FastInput
{
inline int PredictionTicks(float Amount)
{
	return Amount > 0.0f ? static_cast<int>(std::ceil(Amount)) : 0;
}

inline float BestAmount(int Offset, int Smoothing, int Latency)
{
	float Amount = std::clamp(Offset, 0, 1000) / 100.0f;
	if(Smoothing > 0)
		Amount *= 1.0f - std::clamp(Smoothing, 0, 100) / 200.0f;
	if(Latency > 0)
		Amount *= 1.0f + std::clamp(Latency, 0, 50) / 100.0f;
	return Amount;
}

inline float InterpolationIntra(float Intra, float Distance, int Mode)
{
	const float Linear = std::clamp(Intra, 0.0f, 1.0f);
	const float Square = Linear * Linear;
	const float Cubic = 3.0f * Square - 2.0f * Square * Linear;
	switch(std::clamp(Mode, 1, 3))
	{
	case 2:
		return Cubic;
	case 3:
		return Linear + (Cubic - Linear) * std::clamp(Distance / 1000.0f, 0.0f, 1.0f);
	default:
		return Linear;
	}
}

inline float FBlendAlpha(float ExtraTime, float DeltaTime)
{
	const float AlphaReference = 1.0f / (1.0f + std::max(ExtraTime, 0.0f) * 0.5f);
	const float Tau = -(1.0f / 144.0f) / std::log(std::max(1.0f - AlphaReference, 0.0001f));
	const float Dt = std::clamp(DeltaTime, 0.0f, 0.1f);
	return 1.0f - std::exp(-Dt / Tau);
}

inline void OffsetTicks(float Amount, int &Tick, float &Intra)
{
	if(Amount <= 0.0f)
		return;
	const int Whole = static_cast<int>(Amount);
	const float Combined = Intra + (Amount - static_cast<float>(Whole));
	const int Carry = static_cast<int>(Combined);
	Tick += Whole + Carry;
	Intra = Combined - static_cast<float>(Carry);
}

inline void OffsetMillis(int Millis, int &Tick, float &Intra)
{
	float Carry;
	Intra = std::modf(Intra + (Millis % 20) / 20.0f, &Carry);
	Tick += Millis / 20 + static_cast<int>(Carry);
}

inline void OffsetAbsolute(float Amount, int &Tick, float &Intra)
{
	// Keep the absolute tick out of floating-point arithmetic. Large game ticks are
	// not precise enough to retain Intra when converted to float.
	OffsetTicks(Amount, Tick, Intra);
}

struct CAncientState
{
	float m_Smooth = 0.0f;
	int m_LastUpdate = std::numeric_limits<int>::min();
	int m_LastDirection = 0;
	int m_LastReverse = std::numeric_limits<int>::min();
};

inline float AncientAmount(CAncientState &State, float Base, int Direction, float VelocityX, float VelocityY, int Tick)
{
	if(Base <= 0.0f)
		return Base;
	Direction = std::clamp(Direction, -1, 1);
	const int Previous = std::clamp(State.m_LastDirection, -1, 1);
	const bool Reversed = Direction != 0 && Previous != 0 && Direction != Previous;
	if(Reversed)
		State.m_LastReverse = Tick;
	State.m_LastDirection = Direction;

	float Target = Base;
	// 保留原码的有符号32位回绕，避免跨tick边界时产生有符号溢出。
	const int32_t Delta = static_cast<int32_t>(static_cast<uint32_t>(Tick) - static_cast<uint32_t>(State.m_LastReverse));
	const int32_t Distance = Delta < 0 ? static_cast<int32_t>(0u - static_cast<uint32_t>(Delta)) : Delta;
	if(State.m_LastReverse != std::numeric_limits<int>::min() && Distance <= 1 &&
		std::fabs(VelocityY) > std::max(std::fabs(VelocityX) + 0.5f, 1.75f))
		Target *= 0.55f;

	if(State.m_LastUpdate == std::numeric_limits<int>::min())
	{
		State.m_Smooth = Target;
		State.m_LastUpdate = Tick;
		return Target;
	}
	if(State.m_LastUpdate != Tick || Reversed)
	{
		const float Factor = State.m_Smooth > Target ? 0.7f : 0.35f;
		State.m_Smooth += (Target - State.m_Smooth) * Factor;
		State.m_LastUpdate = Tick;
	}
	return std::clamp(State.m_Smooth, 0.0f, Base);
}

inline int AncientTicks(float Millis)
{
	float Whole;
	const float Fraction = std::modf(std::max(Millis, 0.0f) / 20.0f, &Whole);
	return std::max(static_cast<int>(Whole), 0) + static_cast<int>(Fraction > 0.0001f);
}

inline void OffsetAncient(float Millis, int &Tick, float &Intra)
{
	if(Millis <= 0.0f)
		return;
	float Whole;
	const float Fraction = std::modf(std::max(Millis, 0.0f) / 20.0f, &Whole);
	float Carry;
	Intra = std::modf(Intra + Fraction, &Carry);
	Tick += std::max(static_cast<int>(Whole), 0) + static_cast<int>(Carry);
}

inline float SalatikAmount(int Offset, int Smoothing, int Latency)
{
	return BestAmount(Offset, Smoothing, Latency);
}

inline float SalatikIntra(float Intra, float Distance, int Mode)
{
	return InterpolationIntra(Intra, Distance, Mode);
}

// ---- 2026-11-19 新增档（对拍依据见 D:\kehuduan\NovaClient-2.2.9.1-分析\modes-15对拍-20261119.md §9）----

// Fast (JuKKi)（mode 15，Nova bc_inputs=13）：量 0.001-tick，ticks = amount/1000
// （Nova 0x14020f42f divss 1000.0；cvar bc_jukki_input_amount def=1000 max=5000）。
inline float JukkiAmount(int Amount)
{
	return Amount / 1000.0f;
}

// NovaPilot（mode 16，Nova 隐藏 mode 16）：量 0.01-tick，ticks = amount/100
// （Nova 0x14020f627；cvar nc_novapilot_input_amount def=100 max=300）。
inline float NovaPilotAmount(int Amount)
{
	return Amount / 100.0f;
}

// NovaPilot 自适应 jitter（Nova 0x14020ef50 逐条复核）：16 槽 int32 延迟样本环的
// population 方差；样本数 <=4 → 0；jitter = clamp(0.05·std, 0, 0.35)；
// var<0/NaN/Inf → 0（0x140c7eb60=0.05、0x140c83a64=0.35 实读）。
constexpr int NovaPilotJitterSampleSlots = 16;
inline float NovaPilotJitterTicks(const int *pSamples, int Count)
{
	if(!pSamples || Count <= 4)
		return 0.0f;
	if(Count > NovaPilotJitterSampleSlots)
		Count = NovaPilotJitterSampleSlots;
	float Sum = 0.0f;
	for(int i = 0; i < Count; i++)
		Sum += static_cast<float>(pSamples[i]);
	const float Mean = Sum / Count;
	float Variance = 0.0f;
	for(int i = 0; i < Count; i++)
	{
		const float Diff = static_cast<float>(pSamples[i]) - Mean;
		Variance += Diff * Diff;
	}
	Variance /= Count;
	if(!std::isfinite(Variance) || Variance <= 0.0f)
		return 0.0f;
	const float Jitter = std::sqrt(Variance) * 0.05f;
	if(!std::isfinite(Jitter))
		return 0.0f;
	return std::clamp(Jitter, 0.0f, 0.35f);
}

inline float NovaPilotTickOffset(int Amount, bool JitterEnabled, const int *pSamples, int Count)
{
	return NovaPilotAmount(Amount) + (JitterEnabled ? NovaPilotJitterTicks(pSamples, Count) : 0.0f);
}

inline bool FinitePos(vec2 Pos)
{
	return std::isfinite(Pos.x) && std::isfinite(Pos.y);
}

inline bool NovaPilotRejectPosition(vec2 Base, vec2 Pos)
{
	const float Distance = distance(Base, Pos);
	return !FinitePos(Pos) || !std::isfinite(Distance) || Distance > 300.0f || Distance <= 1.0f;
}

inline bool NovaPilotFreezeTile(int Tile)
{
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
}

// NovaPilot 渲染 Catmull-Rom（Nova 0x14020fa30 求值；采样门 0x1408126c0：t-2/t-1/t/t+1
// 四槽连续）。标准均匀 Catmull-Rom（切向 0.5·(p2-p0)），p(0)=P1、p(1)=P2。
inline float CatmullRom(float P0, float P1, float P2, float P3, float T)
{
	const float T2 = T * T;
	const float T3 = T2 * T;
	return 0.5f * ((2.0f * P1) + (-P0 + P2) * T +
			       (2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * T2 +
				       (-P0 + 3.0f * P1 - 3.0f * P2 + P3) * T3);
}

inline vec2 NovaPilotCurvedPos(vec2 P0, vec2 P1, vec2 P2, vec2 P3, float T, vec2 Linear)
{
	if(!FinitePos(P0) || !FinitePos(P1) || !FinitePos(P2) || !FinitePos(P3) ||
		!std::isfinite(T) || distance(P0, P1) > 256.0f ||
		distance(P1, P2) > 256.0f || distance(P2, P3) > 256.0f)
		return Linear;
	const vec2 Curved(CatmullRom(P0.x, P1.x, P2.x, P3.x, T),
		CatmullRom(P0.y, P1.y, P2.y, P3.y, T));
	return FinitePos(Curved) ? Curved : Linear;
}
}

#endif
