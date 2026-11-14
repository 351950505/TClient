#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_HUD_LOGIC_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_FROZEN_HUD_LOGIC_H

#include <array>
#include <cstddef>

enum class ETcFrozenHudState
{
	UNKNOWN,
	UNFROZEN,
	FROZEN,
};

// Character presence is from SNAP_CURRENT, not the two-snapshot render m_Active.
// FreezeEnd alone preserves ordinary/deep freeze semantics; live freeze and
// IN_FREEZE are not equivalent to being unable to move under ordinary freeze.
inline ETcFrozenHudState TcFrozenHudState(bool HasFreshCharacter, bool HasExtendedData, int FreezeEnd)
{
	if(!HasFreshCharacter || !HasExtendedData)
		return ETcFrozenHudState::UNKNOWN;
	return FreezeEnd == -1 || FreezeEnd > 0 ? ETcFrozenHudState::FROZEN : ETcFrozenHudState::UNFROZEN;
}

inline int TcFrozenHudTeamClientId(int LocalClientId, bool Spectating, int TargetId, int MaxClients)
{
	if(Spectating && TargetId >= 0 && TargetId < MaxClients)
		return TargetId;
	if(LocalClientId >= 0 && LocalClientId < MaxClients)
		return LocalClientId;
	return -1;
}

struct STcFrozenHudCounts
{
	int m_NumInTeam = 0;
	int m_KnownUnfrozen = 0;
	int m_Frozen = 0;
	int m_Unknown = 0;

	void Add(ETcFrozenHudState State)
	{
		++m_NumInTeam;
		if(State == ETcFrozenHudState::UNFROZEN)
			++m_KnownUnfrozen;
		else if(State == ETcFrozenHudState::FROZEN)
			++m_Frozen;
		else
			++m_Unknown;
	}

	bool NotifyWhenLast() const
	{
		return m_NumInTeam > 1 && m_KnownUnfrozen == 1 && m_Unknown == 0;
	}
};

template<std::size_t MaxClients>
struct STcFrozenHudFrame
{
	STcFrozenHudCounts m_Counts;
	std::array<bool, MaxClients> m_aIncluded{};
	std::array<ETcFrozenHudState, MaxClients> m_aStates{};

	void AddPlayer(int ClientId, ETcFrozenHudState State)
	{
		if(ClientId < 0 || static_cast<std::size_t>(ClientId) >= MaxClients || m_aIncluded[ClientId])
			return;
		m_aIncluded[ClientId] = true;
		m_aStates[ClientId] = State;
		m_Counts.Add(State);
	}
};

// Keep client-id order without overflow. With overflow prioritize known
// unfrozen tees, then explicitly unknown tees, and finally known frozen tees.
inline int TcFrozenHudOverflowPass(ETcFrozenHudState State)
{
	if(State == ETcFrozenHudState::UNFROZEN)
		return 0;
	return State == ETcFrozenHudState::UNKNOWN ? 1 : 2;
}

#endif
