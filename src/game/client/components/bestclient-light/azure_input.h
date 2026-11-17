/* Copyright © 2026 BestProject Team */
#ifndef GAME_CLIENT_COMPONENTS_BESTCLIENT_LIGHT_AZURE_INPUT_H
#define GAME_CLIENT_COMPONENTS_BESTCLIENT_LIGHT_AZURE_INPUT_H

#include <base/vmath.h>

#include <engine/client/enums.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls;
class CGameClient;

class CAzureInput : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	bool IsActive() const;
	float Amount() const;
	bool Others() const;
	int SelfTickOffset() const;
	int OthersTickOffset() const;
	CNetObj_PlayerInput &Input(int Dummy);
	const CNetObj_PlayerInput &Input(int Dummy) const;
	bool CheckNewInput(const CControls &Controls);
	void ApplySampleOffset(const CGameClient &GameClient, int ClientId, int &Tick, float &Intra) const;
	bool TryGetPredPos(const CGameClient &GameClient, int ClientId, int Tick, float Intra, int MaxTick, vec2 &OutPos) const;
	vec2 FastInputPos(const CGameClient &GameClient, int ClientId) const;

private:
	void ApplyOffset(float OffsetTicks, int &Tick, float &Intra) const;

	CNetObj_PlayerInput m_aInput[NUM_DUMMIES] = {};
};

#endif
