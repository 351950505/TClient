/* TClient: LoL 式地图信号轮盘。按住键出轮盘，选信号在准星世界坐标插地图标记；
   信号通过聊天通道（⟨P:id:x:y⟩ 前缀）同步，装了 TClient 的同服玩家解析成标记，
   其他玩家看到一条短文本。服务端零改动。
   图标：可放 data/tclient/pings/signal0.png ~ signal7.png（0=危险…7=等等）自定义，
   未提供则用内置绘制样式。注意：Riot 版权素材请勿打进公开分发包。 */
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PINGWHEEL_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PINGWHEEL_H

#include <array>

#include <base/vmath.h>

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

	// 聊天编码前缀（⟨P:），chat.cpp 靠它拦行、本组件靠它解析
	static constexpr const char *CHAT_PREFIX = "\xE2\x9F\xA8P:";
	static constexpr float MARKER_LIFE = 6.0f;       // 标记存活秒数
	static constexpr int RATE_COUNT = 5;             // 限频：每个窗口最多发送次数（防服务器禁言）
	static constexpr float RATE_WINDOW = 40.0f;      // 限频窗口秒数，从窗口内第一次发送开始计

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
	float m_RateWindowStart = -1e9f;
	int m_RateSentCount = 0;
	IGraphics::CTextureHandle m_aSignalTextures[NUM_SIGNALS];
	bool m_IconsTried = false;
	std::array<CMarker, 16> m_aMarkers;

	static void ConKeyPingWheel(IConsole::IResult *pResult, void *pUserData);

	bool RateLimitedAndRecord();
	void TryLoadIcons();
	void TryLoadSounds();
	void PlaySignalSound(int Signal);
	void AddMarker(int Signal, vec2 WorldPos);
	void RenderMarkers();
	vec2 GetPingWorldPos() const;

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
