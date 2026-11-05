#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_WATER_HAMMER_INDICATOR_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_WATER_HAMMER_INDICATOR_H

#include "water_hammer_indicator_logic.h"

#include <game/client/component.h>

class CSnapshot;

class CWaterHammerIndicator : public CComponent
{
	CTcWaterHammerInputCache m_Inputs;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;

	void OnSnapshot(int Connection, int SnapshotTick, const CSnapshot *pSnapshot);
	void OnPreInput(int Connection, const CNetMsg_Sv_PreInput &Message);
	void OnClientDrop(int Connection, int ClientId);
	void OnDummyDisconnect();
	void OnLocalInput(bool Hammer);
	void OnDummySwap();
	bool IsMarked(int ClientId) const;
};

#endif // GAME_CLIENT_COMPONENTS_TCLIENT_WATER_HAMMER_INDICATOR_H
