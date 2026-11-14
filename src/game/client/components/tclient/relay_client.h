/* TClient: 信令/中继客户端。玩家连上任意游戏服后向 TClient 信令服务注册
   （游戏服地址+名字），同游戏服的 TClient 玩家经信令互发 ping 数据：
   - 不占用游戏服的聊天/表情，对原版玩家和聊天框完全无痕
   - 旁观/死亡也能发（无活体要求），不受服务器聊天/表情冷却限制
   协议（UDP 文本行）：
   REG <gameaddr> <name>          注册/心跳（信令回 REG_OK）
   BC  <gameaddr> <name> <payload> 广播给同游戏服其他注册者（信令回 BC_OK N）
   payload 为不透明的字节串（当前是 pingwheel 的紧凑编码），收到即原样转投
   订阅方（CPingWheel::OnRelayPayload）——transport 不知道也不关心载荷语义。
   信令不可用时自动回退现有通道（表情/本地显示）。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_RELAY_CLIENT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_RELAY_CLIENT_H

#include <engine/client.h>
#include <engine/console.h>

#include <game/client/component.h>

#include <memory>
#include <string>
#include <vector>

namespace RelayWs
{
	class CTransport;
}

namespace RelayRouting
{
	enum class ETransport
	{
		NONE,
		UDP,
		CLOUDFLARE,
	};

	inline bool IsPrototypeUrl(const std::string &Url)
	{
		return Url == "wss://wsproto20261114.me3.dpdns.org/ws" ||
		       Url == "wss://wsproto20261114.me3.dpdns.org";
	}

	inline ETransport PreferredTransport(bool UdpOnline, bool CfEnabled, bool CfOnline, bool CfOnly = false)
	{
		if(CfOnly)
			return CfEnabled && CfOnline ? ETransport::CLOUDFLARE : ETransport::NONE;
		if(UdpOnline)
			return ETransport::UDP;
		if(CfEnabled && CfOnline)
			return ETransport::CLOUDFLARE;
		return ETransport::NONE;
	}

	// 只向真实在线的游戏服注册；demo 回放的 ServerAddress 为空，不能格式化为地址。
	inline bool CanRegister(IClient::EClientState ClientState, unsigned int ServerAddressType)
	{
		return ClientState == IClient::STATE_ONLINE && (ServerAddressType & NETTYPE_ALL) != 0;
	}

	// Keep direct delivery and relay suppression in one decision. Fresh peers only
	// suppress the relay when direct delivery is actually enabled.
	template<typename TPeers, typename TDirect, typename TSendDirect, typename TSendRelay>
	int Broadcast(bool P2pEnabled, bool RelayOnline, const TPeers &Peers, TDirect Direct, TSendDirect SendDirect, TSendRelay SendRelay)
	{
		bool AllDirect = P2pEnabled && !Peers.empty();
		int SentDirect = 0;
		if(P2pEnabled)
		{
			for(const auto &Peer : Peers)
			{
				if(Direct(Peer))
				{
					SendDirect(Peer);
					++SentDirect;
				}
				else
					AllDirect = false;
			}
		}
		if(RelayOnline && !AllDirect)
			SendRelay();
		return SentDirect;
	}
} // namespace RelayRouting

class CRelayClient : public CComponent
{
public:
	CRelayClient();
	~CRelayClient() override;
	int Sizeof() const override { return sizeof(*this); }

	void OnInit() override;
	void OnShutdown() override;
	void OnReset() override;
	void OnRender() override;
	void OnConsoleInit() override;

	// 信令最近 30s 内有回应时视为在线
	bool Online() const;
	// 有可用通道：信令在线，或已知同服成员（可尝试 P2P 直连）
	bool Available() const;
	// 把 ping 载荷发出去：P2P 直连优先，未打通/无成员时走服务器中转
	void Broadcast(const char *pPayload);
	// 结构化状态（tc_relay_status 用）
	void FormatStatus(char *pBuf, int Size) const;

private:
	// 同服成员的直连地址（由信令 PEER 推送，是对方在信令侧的 NAT 映射地址）
	struct SPeer
	{
		char m_aUid[48] = "";
		NETADDR m_Addr = {};
		float m_LastSeen = -1e9f; // 最近一次收到 PEER 推送或探针
		float m_LastDirect = -1e9f; // 最近一次收到该地址的直连包
		float m_LastProbe = -1e9f; // 最近一次向该地址打洞（主动和回复共用）
	};
	struct SQueued
	{
		std::string m_Payload;
		bool m_UseRelay = false;
		float m_QueuedAt = 0.0f;
	};

	NETSOCKET m_Socket = {};
	char m_aUid[48] = ""; // 会话唯一键（名字-随机），同服同名客户端不互顶
	bool m_SocketValid = false;
	NETADDR m_RelayAddr = {};
	bool m_RelayAddrValid = false;
	float m_LastServerReply = -1e9f;
	float m_LastRegTime = -1e9f;
	float m_LastSendTime = -1e9f;
	float m_LastSocketAttempt = -1e9f;
	float m_LastRenderTime = -1e9f;
	char m_aGameAddr[64] = "";
	char m_aFailure[64] = "等待注册";
	// 诊断日志去重表：按"原因"分别记住上次的细节，只有该原因的细节变化时才写
	// F1。心跳成功路径不再每 10s 刷屏，但首次注册/切服/掉线恢复仍各留一条。
	struct SLogMemo
	{
		char m_aReason[32] = "";
		char m_aDetail[96] = "";
	};
	SLogMemo m_aLogMemo[8] = {};
	int m_NumLogMemo = 0;
	std::vector<SQueued> m_vSendQueue; // 待发载荷
	std::vector<SPeer> m_vPeers; // 同服成员（P2P 候选）
	char m_aLastRecvPayload[161] = ""; // 收到的上一条载荷（P2P+中转去重）
	float m_LastRecvPayloadTime = -1e9f;
	std::unique_ptr<RelayWs::CTransport> m_pCfTransport;
	std::string m_CfRoom;
	std::string m_CfUrl;
	std::string m_CfError;
	bool m_CfOnline = false;
	RelayRouting::ETransport m_LastTransport = RelayRouting::ETransport::NONE;

	bool UdpOnline() const;
	bool CfEnabled() const;
	RelayRouting::ETransport PreferredTransport() const;
	void PumpCloudflare();
	void DeliverPayload(const char *pPayload, const char *pSource);
	bool GameServerAddr(char *pBuf, int Size) const;
	static void ConRelayStatus(IConsole::IResult *pResult, void *pUserData);
	void HandlePeer(const char *pUid, const char *pAddrStr);
	bool PeerDirect(const SPeer &Peer) const;
	bool SendReg();
	// 只在诊断原因变化时输出日志；同一原因重复发生不再写 F1（避免心跳刷屏）。
	void LogOnce(const char *pReason, const char *pDetail);
	void PumpRecv();
	void PumpSend();
};

#endif
