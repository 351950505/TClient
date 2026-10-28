#ifndef GAME_CLIENT_FASTINPUT_H
#define GAME_CLIENT_FASTINPUT_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// 快速输入的纯数值部分；与预测世界分开，便于对拍原始机器码。
namespace FastInput
{
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
	if(Amount <= 0.0f)
		return;
	const float Total = static_cast<float>(Tick - 1) + Intra + Amount;
	const int Whole = static_cast<int>(Total);
	Tick = Whole + 1;
	Intra = Total - static_cast<float>(Whole);
	if(Intra < 0.0f && Tick > 0)
	{
		--Tick;
		Intra += 1.0f;
	}
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
	float Amount = std::clamp(Offset, 0, 1000) / 100.0f;
	if(Smoothing > 0)
		Amount *= 1.0f - std::clamp(Smoothing, 0, 100) / 200.0f;
	if(Latency > 0)
		Amount *= 1.0f + std::clamp(Latency, 0, 50) / 100.0f;
	return Amount;
}

inline float SalatikIntra(float Intra, float Distance, int Mode)
{
	const float Linear = std::clamp(Intra, 0.0f, 1.0f);
	const float Square = Linear * Linear;
	const float Cubic = 3.0f * Square - (2.0f * Square) * Linear;
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
}

#endif
