/* TClient 信令/中继客户端，见 relay_client.h */
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
	char aGame[64];
	pSelf->GameServerAddr(aGame, sizeof(aGame));
	const float Age = pSelf->Client()->LocalTime() - pSelf->m_LastServerReply;
	char aBuf[192];
	str_format(aBuf, sizeof(aBuf),
		"信令: %s | 服务器 %s | 上次回执 %.1fs 前%s | 待发 %d",
		pSelf->Online() ? "已连接" : "未连接",
		pSelf->m_RelayAddrValid ? g_Config.m_TcRelayAddr : "(地址无效)",
		Age < 1e8f ? Age : -1.0f,
		aGame,
		(int)pSelf->m_vSendQueue.size());
	pSelf->GameClient()->m_Chat.Echo(aBuf);
}

void CRelayClient::OnConsoleInit()
{
	Console()->Register("tc_relay_status", "", CFGFLAG_CLIENT, ConRelayStatus, this, "Show signal relay connection status");
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
	m_LastServerReply = -1e9f;
}

bool CRelayClient::Online() const
{
	return m_SocketValid && Client()->LocalTime() - m_LastServerReply < 30.0f;
}

void CRelayClient::GameServerAddr(char *pBuf, int Size) const
{
	// 未连接时用占位，保证信令分组键稳定（重连后 ServerAddress 变化会自然重新分组）
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		str_copy(pBuf, "offline", Size);
		return;
	}
	net_addr_str(&Client()->ServerAddress(), pBuf, Size, true);
}

void CRelayClient::SendReg()
{
	char aGameAddr[64];
	GameServerAddr(aGameAddr, sizeof(aGameAddr));
	char aBuf[160];
	str_format(aBuf, sizeof(aBuf), "REG %s %s %s", aGameAddr, m_aUid, g_Config.m_PlayerName);
	net_udp_send(m_Socket, &m_RelayAddr, aBuf, str_length(aBuf) + 1);
	m_LastRegTime = Client()->LocalTime();
}

void CRelayClient::Broadcast(const char *pPayload)
{
	// 只入队裸载荷，BC 行组装统一在 PumpSend（避免双重嵌套）
	if(!m_SocketValid || str_length(pPayload) > 160)
		return;
	if(m_vSendQueue.size() < 8) // 限深：ping 频率远低于此
	{
		m_vSendQueue.push_back(pPayload);
		log_info("relaydiag", "broadcast queued: %s", pPayload);
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
		// 纯 transport：载荷原样转投，"这是什么/要不要收"由订阅方决定
		log_info("relaydiag", "recv payload: %s", aText);
		GameClient()->m_PingWheel.OnRelayPayload(aText);
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
	if(m_vSendQueue.empty())
		return;
	// 广播限速：12 条/秒（远低于信令 20/s 限速，ping 频率也远达不到）
	if(Now - m_LastSendTime < 0.08f)
		return;
	m_LastSendTime = Now;
	std::string Payload = m_vSendQueue.front();
	m_vSendQueue.erase(m_vSendQueue.begin());
	char aGameAddr[64];
	GameServerAddr(aGameAddr, sizeof(aGameAddr));
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "BC %s %s %s", aGameAddr, m_aUid, Payload.c_str());
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
