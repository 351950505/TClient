/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_LIGHT_ANCIENT_INPUT_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_LIGHT_ANCIENT_INPUT_H

#include <base/vmath.h>

#include <engine/client/enums.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls;
class CGameClient;

class CAncientInput : public CComponent
{
public:
	CAncientInput();

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;

	bool IsActive() const;
	float Amount() const;
	bool Others() const;
	int SelfTickOffset() const;
	int OthersTickOffset() const;
	CNetObj_PlayerInput &Input(int Dummy);
	const CNetObj_PlayerInput &Input(int Dummy) const;
	bool CheckNewInput(const CControls &Controls);
	void ApplyOffset(const CGameClient &GameClient, int ClientId, int &Tick, float &Intra) const;
	bool TryGetPredPos(const CGameClient &GameClient, int ClientId, int Tick, float Intra, vec2 &OutPos) const;
	vec2 FastInputPos(const CGameClient &GameClient, int ClientId) const;
	vec2 ApplyToRenderedPos(const CGameClient &GameClient, int ClientId, vec2 BasePos) const;

private:
	float TickMs(const CGameClient &GameClient) const;
	float OthersAmountMs() const;
	int LocalConn(const CGameClient &GameClient, int ClientId) const;
	float GetStabilizedAmountMs(const CGameClient &GameClient, int ClientId, float AmountMs) const;
	void SplitAmount(const CGameClient &GameClient, float AmountMs, int &Ticks, float &Intra) const;
	vec2 SamplePos(const CGameClient &GameClient, int ClientId, float AmountMs, bool SafeGuard) const;

	CNetObj_PlayerInput m_aInput[NUM_DUMMIES] = {};
	mutable float m_aSmoothedAmount[NUM_DUMMIES] = {};
	mutable int m_aSmoothedTick[NUM_DUMMIES] = {};
	mutable int m_aLastDirection[NUM_DUMMIES] = {};
	mutable int m_aLastFlipTick[NUM_DUMMIES] = {};
};

#endif
