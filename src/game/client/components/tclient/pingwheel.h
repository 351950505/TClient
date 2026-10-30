/* TClient: LoL 式地图信号轮盘。按住键出轮盘，选信号在准星世界坐标插地图标记。
   发送通道（Ping() 内按可用性路由，信令优先）：
   - 信令广播（首选，tc_relay）：经 TClient 信令服务转发给同服 TClient 玩家，
     旁观/死亡也能发，游戏服聊天与表情零占用，原版玩家全程无感知。
   - 表情通道（fallback，tc_pingwheel_emote）：帧头+9 个表情承载 signal+坐标，
     间隔 tc_pingwheel_emote_ms（默认 60ms，需 >= 服务器 sv_emoticon_ms_delay，
     否则第 2 个起被个人冷却丢弃），全服广播；帧发完后对比自己收到的回显数，
     不足则提示被服务器冷却丢弃。对所有人可见（表情气泡+音效），只有 TClient 知道语义。
   - 聊天发送已移除；聊天接收兼容旧格式（⟨P:id:x:y:名⟩ 与 ⟨B:...），
     收到后同样产出地图标记与聊天图标行。
   0.6 协议下客户端不能直连、服务器只广播白名单消息，服务端零改动。
   图标：可放 data/tclient/pings/signal0.png ~ signal7.png（0=危险…7=等等）自定义，
   未提供则用内置绘制样式。注意：Riot 版权素材请勿打进公开分发包。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PINGWHEEL_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PINGWHEEL_H

#include <array>

#include <base/vmath.h>

#include <cstdint>
#include <vector>

#include <engine/console.h>
#include <engine/graphics.h>

#include <game/client/component.h>
#include <game/client/ui.h>

class CPingWheel : public CComponent
{
public:
	enum
	{
		SIGNAL_DANGER = 0, // 危险
		SIGNAL_RETREAT,    // 撤退
		SIGNAL_PUSH,       // 推进
		SIGNAL_ONMYWAY,    // 赶来
		SIGNAL_GROUP,      // 集合
		SIGNAL_HELP,       // 支援
		SIGNAL_ENEMYMISS,  // 敌消
		SIGNAL_WAIT,       // 等等
		NUM_SIGNALS,
	};

	// 聊天编码前缀。旧明文格式 ⟨P:id:x:y:名⟩ 仅接收兼容；发送用新紧凑格式 ⟨B:...⟩。
	// chat.cpp 收到同步行时调 SignalFromMessage 识别并静默，本组件负责解码。
	static constexpr const char *CHAT_PREFIX = "\xE2\x9F\xA8P:";
	// 注意：'B' 是十六进制字符，不能直接跟在 \xA8 后（会被贪婪解析），用字面量拼接
	static constexpr const char *CHAT_PREFIX_BIN = "\xE2\x9F\xA8" "B:";
	static constexpr float MARKER_LIFE = 6.0f; // 标记存活秒数

	struct CMarker
	{
		int m_Type = -1;
		vec2 m_WorldPos = vec2(0.0f, 0.0f);
		float m_StartTime = -1.0f; // Client()->LocalTime()，<0 表示空槽
	};

private:
	float m_AnimationTime = 0.0f;
	bool m_WasActive = false;
	bool m_Active = false;
	vec2 m_SelectorMouse;
	int m_SelectedSignal = -1;
	int m_aSoundIds[NUM_SIGNALS] = {-1, -1, -1, -1, -1, -1, -1, -1};   // 每个信号的音效（data/tclient/pings/<名>.wav）
	bool m_SoundsTried = false;
	vec2 m_PingWorldPos = vec2(0.0f, 0.0f);
	IGraphics::CTextureHandle m_aSignalTextures[NUM_SIGNALS];
	bool m_IconsTried = false;
	std::array<CMarker, 16> m_aMarkers;

	static void ConKeyPingWheel(IConsole::IResult *pResult, void *pUserData);
	static void ConTestPing(IConsole::IResult *pResult, void *pUserData);

	// 紧凑二进制编码：signal(3bit) + x(16bit) + y(16bit) → 6 个编码字符（36bit）。
	// 坐标为整数像素、有符号 ±32767（超出钳制）。
	static void EncodeBinary(int Signal, int X, int Y, char (&aBuf)[24]);
	static bool DecodeBinary(const char *pMessage, int *pSignal, int *pX, int *pY);

	// 表情通道：帧头 + 9 个 4bit 表情承载 signal(3)|x(16)|y(16)（左移 1 位补 36bit）。
	// 每个表情间隔 tc_pingwheel_emote_ms 毫秒（必须 >= 服务器 sv_emoticon_ms_delay，
	// 否则第 2 个起被个人冷却直接丢弃）。帧发完后对比自己的回显数检测丢失。
	std::vector<int> m_vEmoteQueue; // 待发表情序列
	float m_LastEmoteSendTime = -1e9f;
	int m_EmoteRecvCount = -1;          // -1 = 等帧头
	uint64_t m_EmoteRecvPayload = 0;
	float m_EmoteRecvStart = -1e9f;
	int m_EmoteFrameSent = 0;           // 当前帧已发送数（10）
	int m_EmoteEchoSeen = 0;            // 帧发送前的自回显计数基准
	int m_EmoteEchoTotal = 0;           // 自回显累计计数
	float m_EmoteFrameDoneTime = -1e9f; // 帧发完时刻（丢失检测用）
	void EmotePing(int Signal, int X, int Y);
	void PumpEmoteQueue();

	void TryLoadIcons();
	void TryLoadSounds();
	void PlaySignalSound(int Signal, vec2 WorldPos);
	void AddMarker(int Signal, vec2 WorldPos);
	void RenderMarkers();
	vec2 GetPingWorldPos() const;
	vec2 WorldToUi(vec2 WorldPos) const;
	void RenderSpecCursor();
	vec2 m_LastSpecMouse = vec2(0.0f, 0.0f);
	float m_LastSpecMouseMove = 0.0f;

public:
	CPingWheel();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnConsoleInit() override;
	void OnRender() override;
	void OnRelease() override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	bool OnInput(const IInput::CEvent &Event) override;
	void OnMessage(int MsgType, void *pRawMsg) override;

	void Ping(int Signal, vec2 WorldPos);
	// 信令广播载荷入口（CRelayClient 收到同服 TClient 的 ping 时调用）
	void OnRelayPayload(const char *pText);
	// 从消息解析信号类型（⟨B: 新格式 / ⟨P: 旧格式），失败返回 -1；
	// chat.cpp 用它把 ping 行改写成图标行（QmClient 式聊天可视化）
	static int SignalFromMessage(const char *pMessage);
	// TC 设置 Ping 页用的顺序管理
	int LayoutType(int Sector) const;                 // 扇区 → 信号类型
	void LayoutMove(int Sector, int Dir);             // 交换相邻扇区并写回 cvar
	bool SignalTextureValid(int Signal) const { return m_aSignalTextures[Signal].IsValid(); }
	const IGraphics::CTextureHandle *SignalTexture(int Signal) const { return &m_aSignalTextures[Signal]; }
	static const char *SignalName(int Signal);  // 轮盘用两字名
	static const char *SignalShort(int Signal); // 地图标记用单字

	bool IsActive() const { return m_Active; }
};

#endif
