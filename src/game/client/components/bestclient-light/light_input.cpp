/* Copyright © 2026 BestProject Team */
#include "light_input.h"

// TClient 移植（2026-11-18）：配置映射到 tc_* cvar；SnapTap 方向解析替换为普通方向计算；
// 模式号 12=Light 13=Azure 14=BestClient-Light Ancient。忠实对齐 BestClient-Light c56daba94。

#include <base/math.h>

#include <engine/shared/config.h>

#include <game/client/components/controls.h>
#include <game/client/gameclient.h>

#include <cmath>

bool CLightInput::IsActive() const
{
	return g_Config.m_TcFastInputMode == 12 && Amount() > 0.0f;
}

float CLightInput::Amount() const
{
	return g_Config.m_TcLightInput / 100.0f;
}

bool CLightInput::Others() const
{
	return IsActive() && g_Config.m_TcLightInputOthers != 0;
}

int CLightInput::PredictionTickOffset() const
{
	if(Amount() <= 0.0f)
		return 0;
	return (int)std::ceil(Amount());
}

int CLightInput::HookAmountMs() const
{
	if(g_Config.m_TcLightInputHook)
		return g_Config.m_TcLightInputHookAmount;
	return (g_Config.m_TcLightInput + 2) / 5;
}

int CLightInput::HookTickOffset() const
{
	return (HookAmountMs() + 19) / 20;
}

bool CLightInput::IsHookClient(const CGameClient &GameClient, int ClientId) const
{
	if(!IsActive() || ClientId < 0 || ClientId >= MAX_CLIENTS || !GameClient.m_Snap.m_aCharacters[ClientId].m_Active)
		return false;

	const auto HasCoreHook = [](const CCharacterCore &Core) {
		return Core.m_HookState != HOOK_IDLE && Core.m_HookState != HOOK_RETRACTED;
	};
	const auto HasSnapHook = [](const CNetObj_Character &Char) {
		return Char.m_HookState != HOOK_IDLE && Char.m_HookState != HOOK_RETRACTED;
	};

	if(HasCoreHook(GameClient.m_aClients[ClientId].m_Predicted) ||
		HasCoreHook(GameClient.m_aClients[ClientId].m_PrevPredicted) ||
		HasSnapHook(GameClient.m_Snap.m_aCharacters[ClientId].m_Cur) ||
		HasSnapHook(GameClient.m_Snap.m_aCharacters[ClientId].m_Prev) ||
		GameClient.m_aClients[ClientId].m_Predicted.HookedPlayer() >= 0 ||
		GameClient.m_aClients[ClientId].m_PrevPredicted.HookedPlayer() >= 0 ||
		GameClient.m_Snap.m_aCharacters[ClientId].m_Cur.m_HookedPlayer >= 0 ||
		GameClient.m_Snap.m_aCharacters[ClientId].m_Prev.m_HookedPlayer >= 0)
		return true;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == ClientId || !GameClient.m_Snap.m_aCharacters[i].m_Active)
			continue;
		if(GameClient.m_aClients[i].m_Predicted.HookedPlayer() == ClientId ||
			GameClient.m_aClients[i].m_PrevPredicted.HookedPlayer() == ClientId ||
			GameClient.m_Snap.m_aCharacters[i].m_Cur.m_HookedPlayer == ClientId ||
			GameClient.m_Snap.m_aCharacters[i].m_Prev.m_HookedPlayer == ClientId)
			return true;
	}
	return false;
}

bool CLightInput::UseHookLogic(const CGameClient &GameClient) const
{
	if(!IsActive())
		return false;
	if(IsHookClient(GameClient, GameClient.m_Snap.m_LocalClientId))
		return true;
	if(GameClient.PredictDummy())
		return IsHookClient(GameClient, GameClient.m_aLocalIds[!g_Config.m_ClDummy]);
	return false;
}

bool CLightInput::IsHookLinkedClient(const CGameClient &GameClient, int ClientId) const
{
	if(!UseHookLogic(GameClient) || ClientId < 0 || ClientId >= MAX_CLIENTS || !GameClient.m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	if(GameClient.IsFastInputLocalClient(ClientId))
		return true;

	const auto HooksPlayer = [&](int From, int To) {
		if(From < 0 || From >= MAX_CLIENTS || To < 0 || To >= MAX_CLIENTS || !GameClient.m_Snap.m_aCharacters[From].m_Active)
			return false;
		return GameClient.m_aClients[From].m_Predicted.HookedPlayer() == To ||
		       GameClient.m_aClients[From].m_PrevPredicted.HookedPlayer() == To ||
		       GameClient.m_Snap.m_aCharacters[From].m_Cur.m_HookedPlayer == To ||
		       GameClient.m_Snap.m_aCharacters[From].m_Prev.m_HookedPlayer == To;
	};

	const int aLocalIds[] = {GameClient.m_Snap.m_LocalClientId, GameClient.PredictDummy() ? GameClient.m_aLocalIds[!g_Config.m_ClDummy] : -1};
	for(const int LocalId : aLocalIds)
	{
		if(LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient.m_Snap.m_aCharacters[LocalId].m_Active &&
			(HooksPlayer(LocalId, ClientId) || HooksPlayer(ClientId, LocalId)))
			return true;
	}
	return false;
}

CNetObj_PlayerInput &CLightInput::Input(int Dummy)
{
	return m_aInput[Dummy];
}

const CNetObj_PlayerInput &CLightInput::Input(int Dummy) const
{
	return m_aInput[Dummy];
}

bool CLightInput::CheckNewInput(const CControls &Controls)
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

		bool SetMousePos = false;
		if(Dummy == g_Config.m_ClDummy)
		{
			if(g_Config.m_ClSubTickAiming)
				SetMousePos = true;
			if(m_aInput[Dummy].m_Hook == 0 && NextInput.m_Hook == 1)
			{
				m_HookAction = true;
				SetMousePos = true;
			}
			if(m_aInput[Dummy].m_Fire != NextInput.m_Fire && NextInput.m_Fire % 2 == 1)
			{
				m_FireAction = true;
				SetMousePos = true;
			}
			if(!m_HookAction && !m_FireAction)
				SetMousePos = true;
		}

		if(SetMousePos)
		{
			NextInput.m_TargetX = (int)Controls.m_aMousePos[Dummy].x;
			NextInput.m_TargetY = (int)Controls.m_aMousePos[Dummy].y;
		}
		else
		{
			NextInput.m_TargetX = m_aInput[Dummy].m_TargetX;
			NextInput.m_TargetY = m_aInput[Dummy].m_TargetY;
		}

		m_aInput[Dummy] = NextInput;
	}

	return NewInput[0] || NewInput[1];
}

void CLightInput::ResetActions()
{
	m_HookAction = false;
	m_FireAction = false;
}

void CLightInput::ApplyOffset(float OffsetTicks, int &Tick, float &Intra) const
{
	if(OffsetTicks <= 0.0f)
		return;

	const int WholeTicks = (int)OffsetTicks;
	const float OffsetIntra = OffsetTicks - WholeTicks;
	const float CombinedIntra = Intra + OffsetIntra;
	const int CarryOverTicks = (int)CombinedIntra;
	Tick += WholeTicks + CarryOverTicks;
	Intra = CombinedIntra - CarryOverTicks;
}

void CLightInput::ApplyHookOffset(int &Tick, float &Intra) const
{
	const int FastInputAmount = HookAmountMs();
	const float FastInputIntra = (FastInputAmount % 20) / 20.0f;
	int FastInputTicks = FastInputAmount / 20;
	const float CombinedIntra = Intra + FastInputIntra;
	float IntraRemainder = 0.0f;
	const float FinalIntra = std::modf(CombinedIntra, &IntraRemainder);
	FastInputTicks += (int)IntraRemainder;
	Tick += FastInputTicks;
	Intra = FinalIntra;
}

bool CLightInput::ApplySampleOffset(const CGameClient &GameClient, int ClientId, int &Tick, float &Intra) const
{
	const bool HookLogic = UseHookLogic(GameClient);
	const bool HookLinked = IsHookLinkedClient(GameClient, ClientId);
	const bool Local = GameClient.IsFastInputLocalClient(ClientId);
	const int FastInputTicks = HookLogic ? HookTickOffset() : PredictionTickOffset();
	if(HookLogic && (HookLinked || Others()))
		ApplyHookOffset(Tick, Intra);
	else if(Local)
		ApplyOffset(Amount(), Tick, Intra);
	else if(Others())
		Tick += FastInputTicks;
	else
		return false;
	return true;
}

int CLightInput::SampleMaxTick(const CGameClient &GameClient) const
{
	const int Offset = UseHookLogic(GameClient) ? HookTickOffset() : PredictionTickOffset();
	return GameClient.Client()->PredGameTick(g_Config.m_ClDummy) + Offset;
}

bool CLightInput::TryGetPredPos(const CGameClient &GameClient, int ClientId, int Tick, float Intra, int MaxTick, vec2 &OutPos) const
{
	if(!IsActive() || Tick <= 0)
		return false;
	if(GameClient.m_aClients[ClientId].m_aPredTick[(Tick - 1) % 200] != Tick - 1 ||
		GameClient.m_aClients[ClientId].m_aPredTick[Tick % 200] != Tick ||
		GameClient.m_aClients[ClientId].m_aPredTick[Tick % 200] > MaxTick)
		return false;

	OutPos = mix(GameClient.m_aClients[ClientId].m_aPredPos[(Tick - 1) % 200], GameClient.m_aClients[ClientId].m_aPredPos[Tick % 200], Intra);
	return true;
}

vec2 CLightInput::FastInputPos(const CGameClient &GameClient, int ClientId) const
{
	int PredTick = GameClient.Client()->PredGameTick(g_Config.m_ClDummy);
	float PredIntra = GameClient.Client()->PredIntraGameTick(g_Config.m_ClDummy);
	vec2 Pos = mix(GameClient.m_aClients[ClientId].m_PrevPredicted.m_Pos, GameClient.m_aClients[ClientId].m_Predicted.m_Pos, PredIntra);
	if(!ApplySampleOffset(GameClient, ClientId, PredTick, PredIntra))
		return Pos;
	TryGetPredPos(GameClient, ClientId, PredTick, PredIntra, SampleMaxTick(GameClient), Pos);
	return Pos;
}
