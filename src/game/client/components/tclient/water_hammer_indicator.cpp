#include "water_hammer_indicator.h"

#include <engine/shared/config.h>
#include <engine/shared/snapshot.h>

#include <game/client/gameclient.h>

void CWaterHammerIndicator::OnReset()
{
	m_Inputs.Reset();
}

void CWaterHammerIndicator::OnSnapshot(int Connection, int SnapshotTick, const CSnapshot *pSnapshot)
{
	std::array<bool, MAX_CLIENTS> aPresent{};
	for(int i = 0; i < pSnapshot->NumItems(); ++i)
	{
		if(pSnapshot->GetItemType(i) != NETOBJTYPE_PLAYERINFO ||
			pSnapshot->GetItemSize(i) != (int)sizeof(CNetObj_PlayerInfo))
			continue;
		const auto *pItem = pSnapshot->GetItem(i);
		const auto *pInfo = reinterpret_cast<const CNetObj_PlayerInfo *>(pItem->Data());
		if(pInfo->m_ClientId >= 0 && pInfo->m_ClientId < MAX_CLIENTS && pInfo->m_ClientId == pItem->Id())
			aPresent[pInfo->m_ClientId] = true;
	}
	m_Inputs.OnRoster(Connection, SnapshotTick, aPresent);
}

void CWaterHammerIndicator::OnPreInput(int Connection, const CNetMsg_Sv_PreInput &Message)
{
	m_Inputs.StorePreInput(Connection, Message.m_Owner, Message.m_IntendedTick, Message.m_WantedWeapon, Message.m_Fire);
}

void CWaterHammerIndicator::OnClientDrop(int Connection, int ClientId)
{
	m_Inputs.ResetClient(Connection, ClientId);
}

void CWaterHammerIndicator::OnDummyDisconnect()
{
	m_Inputs.ResetConnection(1);
	m_Inputs.ResetLocalInput();
}

void CWaterHammerIndicator::OnLocalInput(bool Hammer)
{
	const int Connection = !g_Config.m_ClDummy;
	const int ClientId = GameClient()->m_aLocalIds[Connection];
	if(Client()->DummyConnected())
		m_Inputs.RecordLocalInput(Connection, ClientId, Hammer);
}

void CWaterHammerIndicator::OnDummySwap()
{
	m_Inputs.ResetLocalInput();
	// m_DummyInput now contains the formerly controlled tee's actual input.
	// Its snapshot was invalidated before this callback, so do not consult it.
	OnLocalInput(false);
}

bool CWaterHammerIndicator::IsMarked(int ClientId) const
{
	if(!g_Config.m_TcWaterHammerHighlight || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;

	const int LocalClientId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalClientId < 0 || LocalClientId >= MAX_CLIENTS)
		return false;

	const auto &ClientData = GameClient()->m_aClients[ClientId];
	const bool Visible = GameClient()->m_Snap.m_aCharacters[ClientId].m_Active &&
		GameClient()->m_Snap.m_apPlayerInfos[ClientId] && GameClient()->m_Snap.m_apPrevPlayerInfos[ClientId];
	if(!TcIsWaterHammerTarget(ClientId, LocalClientId, ClientData.m_Active,
		ClientData.m_Spec || ClientData.m_Team == TEAM_SPECTATORS, Visible,
		GameClient()->m_Teams.Team(ClientId) == GameClient()->m_Teams.Team(LocalClientId)))
		return false;

	const CNetObj_PlayerInput *pLocalInput = nullptr;
	if(Client()->DummyConnected() && ClientId == GameClient()->m_aLocalIds[!g_Config.m_ClDummy])
	{
		// Never substitute a delayed server echo for missing local lifecycle input.
		if(!m_Inputs.HasLocalInput(!g_Config.m_ClDummy, ClientId, g_Config.m_ClDummyHammer != 0))
			return false;
		pLocalInput = g_Config.m_ClDummyHammer ? &GameClient()->m_HammerInput : &GameClient()->m_DummyInput;
	}
	return TcShouldMarkWaterHammer(ClientData.m_FreezeEnd, ClientData.m_LiveFrozen,
		ClientData.m_RenderCur.m_Weapon, m_Inputs.PreInput(g_Config.m_ClDummy, ClientId), pLocalInput);
}
