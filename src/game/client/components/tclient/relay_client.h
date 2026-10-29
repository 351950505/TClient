/* TClient: 信令/中继客户端。玩家连上任意游戏服后向 TClient 信令服务注册
   （游戏服地址+名字），同游戏服的 TClient 玩家经信令互发 ping 数据：
   - 不占用游戏服的聊天/表情，对原版玩家和聊天框完全无痕
   - 旁观/死亡也能发（无活体要求），不受服务器聊天/表情冷却限制
   协议（UDP 文本行）：
   REG <gameaddr> <name>          注册/心跳（信令回 REG_OK）
   BC  <gameaddr> <name> <payload> 广播给同游戏服其他注册者（信令回 BC_OK N）
   payload 即 pingwheel 的紧凑编码（⟨B:...），由 CPingWheel::OnRelayPayload 解析。
   信令不可用时自动回退现有通道（表情/本地显示）。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_RELAY_CLIENT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_RELAY_CLIENT_H

#include <string>
#include <vector>

#include <engine/client.h>

#include <game/client/component.h>

class CRelayClient : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void OnInit() override;
	void OnReset() override;
	void OnRender() override;

	// 信令最近 30s 内有回应时视为在线
	bool Online() const;
	// 把 ping 载荷广播给同游戏服的其他 TClient 玩家
	void Broadcast(const char *pPayload);

private:
	NETSOCKET m_Socket = {};
	char m_aUid[48] = ""; // 会话唯一键（名字-随机），同服同名客户端不互顶
	bool m_SocketValid = false;
	NETADDR m_RelayAddr = {};
	bool m_RelayAddrValid = false;
	float m_LastServerReply = -1e9f;
	float m_LastRegTime = -1e9f;
	float m_LastSendTime = -1e9f;
	std::vector<std::string> m_vSendQueue; // 待广播载荷

	void GameServerAddr(char *pBuf, int Size) const;
	void SendReg();
	void PumpRecv();
	void PumpSend();
};

#endif
