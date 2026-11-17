/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_LIGHT_LIGHT_INPUT_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_LIGHT_LIGHT_INPUT_H

#include <base/vmath.h>

#include <engine/client/enums.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls;
class CGameClient;

class CLightInput : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	bool IsActive() const;
	float Amount() const;
	bool Others() const;
	int PredictionTickOffset() const;
	int HookAmountMs() const;
	int HookTickOffset() const;
	bool UseHookLogic(const CGameClient &GameClient) const;
	bool IsHookLinkedClient(const CGameClient &GameClient, int ClientId) const;
	CNetObj_PlayerInput &Input(int Dummy);
	const CNetObj_PlayerInput &Input(int Dummy) const;
	bool CheckNewInput(const CControls &Controls);
	void ResetActions();
	bool ApplySampleOffset(const CGameClient &GameClient, int ClientId, int &Tick, float &Intra) const;
	int SampleMaxTick(const CGameClient &GameClient) const;
	bool TryGetPredPos(const CGameClient &GameClient, int ClientId, int Tick, float Intra, int MaxTick, vec2 &OutPos) const;
	vec2 FastInputPos(const CGameClient &GameClient, int ClientId) const;

private:
	void ApplyOffset(float OffsetTicks, int &Tick, float &Intra) const;
	void ApplyHookOffset(int &Tick, float &Intra) const;
	bool IsHookClient(const CGameClient &GameClient, int ClientId) const;

	CNetObj_PlayerInput m_aInput[NUM_DUMMIES] = {};
	bool m_HookAction = false;
	bool m_FireAction = false;
};

#endif
