/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#include "teamscore.h"

#include <base/dbg.h>

#include <engine/shared/config.h>

CTeamsCore::CTeamsCore()
{
	Reset();
}

bool CTeamsCore::SameTeam(int ClientId1, int ClientId2) const
{
	// [TClient] 审计修复（2026-11-20 P2）：所有入口都可能收到 -1（纯观战的
	// LocalClientId）或越界 id，裸索引会读相邻内存；无效 id 一律视为不同队。
	if(ClientId1 < 0 || ClientId1 >= MAX_CLIENTS || ClientId2 < 0 || ClientId2 >= MAX_CLIENTS)
		return false;
	return m_aTeam[ClientId1] == TeamSuper() || m_aTeam[ClientId2] == TeamSuper() || m_aTeam[ClientId1] == m_aTeam[ClientId2];
}

int CTeamsCore::Team(int ClientId) const
{
	// [TClient] 审计修复（2026-11-20 P2）：无效 id 返回 -1（无队伍语义）
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return -1;
	return m_aTeam[ClientId];
}

void CTeamsCore::Team(int ClientId, int Team)
{
	dbg_assert(Team >= TEAM_FLOCK && Team < NUM_DDRACE_TEAMS, "Invalid Team: %d", Team);
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid ClientId: %d", ClientId);
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	m_aTeam[ClientId] = Team;
}

bool CTeamsCore::CanKeepHook(int ClientId1, int ClientId2) const
{
	if(ClientId1 < 0 || ClientId1 >= MAX_CLIENTS || ClientId2 < 0 || ClientId2 >= MAX_CLIENTS)
		return false;
	if(m_aTeam[ClientId1] == TeamSuper() || m_aTeam[ClientId2] == TeamSuper() || ClientId1 == ClientId2)
		return true;
	return m_aTeam[ClientId1] == m_aTeam[ClientId2];
}

bool CTeamsCore::CanCollide(int ClientId1, int ClientId2) const
{
	if(ClientId1 < 0 || ClientId1 >= MAX_CLIENTS || ClientId2 < 0 || ClientId2 >= MAX_CLIENTS)
		return false;
	if(m_aTeam[ClientId1] == TeamSuper() || m_aTeam[ClientId2] == TeamSuper() || ClientId1 == ClientId2)
		return true;
	if(m_aIsSolo[ClientId1] || m_aIsSolo[ClientId2])
		return false;
	return m_aTeam[ClientId1] == m_aTeam[ClientId2];
}

void CTeamsCore::Reset()
{
	m_NumDDRaceTeams = NUM_DDRACE_TEAMS;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
			m_aTeam[i] = i;
		else
			m_aTeam[i] = TEAM_FLOCK;
		m_aIsSolo[i] = false;
	}
}

void CTeamsCore::SetSolo(int ClientId, bool Value)
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "Invalid client id");
	m_aIsSolo[ClientId] = Value;
}

bool CTeamsCore::GetSolo(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	return m_aIsSolo[ClientId];
}
