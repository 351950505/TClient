#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_WATER_HAMMER_INDICATOR_LOGIC_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_WATER_HAMMER_INDICATOR_LOGIC_H

#include <engine/shared/protocol.h>
#include <generated/protocol.h>

#include <array>

struct STcWaterHammerInputState
{
	bool m_Received = false;
	int m_WantedWeapon = 0;
	int m_Fire = 0;
};

inline bool TcIsWaterHammerTarget(int ClientId, int LocalClientId, bool Active, bool Spectator, bool Visible, bool SameTeam)
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS &&
		LocalClientId >= 0 && LocalClientId < MAX_CLIENTS && ClientId != LocalClientId &&
		Active && !Spectator && Visible && SameTeam;
}

inline bool TcShouldMarkWaterHammer(int FreezeEnd, bool LiveFrozen, int CurrentWeapon, const STcWaterHammerInputState &PreInput, const CNetObj_PlayerInput *pLocalInput)
{
	if(!pLocalInput && !PreInput.m_Received)
		return false;

	const int WantedWeapon = pLocalInput ? pLocalInput->m_WantedWeapon : PreInput.m_WantedWeapon;
	const int Fire = pLocalInput ? pLocalInput->m_Fire : PreInput.m_Fire;
	return (FreezeEnd != 0 || LiveFrozen) &&
		(WantedWeapon == WEAPON_HAMMER + 1 || CurrentWeapon == WEAPON_HAMMER) && (Fire & 1) != 0;
}

// Frozen katana is a drawing override, never a change to the shared character.
inline CNetObj_Character TcFrozenKatanaRenderCharacter(const CNetObj_Character &Character, bool FrozenKatana)
{
	CNetObj_Character RenderCharacter = Character;
	if(FrozenKatana)
		RenderCharacter.m_Weapon = WEAPON_NINJA;
	return RenderCharacter;
}

// Pre-input is sent only when it changes. Visibility and control swaps must not
// end its lifetime; only a connection reset or a confirmed roster removal can.
class CTcWaterHammerInputCache
{
	struct CClientState
	{
		STcWaterHammerInputState m_Input{};
		int m_IntendedTick = -1;
		bool m_SeenPresent = false;
	};
	struct CConnectionState
	{
		std::array<CClientState, MAX_CLIENTS> m_aClients{};
		int m_LastSnapshotTick = -1;
	};
	std::array<CConnectionState, 2> m_aConnections{};
	int m_LocalConnection = -1;
	int m_LocalClientId = -1;
	std::array<bool, 2> m_aLocalInputReceived{};

	static bool ValidConnection(int Connection) { return Connection >= 0 && Connection < 2; }
	static bool ValidClient(int ClientId) { return ClientId >= 0 && ClientId < MAX_CLIENTS; }

public:
	void ResetLocalInput()
	{
		m_LocalConnection = -1;
		m_LocalClientId = -1;
		m_aLocalInputReceived = {};
	}

	void Reset()
	{
		m_aConnections = {};
		ResetLocalInput();
	}

	void ResetConnection(int Connection)
	{
		if(!ValidConnection(Connection))
			return;
		m_aConnections[Connection] = {};
		if(m_LocalConnection == Connection)
			ResetLocalInput();
	}

	void ResetClient(int Connection, int ClientId)
	{
		if(!ValidConnection(Connection) || !ValidClient(ClientId))
			return;
		m_aConnections[Connection].m_aClients[ClientId] = {};
		if(m_LocalConnection == Connection && m_LocalClientId == ClientId)
			ResetLocalInput();
	}

	void OnRoster(int Connection, int SnapshotTick, const std::array<bool, MAX_CLIENTS> &aPresent)
	{
		if(!ValidConnection(Connection))
			return;
		auto &State = m_aConnections[Connection];
		if(SnapshotTick <= State.m_LastSnapshotTick)
			return;
		State.m_LastSnapshotTick = SnapshotTick;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			auto &Client = State.m_aClients[ClientId];
			if(aPresent[ClientId])
				Client.m_SeenPresent = true;
			else if(Client.m_SeenPresent)
			{
				// IntendedTick is a prediction time, not an occupant identity.
				ResetClient(Connection, ClientId);
			}
		}
	}

	void StorePreInput(int Connection, int ClientId, int IntendedTick, int WantedWeapon, int Fire)
	{
		if(!ValidConnection(Connection) || !ValidClient(ClientId))
			return;
		auto &Client = m_aConnections[Connection].m_aClients[ClientId];
		Client.m_Input = {true, WantedWeapon, Fire};
		Client.m_IntendedTick = IntendedTick;
	}

	STcWaterHammerInputState PreInput(int Connection, int ClientId) const
	{
		return ValidConnection(Connection) && ValidClient(ClientId) ?
			m_aConnections[Connection].m_aClients[ClientId].m_Input : STcWaterHammerInputState{};
	}

	void RecordLocalInput(int Connection, int ClientId, bool Hammer)
	{
		if(!ValidConnection(Connection) || !ValidClient(ClientId))
			return;
		if(m_LocalConnection != Connection || m_LocalClientId != ClientId)
		{
			ResetLocalInput();
			m_LocalConnection = Connection;
			m_LocalClientId = ClientId;
		}
		m_aLocalInputReceived[Hammer] = true;
	}

	bool HasLocalInput(int Connection, int ClientId, bool Hammer) const
	{
		return ValidConnection(Connection) && ValidClient(ClientId) &&
			m_LocalConnection == Connection && m_LocalClientId == ClientId && m_aLocalInputReceived[Hammer];
	}
};

#endif // GAME_CLIENT_COMPONENTS_TCLIENT_WATER_HAMMER_INDICATOR_LOGIC_H
