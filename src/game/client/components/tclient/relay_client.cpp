/* TClient 信令/中继客户端，见 relay_client.h
   P2P 直连：信令把我们这一组的成员公网地址（PEER）推下来，双方互向对方
   公网地址发探针把 NAT 映射打通；打通后 ping 载荷直接点对点发送，不经信令。
   任一方 NAT 是对称型导致打不通时，自动回退信令中转。 */
#include "relay_client.h"

#include "relay_ws_transport.h"

#include <base/log.h>
#include <base/net.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>

#include <cstdlib>

CRelayClient::CRelayClient() = default;
CRelayClient::~CRelayClient() = default;

void CRelayClient::OnShutdown()
{
	OnReset();
	if(m_SocketValid)
	{
		net_udp_close(m_Socket);
		m_Socket = nullptr;
		m_SocketValid = false;
	}
}

void CRelayClient::ConRelayStatus(IConsole::IResult *pResult, void *pUserData)
{
	CRelayClient *pSelf = (CRelayClient *)pUserData;
	char aBuf[256];
	pSelf->FormatStatus(aBuf, sizeof(aBuf));
	pSelf->GameClient()->m_Chat.Echo(aBuf);
}

void CRelayClient::FormatStatus(char *pBuf, int Size) const
{
	char aGame[64];
	if(!GameServerAddr(aGame, sizeof(aGame)))
		str_copy(aGame, "(未连接游戏服)");
	if(g_Config.m_TcRelayCfOnly)
	{
		str_format(pBuf, Size, "信令: Cloudflare | 服 %s | %s | %s", aGame,
			!CfEnabled() ? "关闭（仅本地）" : m_CfOnline ? "在线" :
								       "连接中（仅本地）",
			m_CfError.empty() ? "CF-only，无 UDP 回退" : m_CfError.c_str());
		return;
	}
	const float Age = Client()->LocalTime() - m_LastServerReply;
	const auto Transport = PreferredTransport();
	const char *pTransport = Transport == RelayRouting::ETransport::UDP        ? "阿里云 UDP" :
				 Transport == RelayRouting::ETransport::CLOUDFLARE ? "Cloudflare" :
										     "仅本地";
	str_format(pBuf, Size,
		"信令: %s | 服 %s | UDP %s (回执 %.1fs 前) | CF %s | 待发 %d | %s",
		pTransport, aGame, UdpOnline() ? "在线" : "未连接",
		Age < 1e8f ? Age : -1.0f,
		!CfEnabled() ? "关闭" : m_CfOnline ? "在线" :
						     "未连接",
		(int)m_vSendQueue.size(),
		CfEnabled() && !m_CfError.empty() ? m_CfError.c_str() : m_aFailure);
}

void CRelayClient::OnConsoleInit()
{
	Console()->Register("tc_relay_status", "", CFGFLAG_CLIENT, ConRelayStatus, this, "Show signal relay / p2p status");
}

void CRelayClient::OnInit()
{
	if(!g_Config.m_TcRelay)
		return;
	if(!m_aUid[0])
	{
		unsigned aSeed[2];
		secure_random_fill(aSeed, sizeof(aSeed));
		str_format(m_aUid, sizeof(m_aUid), "%08x%08x", aSeed[0], aSeed[1]);
	}
	if(g_Config.m_TcRelayCfOnly)
	{
		if(m_SocketValid)
			net_udp_close(m_Socket);
		m_Socket = nullptr;
		m_SocketValid = false;
		m_RelayAddrValid = false;
		return;
	}
	if(net_addr_from_str(&m_RelayAddr, g_Config.m_TcRelayAddr) != 0)
	{
		log_error("relay", "invalid tc_relay_addr '%s', relay disabled", g_Config.m_TcRelayAddr);
		return;
	}
	m_RelayAddrValid = true;
	if(m_SocketValid)
	{
		net_udp_close(m_Socket);
		m_SocketValid = false;
	}
	NETADDR Bind = {0};
	Bind.type = NETTYPE_ALL;
	m_Socket = net_udp_create(Bind);
	m_SocketValid = m_Socket != nullptr; // net_udp_create 失败时返回 nullptr
	if(!m_SocketValid)
		log_error("relay", "failed to create udp socket, relay disabled");
	// Keep the same random uid across UDP socket retries and CF reconnects.
}

void CRelayClient::LogOnce(const char *pReason, const char *pDetail)
{
	// 心跳每 10s（离线 2s）跑一次，逐次打印会把 F1 控制台刷满。这里按"原因"
	// 分别记忆上次的细节：同一原因+同一细节重复发生直接丢弃；首次注册、切服、
	// 掉线恢复、失败原因改变则各留一条。
	const char *pDet = pDetail ? pDetail : "";
	for(int i = 0; i < m_NumLogMemo; i++)
	{
		if(str_comp(m_aLogMemo[i].m_aReason, pReason) != 0)
			continue;
		if(str_comp(m_aLogMemo[i].m_aDetail, pDet) == 0)
			return;
		str_copy(m_aLogMemo[i].m_aDetail, pDet);
		log_info("relaydiag", "%s%s%s", pReason, pDet[0] ? ": " : "", pDet);
		return;
	}
	if(m_NumLogMemo < (int)(sizeof(m_aLogMemo) / sizeof(m_aLogMemo[0])))
	{
		str_copy(m_aLogMemo[m_NumLogMemo].m_aReason, pReason);
		str_copy(m_aLogMemo[m_NumLogMemo].m_aDetail, pDet);
		m_NumLogMemo++;
	}
	log_info("relaydiag", "%s%s%s", pReason, pDet[0] ? ": " : "", pDet);
}

void CRelayClient::OnReset()
{
	if(m_pCfTransport)
		m_pCfTransport->Stop();
	m_CfOnline = false;
	m_CfRoom.clear();
	m_CfUrl.clear();
	m_CfError.clear();
	m_LastTransport = RelayRouting::ETransport::NONE;
	m_vSendQueue.clear();
	m_vPeers.clear();
	m_LastServerReply = -1e9f;
	m_LastRegTime = -1e9f;
	m_LastRenderTime = -1e9f;
	m_aGameAddr[0] = 0;
	str_copy(m_aFailure, "等待注册");
	m_NumLogMemo = 0;
	m_aLastRecvPayload[0] = 0;
	m_LastRecvPayloadTime = -1e9f;
}

bool CRelayClient::UdpOnline() const
{
	return !g_Config.m_TcRelayCfOnly && m_SocketValid && Client()->LocalTime() - m_LastServerReply < 30.0f;
}

bool CRelayClient::CfEnabled() const
{
#if defined(CONF_FAMILY_WINDOWS)
	return g_Config.m_TcRelay && g_Config.m_TcRelayCf;
#else
	return false;
#endif
}

RelayRouting::ETransport CRelayClient::PreferredTransport() const
{
	return RelayRouting::PreferredTransport(UdpOnline(), CfEnabled(), m_CfOnline, g_Config.m_TcRelayCfOnly != 0);
}

bool CRelayClient::Online() const
{
	return g_Config.m_TcRelay && PreferredTransport() != RelayRouting::ETransport::NONE;
}

bool CRelayClient::Available() const
{
	if(!g_Config.m_TcRelay)
		return false;
	if(Online())
		return true;
	if(g_Config.m_TcRelayCfOnly != 0)
		return false;
	if(m_SocketValid)
		for(const SPeer &Peer : m_vPeers)
			if(PeerDirect(Peer))
				return true;
	return false;
}

void CRelayClient::DeliverPayload(const char *pPayload, const char *pSource)
{
	if(str_length(pPayload) > 160 || !str_startswith(pPayload, "\xE2\x9F\xA8"))
		return;
	const float Now = Client()->LocalTime();
	// CF is deduplicated by sender/session/sequence in the transport. Do not
	// suppress distinct legitimate signals just because their payloads match.
	if(str_comp(pSource, "CF") != 0 && str_comp(m_aLastRecvPayload, pPayload) == 0 && Now - m_LastRecvPayloadTime < 1.0f)
		return;
	str_copy(m_aLastRecvPayload, pPayload);
	m_LastRecvPayloadTime = Now;
	LogOnce("recv payload", pSource);
	GameClient()->m_PingWheel.OnRelayPayload(pPayload);
}

void CRelayClient::PumpCloudflare()
{
	if(!CfEnabled() || !m_aGameAddr[0])
	{
		if(m_pCfTransport && !m_CfRoom.empty())
			m_pCfTransport->Stop();
		m_CfOnline = false;
		m_CfRoom.clear();
		m_CfUrl.clear();
		m_CfError.clear();
		return;
	}
	const char *pKey = std::getenv("CF_WS_PROTOTYPE_KEY");
	if(!pKey || !pKey[0])
	{
		if(m_pCfTransport && !m_CfRoom.empty())
			m_pCfTransport->Stop();
		m_CfRoom.clear();
		m_CfUrl.clear();
		m_CfOnline = false;
		m_CfError = "缺少 CF 试验访问密钥，请使用测试启动器";
		LogOnce("CF backup unavailable", m_CfError.c_str());
		return;
	}
	if(!m_pCfTransport)
		m_pCfTransport = std::make_unique<RelayWs::CTransport>();
	if(m_CfRoom != m_aGameAddr || m_CfUrl != g_Config.m_TcRelayCfUrl)
	{
		m_CfOnline = false;
		m_CfRoom = m_aGameAddr;
		m_CfUrl = g_Config.m_TcRelayCfUrl;
		m_pCfTransport->Start(m_CfUrl, m_CfRoom, m_aUid, pKey);
	}
	for(const auto &Event : m_pCfTransport->Drain())
	{
		if(Event.m_Type == RelayWs::CEvent::EType::SIGNAL)
			DeliverPayload(Event.m_Payload.c_str(), "CF");
	}
	const auto Status = m_pCfTransport->Status();
	if(m_CfOnline != Status.m_Online)
		LogOnce("CF backup state", Status.m_Online ? "connected" : "disconnected");
	m_CfOnline = Status.m_Online;
	m_CfError = Status.m_Error;
	if(!m_CfError.empty())
		LogOnce("CF backup error", m_CfError.c_str());
}

bool CRelayClient::PeerDirect(const SPeer &Peer) const
{
	// P2P disabled means no direct route, including stale/probed peers.
	return g_Config.m_TcRelayP2p && Client()->LocalTime() - Peer.m_LastDirect < 10.0f;
}

bool CRelayClient::GameServerAddr(char *pBuf, int Size) const
{
	// 未连服务器时不注册：否则所有"未进服"的客户端会被分到同一个 offline
	// 分组，互相推送无意义的 PEER，还会让外人在列表里看到你。
	if(!RelayRouting::CanRegister(Client()->State(), Client()->ServerAddress().type))
		return false;
	net_addr_str(&Client()->ServerAddress(), pBuf, Size, true);
	return true;
}

bool CRelayClient::SendReg()
{
	if(g_Config.m_TcRelayCfOnly)
		return false;
	char aGameAddr[64];
	if(!GameServerAddr(aGameAddr, sizeof(aGameAddr)))
	{
		str_copy(m_aFailure, "未连接游戏服");
		return false;
	}
	if(str_comp(m_aGameAddr, aGameAddr) != 0)
	{
		str_copy(m_aGameAddr, aGameAddr);
		m_LastServerReply = -1e9f;
		m_vPeers.clear();
		LogOnce("game server changed", m_aGameAddr);
	}
	char aBuf[160];
	str_format(aBuf, sizeof(aBuf), "REG %s %s %s", aGameAddr, m_aUid, g_Config.m_PlayerName);
	m_LastRegTime = Client()->LocalTime();
	if(net_udp_send(m_Socket, &m_RelayAddr, aBuf, str_length(aBuf) + 1) == -1)
	{
		str_copy(m_aFailure, "注册发送失败");
		net_udp_close(m_Socket);
		m_Socket = nullptr;
		m_SocketValid = false;
		m_LastSocketAttempt = Client()->LocalTime();
		m_vPeers.clear();
		m_vSendQueue.clear();
		m_LastServerReply = -1e9f;
		log_error("relaydiag", "failed to send REG for %s", aGameAddr);
		return false;
	}
	str_copy(m_aFailure, "注册已发送，等待回执");
	// 成功路径也走 LogOnce：心跳重复成功只留第一条，掉线后重新成功会再留一条。
	LogOnce("registered", aGameAddr);
	return true;
}

void CRelayClient::HandlePeer(const char *pUid, const char *pAddrStr)
{
	NETADDR Addr;
	if(net_addr_from_str(&Addr, pAddrStr) != 0)
		return;
	for(SPeer &Peer : m_vPeers)
	{
		if(str_comp(Peer.m_aUid, pUid) == 0)
		{
			// 信令侧看到的地址是最新 NAT 映射；已打通时不覆盖（对方真实
			// 回包来源可能与此不同，NAT 学习结果更准）
			if(!PeerDirect(Peer))
				Peer.m_Addr = Addr;
			Peer.m_LastSeen = Client()->LocalTime();
			return;
		}
	}
	if(m_vPeers.size() >= 16)
		return;
	SPeer Peer;
	str_copy(Peer.m_aUid, pUid);
	Peer.m_Addr = Addr;
	Peer.m_LastSeen = Client()->LocalTime();
	m_vPeers.push_back(Peer);
	LogOnce("peer added", pUid);
}

void CRelayClient::Broadcast(const char *pPayload)
{
	if(!g_Config.m_TcRelay || str_length(pPayload) > 160)
		return;
	if(PreferredTransport() == RelayRouting::ETransport::CLOUDFLARE)
	{
		if(m_pCfTransport->Send(pPayload))
			LogOnce("CF signal queued", nullptr);
		return;
	}
	if(g_Config.m_TcRelayCfOnly || !m_SocketValid)
		return;
	// P2P 直发默认关闭（2026-10-31 用户反馈）：走 P2P 的接收方只有已打通的
	// 客户端玩家，其他人看不到 ping。tc_relay_p2p=1 可临时开回测试。
	const int SentDirect = RelayRouting::Broadcast(g_Config.m_TcRelayP2p != 0, Online(), m_vPeers, [this](const SPeer &Peer) { return PeerDirect(Peer); }, [this, pPayload](const SPeer &Peer) {
			char aBuf[192];
			str_format(aBuf, sizeof(aBuf), "D %s", pPayload);
			net_udp_send(m_Socket, &Peer.m_Addr, aBuf, str_length(aBuf) + 1); }, [this, pPayload] {
			if(m_vSendQueue.size() < 8) // 限深：ping 频率远低于此
			{
				SQueued Item;
				Item.m_Payload = pPayload;
				Item.m_UseRelay = true;
				Item.m_QueuedAt = Client()->LocalTime();
				m_vSendQueue.push_back(Item);
				LogOnce("broadcast queued (relay)", nullptr);
			} });
	if(SentDirect > 0)
	{
		char aPeers[16];
		str_format(aPeers, sizeof(aPeers), "%d", SentDirect);
		LogOnce("p2p direct send", aPeers);
	}
}

void CRelayClient::PumpRecv()
{
	for(int i = 0; i < 16; i++)
	{
		NETADDR Addr;
		unsigned char *pData;
		int Bytes = net_udp_recv(m_Socket, &Addr, &pData);
		if(Bytes <= 0)
			break;
		char aText[256];
		str_truncate(aText, sizeof(aText), (const char *)pData, Bytes);
		const bool FromRelay = net_addr_comp(&Addr, &m_RelayAddr) == 0;
		if(str_startswith(aText, "REG_OK") || str_startswith(aText, "BC_OK"))
		{
			if(!FromRelay)
				continue;
			m_LastServerReply = Client()->LocalTime();
			str_copy(m_aFailure, "已收到回执");
			continue;
		}
		if(str_startswith(aText, "PEER "))
		{
			if(!FromRelay)
				continue;
			// PEER <gameaddr> <uid> <ip>:<port>；gameaddr 里不含空格
			const char *pRest = aText + 5;
			const char *pSpace1 = strchr(pRest, ' ');
			if(!pSpace1 || pSpace1 - pRest != str_length(m_aGameAddr) || str_comp_num(pRest, m_aGameAddr, (int)(pSpace1 - pRest)) != 0)
				continue;
			const char *pUid = pSpace1 + 1;
			const char *pSpace2 = strchr(pUid, ' ');
			if(!pSpace2)
				continue;
			char aUid[48];
			str_truncate(aUid, sizeof(aUid), pUid, (int)(pSpace2 - pUid));
			HandlePeer(aUid, pSpace2 + 1);
			continue;
		}
		// 仅已知 UID 可学习 NAT 地址；收发共用每 peer 一秒节流。
		if(str_startswith(aText, "P "))
		{
			if(!g_Config.m_TcRelayP2p)
				continue;
			for(SPeer &Peer : m_vPeers)
			{
				if(str_comp(Peer.m_aUid, aText + 2) != 0)
					continue;
				const float Now = Client()->LocalTime();
				Peer.m_Addr = Addr;
				Peer.m_LastDirect = Now;
				Peer.m_LastSeen = Now;
				if(Now - Peer.m_LastProbe >= 1.0f)
				{
					Peer.m_LastProbe = Now;
					char aReply[64];
					str_format(aReply, sizeof(aReply), "P %s", m_aUid);
					net_udp_send(m_Socket, &Addr, aReply, str_length(aReply) + 1);
				}
				break;
			}
			continue;
		}
		// 直连载荷 D <payload>
		const char *pPayload = aText;
		bool Direct = false;
		if(str_startswith(aText, "D "))
		{
			if(!g_Config.m_TcRelayP2p)
				continue;
			pPayload = aText + 2;
			for(SPeer &Peer : m_vPeers)
			{
				if(net_addr_comp(&Peer.m_Addr, &Addr) == 0)
				{
					Peer.m_LastDirect = Client()->LocalTime();
					Direct = true;
					break;
				}
			}
		}
		if(str_startswith(aText, "D ") ? !Direct : !FromRelay)
			continue;
		if(!str_startswith(pPayload, "\xE2\x9F\xA8"))
			continue; // 非 ping 载荷（可能是扫描/误投），丢弃
		DeliverPayload(pPayload, Direct ? "p2p" : "UDP");
	}
}

void CRelayClient::PumpSend()
{
	if(g_Config.m_TcRelayCfOnly)
		return;
	const float Now = Client()->LocalTime();
	// 心跳：在线时每 10s 重注册；无回执时 2s 快速重试。
	const float RegInterval = UdpOnline() ? 10.0f : 2.0f;
	if(Now - m_LastRegTime > RegInterval && !SendReg())
		return;
	// 打洞探针：P2P 关闭时不发（保存 NAT 映射无意义）
	if(g_Config.m_TcRelayP2p)
	{
		for(SPeer &Peer : m_vPeers)
		{
			if(Now - Peer.m_LastProbe < 1.0f)
				continue;
			Peer.m_LastProbe = Now;
			char aBuf[64];
			str_format(aBuf, sizeof(aBuf), "P %s", m_aUid);
			net_udp_send(m_Socket, &Peer.m_Addr, aBuf, str_length(aBuf) + 1);
		}
	}
	// 30s 没被信令再推送的成员视为已离线
	for(size_t i = 0; i < m_vPeers.size();)
	{
		if(Now - m_vPeers[i].m_LastSeen > 30.0f)
			m_vPeers.erase(m_vPeers.begin() + i);
		else
			i++;
	}
	if(m_vSendQueue.empty())
		return;
	// 中转限速：12 条/秒（远低于信令 20/s 限速，ping 频率也远达不到）
	if(Now - m_LastSendTime < 0.08f)
		return;
	m_LastSendTime = Now;
	SQueued Item = m_vSendQueue.front();
	m_vSendQueue.erase(m_vSendQueue.begin());
	if(!Item.m_UseRelay || Now - Item.m_QueuedAt > 2.0f)
		return;
	if(PreferredTransport() == RelayRouting::ETransport::CLOUDFLARE)
	{
		m_pCfTransport->Send(Item.m_Payload.c_str());
		return;
	}
	if(!UdpOnline())
		return;
	char aGameAddr[64];
	if(!GameServerAddr(aGameAddr, sizeof(aGameAddr)))
		return;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "BC %s %s %s", aGameAddr, m_aUid, Item.m_Payload.c_str());
	net_udp_send(m_Socket, &m_RelayAddr, aBuf, str_length(aBuf) + 1);
	LogOnce("relay send", nullptr);
}

void CRelayClient::OnRender()
{
	if(!g_Config.m_TcRelay)
	{
		if(m_SocketValid || !m_CfRoom.empty())
			OnShutdown();
		return;
	}
	const float Now = Client()->LocalTime();
	if(m_LastRenderTime > -1e8f && Now - m_LastRenderTime > 15.0f)
	{
		LogOnce("render resumed, retry registration", nullptr);
		m_LastRegTime = -1e9f;
	}
	m_LastRenderTime = Now;
	char aGameAddr[64];
	if(!GameServerAddr(aGameAddr, sizeof(aGameAddr)))
	{
		if(m_aGameAddr[0])
			OnReset();
		str_copy(m_aFailure, "未连接游戏服");
		return;
	}
	if(!m_aUid[0])
		OnInit();
	if(str_comp(m_aGameAddr, aGameAddr) != 0)
	{
		OnReset();
		str_copy(m_aGameAddr, aGameAddr);
		LogOnce("registration resumed", aGameAddr);
	}
	const bool CfOnly = g_Config.m_TcRelayCfOnly != 0;
	if(CfOnly && m_SocketValid)
	{
		net_udp_close(m_Socket);
		m_Socket = nullptr;
		m_SocketValid = false;
		m_RelayAddrValid = false;
		m_LastServerReply = -1e9f;
		m_vPeers.clear();
		m_vSendQueue.clear();
	}
	if(!CfOnly)
	{
		if(!m_SocketValid && Now - m_LastSocketAttempt >= 5.0f)
		{
			m_LastSocketAttempt = Now;
			OnInit();
		}
		if(m_SocketValid)
			PumpRecv();
	}
	PumpCloudflare();
	if(m_SocketValid && !CfOnly)
		PumpSend();
	else if(CfOnly)
		m_vSendQueue.clear();
	const auto Transport = PreferredTransport();
	if(Transport != m_LastTransport)
	{
		LogOnce("preferred transport", Transport == RelayRouting::ETransport::UDP        ? "UDP" :
					       Transport == RelayRouting::ETransport::CLOUDFLARE ? "CF" :
												   "local only");
		m_LastTransport = Transport;
	}
}
