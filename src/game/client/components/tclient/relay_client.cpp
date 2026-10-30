/* TClient 信令/中继客户端，见 relay_client.h
   P2P 直连：信令把我们这一组的成员公网地址（PEER）推下来，双方互向对方
   公网地址发探针把 NAT 映射打通；打通后 ping 载荷直接点对点发送，不经信令。
   任一方 NAT 是对称型导致打不通时，自动回退信令中转。 */
#include "relay_client.h"

#include <engine/shared/config.h>

#include <game/client/gameclient.h>

#include <base/log.h>
#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

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
	const float Age = Client()->LocalTime() - m_LastServerReply;
	int Direct = 0;
	for(const SPeer &Peer : m_vPeers)
		if(PeerDirect(Peer))
			Direct++;
	str_format(pBuf, Size,
		"信令: %s | 服 %s | 回执 %.1fs 前 | 中转 %s | P2P %d/%d | 待发 %d",
		Online() ? "已连接" : "未连接",
		aGame,
		Age < 1e8f ? Age : -1.0f,
		m_RelayAddrValid ? g_Config.m_TcRelayAddr : "(地址无效)",
		Direct,
		(int)m_vPeers.size(),
		(int)m_vSendQueue.size());
}

void CRelayClient::OnConsoleInit()
{
	Console()->Register("tc_relay_status", "", CFGFLAG_CLIENT, ConRelayStatus, this, "Show signal relay / p2p status");
}

void CRelayClient::OnInit()
{
	if(!g_Config.m_TcRelay)
		return;
	if(net_addr_from_str(&m_RelayAddr, g_Config.m_TcRelayAddr) != 0)
	{
		log_error("relay", "invalid tc_relay_addr '%s', relay disabled", g_Config.m_TcRelayAddr);
		return;
	}
	m_RelayAddrValid = true;
	NETADDR Bind = {0};
	Bind.type = NETTYPE_ALL;
	m_Socket = net_udp_create(Bind);
	m_SocketValid = m_Socket != nullptr; // net_udp_create 失败时返回 nullptr
	if(!m_SocketValid)
		log_error("relay", "failed to create udp socket, relay disabled");
	// 会话 uid：纯随机十六进制（不含名字——名字可能带空格，会被信令的
	// 空格分词截断导致 uid 错乱）。名字只作为日志字段放最后。
	const unsigned Seed1 = (unsigned)time_get();
	const unsigned Seed2 = (unsigned)(uintptr_t)this;
	str_format(m_aUid, sizeof(m_aUid), "%08x%08x", Seed1, Seed2);
}

void CRelayClient::OnReset()
{
	m_vSendQueue.clear();
	m_vPeers.clear();
	m_LastServerReply = -1e9f;
	m_aLastRecvPayload[0] = 0;
	m_LastRecvPayloadTime = -1e9f;
}

bool CRelayClient::Online() const
{
	return m_SocketValid && Client()->LocalTime() - m_LastServerReply < 30.0f;
}

bool CRelayClient::Available() const
{
	if(!m_SocketValid)
		return false;
	if(Online())
		return true;
	for(const SPeer &Peer : m_vPeers)
		if(PeerDirect(Peer))
			return true;
	return false;
}

bool CRelayClient::PeerDirect(const SPeer &Peer) const
{
	// 10s 内收到过对方的直连包即视为已打通（探针每秒一次，留足抖动余量）
	return Client()->LocalTime() - Peer.m_LastDirect < 10.0f;
}

bool CRelayClient::AllPeersDirect() const
{
	if(m_vPeers.empty())
		return false;
	for(const SPeer &Peer : m_vPeers)
		if(!PeerDirect(Peer))
			return false;
	return true;
}

bool CRelayClient::GameServerAddr(char *pBuf, int Size) const
{
	// 未连服务器时不注册：否则所有"未进服"的客户端会被分到同一个 offline
	// 分组，互相推送无意义的 PEER，还会让外人在列表里看到你。
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return false;
	net_addr_str(&Client()->ServerAddress(), pBuf, Size, true);
	return true;
}

void CRelayClient::SendReg()
{
	char aGameAddr[64];
	if(!GameServerAddr(aGameAddr, sizeof(aGameAddr)))
		return;
	char aBuf[160];
	str_format(aBuf, sizeof(aBuf), "REG %s %s %s", aGameAddr, m_aUid, g_Config.m_PlayerName);
	net_udp_send(m_Socket, &m_RelayAddr, aBuf, str_length(aBuf) + 1);
	m_LastRegTime = Client()->LocalTime();
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
	log_info("relaydiag", "peer added: %s @ %s", pUid, pAddrStr);
}

void CRelayClient::Broadcast(const char *pPayload)
{
	if(!m_SocketValid || str_length(pPayload) > 160)
		return;
	// P2P 优先：已打通的成员直接点对点发（省延迟、不经信令）
	int SentDirect = 0;
	for(const SPeer &Peer : m_vPeers)
	{
		if(!PeerDirect(Peer))
			continue;
		char aBuf[192];
		str_format(aBuf, sizeof(aBuf), "D %s", pPayload);
		net_udp_send(m_Socket, &Peer.m_Addr, aBuf, str_length(aBuf) + 1);
		SentDirect++;
	}
	if(SentDirect > 0)
		log_info("relaydiag", "p2p send: %s -> %d peers", pPayload, SentDirect);
	// 还有成员没打通（或还不知道有谁）时走信令中转兜底
	if(AllPeersDirect() || !Online())
		return;
	if(m_vSendQueue.size() < 8) // 限深：ping 频率远低于此
	{
		SQueued Item;
		Item.m_Payload = pPayload;
		Item.m_UseRelay = true;
		m_vSendQueue.push_back(Item);
		log_info("relaydiag", "broadcast queued (relay): %s", pPayload);
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
		if(str_startswith(aText, "REG_OK") || str_startswith(aText, "BC_OK"))
		{
			m_LastServerReply = Client()->LocalTime();
			continue;
		}
		if(str_startswith(aText, "PEER "))
		{
			// PEER <gameaddr> <uid> <ip>:<port>；gameaddr 里不含空格
			const char *pRest = aText + 5;
			const char *pSpace1 = strchr(pRest, ' ');
			if(!pSpace1)
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
		// 探针 P <uid>：收到即回探针（双向打洞），并把来源地址记为对方
		// 真实可回地址——NAT 学习，比信令推送的映射更准。
		if(str_startswith(aText, "P "))
		{
			const char *pUid = aText + 2;
			for(SPeer &Peer : m_vPeers)
			{
				if(str_comp(Peer.m_aUid, pUid) == 0)
				{
					Peer.m_Addr = Addr;
					Peer.m_LastDirect = Client()->LocalTime();
					Peer.m_LastSeen = Client()->LocalTime();
					break;
				}
			}
			char aReply[64];
			str_format(aReply, sizeof(aReply), "P %s", m_aUid);
			net_udp_send(m_Socket, &Addr, aReply, str_length(aReply) + 1);
			continue;
		}
		// 直连载荷 D <payload>
		const char *pPayload = aText;
		bool Direct = false;
		if(str_startswith(aText, "D "))
		{
			pPayload = aText + 2;
			Direct = true;
			for(SPeer &Peer : m_vPeers)
			{
				if(net_addr_comp(&Peer.m_Addr, &Addr) == 0)
				{
					Peer.m_LastDirect = Client()->LocalTime();
					break;
				}
			}
		}
		if(!str_startswith(pPayload, "\xE2\x9F\xA8"))
			continue; // 非 ping 载荷（可能是扫描/误投），丢弃
		// 去重：同一载荷可能同时经 P2P 与中转到达
		const float Now = Client()->LocalTime();
		if(str_comp(m_aLastRecvPayload, pPayload) == 0 && Now - m_LastRecvPayloadTime < 1.0f)
			continue;
		str_copy(m_aLastRecvPayload, pPayload);
		m_LastRecvPayloadTime = Now;
		// 纯 transport：载荷原样转投，"这是什么/要不要收"由订阅方决定
		log_info("relaydiag", "recv payload (%s): %s", Direct ? "p2p" : "relay", pPayload);
		GameClient()->m_PingWheel.OnRelayPayload(pPayload);
	}
}

void CRelayClient::PumpSend()
{
	const float Now = Client()->LocalTime();
	// 心跳：在线时每 10s 重注册（信令按 120s 过期）；离线（REG_OK 丢失/网络
	// 抖动）时 2s 快速重试，单次回执丢失不再造成长达 10s 的"未连接"窗口
	const float RegInterval = Online() ? 10.0f : 2.0f;
	if(Now - m_LastRegTime > RegInterval)
		SendReg();
	// 打洞探针：未打通的成员每秒探一次（同时维持本端 NAT 映射不被回收）
	for(SPeer &Peer : m_vPeers)
	{
		if(PeerDirect(Peer) || Now - Peer.m_LastProbe < 1.0f)
			continue;
		Peer.m_LastProbe = Now;
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "P %s", m_aUid);
		net_udp_send(m_Socket, &Peer.m_Addr, aBuf, str_length(aBuf) + 1);
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
	if(!Item.m_UseRelay)
		return;
	char aGameAddr[64];
	if(!GameServerAddr(aGameAddr, sizeof(aGameAddr)))
		return;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "BC %s %s %s", aGameAddr, m_aUid, Item.m_Payload.c_str());
	net_udp_send(m_Socket, &m_RelayAddr, aBuf, str_length(aBuf) + 1);
	log_info("relaydiag", "relay send: %s", aBuf);
}

void CRelayClient::OnRender()
{
	if(!m_SocketValid)
		return;
	PumpRecv();
	PumpSend();
}
