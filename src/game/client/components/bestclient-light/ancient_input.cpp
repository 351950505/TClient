/* Copyright © 2026 BestProject Team */
#include "ancient_input.h"

// TClient 移植（2026-11-18）：配置映射到 tc_* cvar；SnapTap 方向解析替换为普通方向计算；
// 模式号 12=Light 13=Azure 14=BestClient-Light Ancient。忠实对齐 BestClient-Light c56daba94。

#include <base/math.h>

#include <engine/shared/config.h>

#include <game/client/components/controls.h>
#include <game/client/gameclient.h>

#include <algorithm>
#include <cmath>
#include <limits>

CAncientInput::CAncientInput()
{
	OnReset();
}

bool CAncientInput::IsActive() const
{
	return g_Config.m_TcFastInputMode == 14 && Amount() > 0.0f;
}

float CAncientInput::Amount() const
{
	return std::clamp((float)g_Config.m_TcLightAncientInput, 0.0f, 100.0f);
}

bool CAncientInput::Others() const
{
	return IsActive() && g_Config.m_TcLightAncientInputOthers != 0;
}

float CAncientInput::TickMs(const CGameClient &GameClient) const
{
	return 1000.0f / (float)std::max(1, GameClient.Client()->GameTickSpeed());
}

float CAncientInput::OthersAmountMs() const
{
	return std::min(Amount(), std::clamp((float)g_Config.m_TcLightAncientInputOthersAmount, 0.0f, 100.0f));
}

int CAncientInput::SelfTickOffset() const
{
	if(!IsActive())
		return 0;
	const float StepMs = TickMs(*GameClient());
	if(Amount() <= 0.0f || StepMs <= 0.0f)
		return 0;
	return std::max(0, (int)std::ceil(Amount() / StepMs - 0.0001f));
}

int CAncientInput::OthersTickOffset() const
{
	if(!Others())
		return 0;
	const float StepMs = TickMs(*GameClient());
	const float OthersAmount = OthersAmountMs();
	if(OthersAmount <= 0.0f || StepMs <= 0.0f)
		return 0;
	return std::max(0, (int)std::ceil(OthersAmount / StepMs - 0.0001f));
}

CNetObj_PlayerInput &CAncientInput::Input(int Dummy)
{
	return m_aInput[Dummy];
}

const CNetObj_PlayerInput &CAncientInput::Input(int Dummy) const
{
	return m_aInput[Dummy];
}

void CAncientInput::OnReset()
{
	for(int i = 0; i < NUM_DUMMIES; i++)
	{
		m_aSmoothedAmount[i] = 0.0f;
		m_aSmoothedTick[i] = std::numeric_limits<int>::min();
		m_aLastDirection[i] = 0;
		m_aLastFlipTick[i] = std::numeric_limits<int>::min();
	}
}

int CAncientInput::LocalConn(const CGameClient &GameClient, int ClientId) const
{
	if(ClientId >= 0 && ClientId == GameClient.m_aLocalIds[0])
		return 0;
	if(ClientId >= 0 && ClientId == GameClient.m_aLocalIds[1])
		return 1;
	return -1;
}

bool CAncientInput::CheckNewInput(const CControls &Controls)
{
	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput NextInput = Controls.m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			const bool LeftPressed = Controls.m_aInputDirectionLeft[Dummy] != 0;
			const bool RightPressed = Controls.m_aInputDirectionRight[Dummy] != 0;
			NextInput.m_Direction = (LeftPressed && !RightPressed) ? -1 : ((!LeftPressed && RightPressed) ? 1 : 0);
		}

		if(m_aInput[Dummy].m_Direction != NextInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aInput[Dummy].m_Hook != NextInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aInput[Dummy].m_Fire != NextInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aInput[Dummy].m_Jump != NextInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aInput[Dummy].m_NextWeapon != NextInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aInput[Dummy].m_PrevWeapon != NextInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aInput[Dummy].m_WantedWeapon != NextInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		if(Dummy == g_Config.m_ClDummy && g_Config.m_ClSubTickAiming)
		{
			NextInput.m_TargetX = (int)Controls.m_aMousePos[Dummy].x;
			NextInput.m_TargetY = (int)Controls.m_aMousePos[Dummy].y;
		}

		m_aInput[Dummy] = NextInput;
	}

	return NewInput[0] || NewInput[1];
}

float CAncientInput::GetStabilizedAmountMs(const CGameClient &GameClient, int ClientId, float AmountMs) const
{
	if(AmountMs <= 0.0f)
		return AmountMs;

	const int Conn = LocalConn(GameClient, ClientId);
	if(Conn < 0)
		return AmountMs;

	const int Direction = std::clamp(m_aInput[Conn].m_Direction, -1, 1);
	const int PrevDirection = std::clamp(m_aLastDirection[Conn], -1, 1);
	const int PredTick = GameClient.Client()->PredGameTick(g_Config.m_ClDummy);
	const vec2 Vel = GameClient.m_aClients[ClientId].m_Predicted.m_Vel;
	const bool VerticalDominant = absolute(Vel.y) > std::max(1.75f, absolute(Vel.x) + 0.5f);
	const bool DirectionFlip = Direction != 0 && PrevDirection != 0 && Direction != PrevDirection;

	if(DirectionFlip)
		m_aLastFlipTick[Conn] = PredTick;
	m_aLastDirection[Conn] = Direction;

	const bool RecentFlip = m_aLastFlipTick[Conn] != std::numeric_limits<int>::min() &&
				absolute(PredTick - m_aLastFlipTick[Conn]) <= 1;

	float TargetAmount = AmountMs;
	if(RecentFlip && VerticalDominant)
		TargetAmount *= 0.55f;

	if(m_aSmoothedTick[Conn] == std::numeric_limits<int>::min())
	{
		m_aSmoothedAmount[Conn] = TargetAmount;
		m_aSmoothedTick[Conn] = PredTick;
		return m_aSmoothedAmount[Conn];
	}

	if(m_aSmoothedTick[Conn] != PredTick || DirectionFlip)
	{
		const float CurrentAmount = m_aSmoothedAmount[Conn];
		const float Blend = TargetAmount < CurrentAmount ? 0.70f : 0.35f;
		m_aSmoothedAmount[Conn] = CurrentAmount + (TargetAmount - CurrentAmount) * Blend;
		m_aSmoothedTick[Conn] = PredTick;
	}

	return std::clamp(m_aSmoothedAmount[Conn], 0.0f, AmountMs);
}

void CAncientInput::SplitAmount(const CGameClient &GameClient, float AmountMs, int &Ticks, float &Intra) const
{
	const float StepMs = TickMs(GameClient);
	if(StepMs <= 0.0f)
	{
		Ticks = 0;
		Intra = 0.0f;
		return;
	}

	const float TotalTicks = std::max(0.0f, AmountMs) / StepMs;
	float WholeTicks = 0.0f;
	Intra = std::modf(TotalTicks, &WholeTicks);
	Ticks = std::max(0, (int)WholeTicks);
}

void CAncientInput::ApplyOffset(const CGameClient &GameClient, int ClientId, int &Tick, float &Intra) const
{
	if(!IsActive())
		return;

	const bool IsLocal = GameClient.IsFastInputLocalClient(ClientId);
	if(!IsLocal && !Others())
		return;

	const float BaseAmountMs = IsLocal ? Amount() : OthersAmountMs();
	const float AmountMs = IsLocal ? GetStabilizedAmountMs(GameClient, ClientId, BaseAmountMs) : BaseAmountMs;

	int FastInputTicks = 0;
	float FastInputIntra = 0.0f;
	SplitAmount(GameClient, AmountMs, FastInputTicks, FastInputIntra);

	const float CombinedIntra = Intra + FastInputIntra;
	float WholeIntra = 0.0f;
	const float FinalIntra = std::modf(CombinedIntra, &WholeIntra);
	const int CarryOverTicks = (int)WholeIntra;

	Tick += FastInputTicks + CarryOverTicks;
	Intra = FinalIntra;

	if(Intra < 0.0f && Tick > 0)
	{
		Tick -= 1;
		Intra += 1.0f;
	}
}

bool CAncientInput::TryGetPredPos(const CGameClient &GameClient, int ClientId, int Tick, float Intra, vec2 &OutPos) const
{
	if(!IsActive() || Tick <= 0)
		return false;

	const bool IsLocal = GameClient.IsFastInputLocalClient(ClientId);
	const int FastInputTicks = IsLocal ? SelfTickOffset() : OthersTickOffset();
	const int MaxTick = GameClient.Client()->PredGameTick(g_Config.m_ClDummy) + FastInputTicks;

	if(GameClient.m_aClients[ClientId].m_aPredTick[(Tick - 1) % 200] != Tick - 1 ||
		GameClient.m_aClients[ClientId].m_aPredTick[Tick % 200] != Tick ||
		GameClient.m_aClients[ClientId].m_aPredTick[Tick % 200] > MaxTick)
		return false;

	OutPos = mix(GameClient.m_aClients[ClientId].m_aPredPos[(Tick - 1) % 200], GameClient.m_aClients[ClientId].m_aPredPos[Tick % 200], Intra);
	return true;
}

vec2 CAncientInput::SamplePos(const CGameClient &GameClient, int ClientId, float AmountMs, bool SafeGuard) const
{
	float PredIntraTick = GameClient.Client()->PredIntraGameTick(g_Config.m_ClDummy);
	int PredTick = GameClient.Client()->PredGameTick(g_Config.m_ClDummy);

	vec2 Pos = mix(GameClient.m_aClients[ClientId].m_PrevPredicted.m_Pos, GameClient.m_aClients[ClientId].m_Predicted.m_Pos, PredIntraTick);
	const vec2 RegularPos = GameClient.m_aClients[ClientId].m_RegularPredicted.m_Pos;

	int FastInputTicks = 0;
	float FastInputIntra = 0.0f;
	SplitAmount(GameClient, AmountMs, FastInputTicks, FastInputIntra);

	const float CombinedIntra = PredIntraTick + FastInputIntra;
	float WholeIntra = 0.0f;
	const float FinalIntra = std::modf(CombinedIntra, &WholeIntra);
	const int CarryOverTicks = (int)WholeIntra;

	FastInputTicks += CarryOverTicks;
	const int FinalTick = PredTick + FastInputTicks;

	if(FinalTick > 0 &&
		GameClient.m_aClients[ClientId].m_aPredTick[(FinalTick - 1) % 200] >= GameClient.Client()->PrevGameTick(g_Config.m_ClDummy) &&
		GameClient.m_aClients[ClientId].m_aPredTick[FinalTick % 200] <= GameClient.Client()->PredGameTick(g_Config.m_ClDummy) + FastInputTicks)
	{
		Pos = mix(GameClient.m_aClients[ClientId].m_aPredPos[(FinalTick - 1) % 200], GameClient.m_aClients[ClientId].m_aPredPos[FinalTick % 200], FinalIntra);
	}

	if(!SafeGuard)
		return Pos;

	const auto &ClientData = GameClient.m_aClients[ClientId];
	const bool StateChanged =
		ClientData.m_Predicted.m_FreezeEnd != ClientData.m_RegularPredicted.m_FreezeEnd ||
		ClientData.m_Predicted.m_HookState != ClientData.m_RegularPredicted.m_HookState ||
		ClientData.m_Predicted.HookedPlayer() != ClientData.m_RegularPredicted.HookedPlayer();
	if(StateChanged)
		return RegularPos;

	const float MaxAdvance = std::max(24.0f, length(ClientData.m_RegularPredicted.m_Vel) * (AmountMs / 1000.0f) * 1.75f + 18.0f);
	const vec2 Delta = Pos - RegularPos;
	const float DeltaLength = length(Delta);
	if(DeltaLength > MaxAdvance && DeltaLength > 0.0f)
		Pos = RegularPos + Delta * (MaxAdvance / DeltaLength);

	return Pos;
}

vec2 CAncientInput::FastInputPos(const CGameClient &GameClient, int ClientId) const
{
	const bool Local = GameClient.IsFastInputLocalClient(ClientId);
	const float AmountMs = Local ? GetStabilizedAmountMs(GameClient, ClientId, Amount()) : OthersAmountMs();
	if(!Local && !Others())
		return mix(GameClient.m_aClients[ClientId].m_PrevPredicted.m_Pos, GameClient.m_aClients[ClientId].m_Predicted.m_Pos, GameClient.Client()->PredIntraGameTick(g_Config.m_ClDummy));
	return SamplePos(GameClient, ClientId, AmountMs, true);
}

vec2 CAncientInput::ApplyToRenderedPos(const CGameClient &GameClient, int ClientId, vec2 BasePos) const
{
	if(!Others() || !IsActive())
		return BasePos;

	const float OthersMs = OthersAmountMs();
	if(OthersMs <= 0.0f)
		return BasePos;

	const vec2 RegularPos = GameClient.m_aClients[ClientId].m_RegularPredicted.m_Pos;
	const vec2 FastPos = SamplePos(GameClient, ClientId, OthersMs, true);
	return BasePos + (FastPos - RegularPos);
}
