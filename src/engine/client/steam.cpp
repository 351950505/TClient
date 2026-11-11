#include <base/dbg.h>
#include <base/net.h>
#include <base/str.h>

#include <engine/shared/config.h>
#include <engine/steam.h>

#include <steam/steam_api_flat.h>

class CSteam : public ISteam
{
	HSteamPipe m_SteamPipe;
	ISteamApps *m_pSteamApps;
	ISteamFriends *m_pSteamFriends;
	char m_aPlayerName[16];
	bool m_GotConnectAddr;
	NETADDR m_ConnectAddr;
	// [STEAM-HIDE] 运行中隐藏后不再调用任何 SteamAPI 函数（句柄已失效）
	bool m_Hidden = false;
	bool m_Shutdown = false;

public:
	CSteam()
	{
		SteamAPI_ManualDispatch_Init();
		m_SteamPipe = SteamAPI_GetHSteamPipe();
		m_pSteamApps = SteamAPI_SteamApps_v008();
		m_pSteamFriends = SteamAPI_SteamFriends_v017();

		ReadLaunchCommandLine();
		str_copy(m_aPlayerName, SteamAPI_ISteamFriends_GetPersonaName(m_pSteamFriends));
	}
	~CSteam() override
	{
		if(!m_Shutdown)
			SteamAPI_Shutdown();
	}

	void ParseConnectString(const char *pConnect)
	{
		if(pConnect[0])
		{
			NETADDR Connect;
			if(net_host_lookup(pConnect, &Connect, NETTYPE_ALL) == 0)
			{
				m_ConnectAddr = Connect;
				m_GotConnectAddr = true;
			}
			else
			{
				dbg_msg("steam", "got unparsable connect string: '%s'", pConnect);
			}
		}
	}

	void ReadLaunchCommandLine()
	{
		char aConnect[NETADDR_MAXSTRSIZE];
		int ConnectSize = SteamAPI_ISteamApps_GetLaunchCommandLine(m_pSteamApps, aConnect, sizeof(aConnect));
		if(ConnectSize >= NETADDR_MAXSTRSIZE)
		{
			ConnectSize = NETADDR_MAXSTRSIZE - 1;
		}
		aConnect[ConnectSize] = 0;
		m_GotConnectAddr = false;
		ParseConnectString(aConnect);
	}

	void OnGameRichPresenceJoinRequested(GameRichPresenceJoinRequested_t *pEvent)
	{
		ParseConnectString(pEvent->m_aRGCHConnect);
	}

	const char *GetPlayerName() override
	{
		return m_aPlayerName;
	}

	const NETADDR *GetConnectAddress() override
	{
		if(m_GotConnectAddr)
		{
			return &m_ConnectAddr;
		}
		else
		{
			return nullptr;
		}
	}
	void ClearConnectAddress() override
	{
		m_GotConnectAddr = false;
	}

	void Update() override
	{
		if(m_Hidden)
			return;
		SteamAPI_ManualDispatch_RunFrame(m_SteamPipe);
		CallbackMsg_t Callback;
		while(SteamAPI_ManualDispatch_GetNextCallback(m_SteamPipe, &Callback))
		{
			switch(Callback.m_iCallback)
			{
			case NewUrlLaunchParameters_t::k_iCallback:
				ReadLaunchCommandLine();
				break;
			case GameRichPresenceJoinRequested_t::k_iCallback:
				OnGameRichPresenceJoinRequested((GameRichPresenceJoinRequested_t *)Callback.m_pubParam);
				break;
			default:
				if(g_Config.m_Debug)
				{
					dbg_msg("steam/dbg", "unhandled callback id=%d", Callback.m_iCallback);
				}
			}
			SteamAPI_ManualDispatch_FreeLastCallback(m_SteamPipe);
		}
	}
	void ClearGameInfo() override
	{
		if(m_Hidden)
			return;
		SteamAPI_ISteamFriends_ClearRichPresence(m_pSteamFriends);
	}
	void SetGameInfo(const NETADDR &ServerAddr, const char *pMapName, bool AnnounceAddr) override
	{
		if(m_Hidden)
			return;
		if(AnnounceAddr)
		{
			char aServerAddr[NETADDR_MAXSTRSIZE];
			net_addr_str(&ServerAddr, aServerAddr, sizeof(aServerAddr), true);
			SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, "connect", aServerAddr);
			SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, "steam_player_group", aServerAddr);
		}

		SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, "map", pMapName);
		SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, "status", pMapName);
		SteamAPI_ISteamFriends_SetRichPresence(m_pSteamFriends, "steam_display", "#Status");
	}
	void HideFromSteam() override
	{
		if(m_Hidden)
			return;
		m_Hidden = true;
		SteamAPI_ISteamFriends_ClearRichPresence(m_pSteamFriends);
		SteamAPI_Shutdown();
		m_Shutdown = true;
	}
};

class CSteamStub : public ISteam
{
	const char *GetPlayerName() override { return nullptr; }
	const NETADDR *GetConnectAddress() override { return nullptr; }
	void ClearConnectAddress() override {}
	void Update() override {}
	void ClearGameInfo() override {}
	void SetGameInfo(const NETADDR &ServerAddr, const char *pMapName, bool AnnounceAddr) override {}
	void HideFromSteam() override {}
};

static CSteam *CreateRealSteam()
{
	if(!SteamAPI_Init())
		return nullptr;
	return new CSteam();
}

// [STEAM-HIDE] 运行中切换 Steam 连接：
// 隐藏时关闭 SteamAPI，取消隐藏时尝试重新 Init，避免必须重启客户端。
class CSteamProxy : public ISteam
{
	ISteam *m_pImpl;
	bool m_Hidden;
	bool m_Real;

public:
	CSteamProxy(ISteam *pImpl, bool Hidden, bool Real) :
		m_pImpl(pImpl), m_Hidden(Hidden), m_Real(Real) {}
	~CSteamProxy() override { delete m_pImpl; }

	const char *GetPlayerName() override { return m_Hidden ? nullptr : m_pImpl->GetPlayerName(); }
	const NETADDR *GetConnectAddress() override { return m_Hidden ? nullptr : m_pImpl->GetConnectAddress(); }
	void ClearConnectAddress() override
	{
		if(!m_Hidden)
			m_pImpl->ClearConnectAddress();
	}
	void Update() override
	{
		if(!m_Hidden)
			m_pImpl->Update();
	}
	void ClearGameInfo() override
	{
		if(!m_Hidden)
			m_pImpl->ClearGameInfo();
	}
	void SetGameInfo(const NETADDR &ServerAddr, const char *pMapName, bool AnnounceAddr) override
	{
		if(!m_Hidden)
			m_pImpl->SetGameInfo(ServerAddr, pMapName, AnnounceAddr);
	}
	void HideFromSteam() override
	{
		if(m_Hidden)
			return;
		m_pImpl->HideFromSteam();
		m_Hidden = true;
	}
	bool Reconnect() override
	{
		if(m_Real && !m_Hidden)
			return true;
		CSteam *pNew = CreateRealSteam();
		if(!pNew)
			return false;
		delete m_pImpl;
		m_pImpl = pNew;
		m_Hidden = false;
		m_Real = true;
		return true;
	}
};

ISteam *CreateSteam(bool HideFromSteam)
{
	ISteam *pImpl = nullptr;
	bool Real = false;
	if(!HideFromSteam)
	{
		pImpl = CreateRealSteam();
		Real = pImpl != nullptr;
	}
	if(!pImpl)
		pImpl = new CSteamStub();
	return new CSteamProxy(pImpl, HideFromSteam, Real);
}
