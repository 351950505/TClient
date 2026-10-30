/* TClient: LoL 式地图信号轮盘（详见 pingwheel.h） */
#include "pingwheel.h"

#include <base/io.h>
#include <base/log.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/components/chat.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>

static const char *const s_apSignalNames[] = {"危险", "撤退", "推进", "赶来", "集合", "支援", "敌消", "等等"};
static const char *const s_apSignalShorts[] = {"危", "退", "推", "来", "集", "援", "消", "等"};
static const vec3 s_aSignalColors[] = {
	vec3(0.733f, 0.672f, 0.151f), // 危险（图标底色，固定）
	vec3(0.714f, 0.267f, 0.557f), // 撤退
	vec3(0.333f, 0.718f, 0.220f), // 推进
	vec3(0.152f, 0.377f, 0.753f), // 赶来
	vec3(0.733f, 0.674f, 0.140f), // 集合
	vec3(0.325f, 0.696f, 0.223f), // 支援
	vec3(0.735f, 0.683f, 0.176f), // 敌消
	vec3(0.353f, 0.729f, 0.241f), // 等等
};

static_assert(std::size(s_apSignalNames) == CPingWheel::NUM_SIGNALS);
static_assert(std::size(s_apSignalShorts) == CPingWheel::NUM_SIGNALS);
static_assert(std::size(s_aSignalColors) == CPingWheel::NUM_SIGNALS);

// 紧凘编码字母表：64 个不含 ':' 的安全可见字符。
static const char s_aPingAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789#%";
static_assert(std::size(s_aPingAlphabet) == 65, "alphabet must be 64 chars + NUL");

void CPingWheel::EncodeBinary(int Signal, int X, int Y, char (&aBuf)[24])
{
	const uint32_t ux = (uint32_t)std::clamp(X, -32768, 32767) + 32768;
	const uint32_t uy = (uint32_t)std::clamp(Y, -32768, 32767) + 32768;
	uint64_t Payload = ((uint64_t)(Signal & 7) << 32) | ((uint64_t)ux << 16) | (uint64_t)uy;
	char *pOut = aBuf;
	*pOut++ = 0xE2; *pOut++ = 0x9F; *pOut++ = 0xA8; // ⟨
	*pOut++ = 'B';
	*pOut++ = ':';
	for(int i = 5; i >= 0; i--)
		*pOut++ = s_aPingAlphabet[(Payload >> (i * 6)) & 0x3F];
	*pOut = 0;
}

bool CPingWheel::DecodeBinary(const char *pMessage, int *pSignal, int *pX, int *pY)
{
	const char *pCur = str_startswith(pMessage, CHAT_PREFIX_BIN);
	if(!pCur || str_length(pCur) < 6)
		return false;
	uint64_t Payload = 0;
	for(int i = 0; i < 6; i++)
	{
		const char *pSym = (const char *)memchr(s_aPingAlphabet, pCur[i], 64);
		if(!pSym)
			return false;
		Payload = (Payload << 6) | (uint64_t)(pSym - s_aPingAlphabet);
	}
	if(pCur[6] != 0) // 后面若还有字符则非合法载荷（防误判普通聊天）
		return false;
	const int Signal = (int)((Payload >> 32) & 7);
	const int X = (int)((Payload >> 16) & 0xFFFF) - 32768;
	const int Y = (int)(Payload & 0xFFFF) - 32768;
	if(Signal >= NUM_SIGNALS)
		return false;
	*pSignal = Signal;
	*pX = X;
	*pY = Y;
	return true;
}

// ---- 表情通道 ----

void CPingWheel::EmotePing(int Signal, int X, int Y)
{
	uint64_t Payload = ((uint64_t)(Signal & 7) << 32) |
		((uint64_t)(uint32_t)(std::clamp(X, -32768, 32767) + 32768) << 16) |
		(uint64_t)(uint32_t)(std::clamp(Y, -32768, 32767) + 32768);
	Payload <<= 1; // 35bit → 36bit（9 个 4bit 表情）
	m_vEmoteQueue.clear();
	m_vEmoteQueue.push_back(EMOTICON_OOP); // 帧头
	for(int i = 8; i >= 0; i--)
		m_vEmoteQueue.push_back((int)((Payload >> (4 * i)) & 0xF));
	m_EmoteRecvCount = -1; // 重置接收（自己的表情会被服务器广播回来，接收端按 cid 过滤）
	// 丢失检测基准：记下发帧前的自回显计数
	m_EmoteFrameSent = 10;
	m_EmoteEchoSeen = m_EmoteEchoTotal;
	m_EmoteFrameDoneTime = -1e9f;
}

void CPingWheel::PumpEmoteQueue()
{
	if(m_vEmoteQueue.empty())
	{
		// 帧发完 3 秒后对比自回显数：少于发送数 = 表情被服务器冷却丢弃
		if(m_EmoteFrameDoneTime > 0 && Client()->LocalTime() - m_EmoteFrameDoneTime > 3.0f)
		{
			m_EmoteFrameDoneTime = -1e9f;
			const int Echoed = m_EmoteEchoTotal - m_EmoteEchoSeen;
			if(Echoed < m_EmoteFrameSent)
			{
				char aBuf[192];
				str_format(aBuf, sizeof(aBuf), "表情信号只发出 %d/%d 个：服务器表情冷却过长。请在设置里调大表情间隔，或关闭表情通道改用队伍聊天", Echoed, m_EmoteFrameSent);
				GameClient()->m_Chat.Echo(aBuf);
			}
		}
		return;
	}
	if(!GameClient()->m_Snap.m_pLocalCharacter)
	{
		// 表情需要活体（服务器丢弃无角色的表情），死亡就放弃残帧
		m_vEmoteQueue.clear();
		return;
	}
	const float Now = Client()->LocalTime();
	if(Now - m_LastEmoteSendTime < g_Config.m_TcPingwheelEmoteMs / 1000.0f)
		return;
	m_LastEmoteSendTime = Now;
	CNetMsg_Cl_Emoticon Msg;
	Msg.m_Emoticon = m_vEmoteQueue.front();
	m_vEmoteQueue.erase(m_vEmoteQueue.begin());
	if(m_vEmoteQueue.empty())
		m_EmoteFrameDoneTime = Now; // 本帧发完，进入丢失检测窗口
	Client()->SendPackMsgActive(&Msg, MSGFLAG_VITAL);
}

static void PingWheelParseLayout(const char *p, int *pOut) // pOut 至少 NUM_SIGNALS；非法/缺项回退恒等顺序
{
	for(int i = 0; i < CPingWheel::NUM_SIGNALS; i++)
		pOut[i] = i;
	bool aUsed[CPingWheel::NUM_SIGNALS] = {};
	int Slot = 0;
	while(*p && Slot < CPingWheel::NUM_SIGNALS)
	{
		if(*p >= '0' && *p <= '0' + CPingWheel::NUM_SIGNALS - 1)
		{
			const int Type = *p - '0';
			if(!aUsed[Type])
			{
				pOut[Slot++] = Type;
				aUsed[Type] = true;
			}
		}
		p++;
	}
}

const char *CPingWheel::SignalName(int Signal) { return s_apSignalNames[Signal]; }
const char *CPingWheel::SignalShort(int Signal) { return s_apSignalShorts[Signal]; }

CPingWheel::CPingWheel()
{
	CPingWheel::OnReset();
}

void CPingWheel::ConKeyPingWheel(IConsole::IResult *pResult, void *pUserData)
{
	CPingWheel *pSelf = (CPingWheel *)pUserData;

	if(pSelf->GameClient()->m_Scoreboard.IsActive())
		return;

	if(!g_Config.m_TcPingwheel)
	{
		pSelf->m_Active = false;
		return;
	}

	// [TClient] 观察模式也允许使用（落点见 GetPingWorldPos）；demo 回放不可发
	if(pSelf->Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return;

	if(pSelf->GameClient()->m_BindWheel.IsActive())
		pSelf->m_Active = false;
	else
	{
		const bool Activating = pResult->GetInteger(0) != 0;
		// 激活瞬间锁定 ping 落点
		if(Activating && !pSelf->m_Active)
			pSelf->m_PingWorldPos = pSelf->GetPingWorldPos();
		pSelf->m_Active = Activating;
	}
}

void CPingWheel::ConTestPing(IConsole::IResult *pResult, void *pUserData)
{
	CPingWheel *pSelf = (CPingWheel *)pUserData;
	if(pSelf->Client()->State() != IClient::STATE_ONLINE)
		return;
	// 调试：直接在准星位置发一次危险信号（不经轮盘）
	pSelf->Ping(0, pSelf->GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy]);
}

void CPingWheel::OnConsoleInit()
{
	Console()->Register("+tc_pingwheel", "", CFGFLAG_CLIENT, ConKeyPingWheel, this, "Open ping selector");
	Console()->Register("tc_testping", "", CFGFLAG_CLIENT, ConTestPing, this, "Send a test ping at cursor position");
}

void CPingWheel::OnReset()
{
	m_WasActive = false;
	m_Active = false;
	m_SelectedSignal = -1;
	m_vEmoteQueue.clear();
	m_EmoteRecvCount = -1;
	m_EmoteFrameDoneTime = -1e9f;
	m_EmoteTestFired = false;
	for(CMarker &Marker : m_aMarkers)
		Marker.m_StartTime = -1.0f;
}

void CPingWheel::OnRelease()
{
	m_Active = false;
}

bool CPingWheel::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(!m_Active)
		return false;

	Ui()->ConvertMouseMove(&x, &y, CursorType);
	m_SelectorMouse += vec2(x, y);
	return true;
}

bool CPingWheel::OnInput(const IInput::CEvent &Event)
{
	if(IsActive() && Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		OnRelease();
		return true;
	}
	return false;
}

void CPingWheel::TryLoadIcons()
{
	if(m_IconsTried)
		return;
	m_IconsTried = true;
	// 固定图标：data/tclient/pings/<信号名>.png（危险.png 等），缺失则用内置绘制
	for(int i = 0; i < NUM_SIGNALS; i++)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "tclient/pings/%s.png", SignalName(i));
		m_aSignalTextures[i] = Graphics()->LoadTexture(aBuf, IStorage::TYPE_ALL);
	}
}

void CPingWheel::TryLoadSounds()
{
	if(m_SoundsTried)
		return;
	m_SoundsTried = true;
	// 固定音效：data/tclient/pings/<信号名>.wv（DDNet 原生 WavPack 格式，优先）→ .wav 回退；
	// 都失败则该信号不播音（id=-1）。加载结果打日志，没声音时控制台能直接看到原因。
	for(int i = 0; i < NUM_SIGNALS; i++)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "tclient/pings/%s.wv", SignalName(i));
		m_aSoundIds[i] = Sound()->LoadWV(aBuf, IStorage::TYPE_ALL);
		if(m_aSoundIds[i] < 0)
		{
			str_format(aBuf, sizeof(aBuf), "tclient/pings/%s.wav", SignalName(i));
			m_aSoundIds[i] = Sound()->LoadWav(aBuf, IStorage::TYPE_ALL);
		}
		log_info("pingwheel", "sound '%s' -> id=%d", SignalName(i), m_aSoundIds[i]);
	}
}

void CPingWheel::PlaySignalSound(int Signal, vec2 WorldPos)
{
	TryLoadSounds();
	const int SoundId = m_aSoundIds[Signal];
	if(SoundId < 0)
		return;

	// 按距离衰减：以自己角色的位置为准（观战/死亡时用镜头中心），超出可听半径就完全静音。
	// 这样信号只在附近的玩家才听得到，而不是全图都响。
	CGameClient *pGC = GameClient();
	const vec2 Listener = pGC->m_Snap.m_pLocalCharacter ? pGC->m_LocalCharacterPos : pGC->m_Camera.m_Center;
	const float Range = (float)g_Config.m_TcPingwheelSoundRange;
	const float Distance = distance(Listener, WorldPos);
	float Volume = 1.0f;
	if(Range > 0.0f)
	{
		if(Distance >= Range)
			return;
		Volume = 1.0f - Distance / Range;
		Volume *= Volume; // 平方衰减，远处掉得快
	}
	if(Volume <= 0.01f)
		return;
	GameClient()->m_Sounds.PlaySample(CSounds::CHN_PINGWHEEL, SoundId, 0, Volume);
}

vec2 CPingWheel::GetPingWorldPos() const
{
	CGameClient *pGC = GameClient();
	// 准星贴图是用 zoom=1 的投影画的（见 CHud::RenderCursor），地图和标记用的是相机
	// zoom 投影，两者只在 zoom==1 时重合。所以不能直接把 m_aTargetPos 当世界坐标发出去，
	// 否则不同缩放的玩家会 ping 到不同位置。这里反过来算：先取准星的屏幕位置，再用
	// 地图投影反解成世界坐标 —— 玩家看到准星压在哪，ping 就落在哪，与缩放无关。
	const vec2 Center = pGC->m_Camera.m_Center;
	const CScreenRect CursorRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), 1.0f);
	const vec2 CursorWorld = pGC->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
	const vec2 Fraction = (CursorWorld - CursorRect.m_TopLeft) / (CursorRect.m_BottomRight - CursorRect.m_TopLeft);

	const CScreenRect MapRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), pGC->m_Camera.m_Zoom);
	return MapRect.m_TopLeft + Fraction * (MapRect.m_BottomRight - MapRect.m_TopLeft);
}

vec2 CPingWheel::WorldToUi(vec2 WorldPos) const
{
	const vec2 Center = GameClient()->m_Camera.m_Center;
	CScreenRect ScreenRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom);
	const vec2 Fraction = (WorldPos - ScreenRect.m_TopLeft) / (ScreenRect.m_BottomRight - ScreenRect.m_TopLeft);
	const CUIRect *pUiScreen = Ui()->Screen();
	return vec2(pUiScreen->x, pUiScreen->y) + Fraction * vec2(pUiScreen->w, pUiScreen->h);
}

int CPingWheel::SignalFromMessage(const char *pMessage)
{
	int Signal, X, Y;
	if(DecodeBinary(pMessage, &Signal, &X, &Y))
		return Signal;
	const char *pCur = str_startswith(pMessage, CHAT_PREFIX); // 旧明文格式 ⟨P:id:...
	if(pCur)
		return str_toint(pCur);
	return -1;
}

void CPingWheel::AddMarker(int Signal, vec2 WorldPos)
{
	// 找空槽，满了换最老的
	CMarker *pSlot = &m_aMarkers[0];
	for(CMarker &Marker : m_aMarkers)
	{
		if(Marker.m_StartTime < 0.0f)
		{
			pSlot = &Marker;
			break;
		}
		if(Marker.m_StartTime < pSlot->m_StartTime)
			pSlot = &Marker;
	}
	pSlot->m_Type = Signal;
	pSlot->m_WorldPos = WorldPos;
	pSlot->m_StartTime = Client()->LocalTime();
}

void CPingWheel::OnRelayPayload(const char *pText)
{
	if(!g_Config.m_TcPingwheel) // 功能开关归本模块，relay 是纯 transport
		return;
	int Signal, X, Y;
	if(!DecodeBinary(pText, &Signal, &X, &Y) || Signal < 0 || Signal >= NUM_SIGNALS)
		return;
	AddMarker(Signal, vec2((float)X, (float)Y));
	PlaySignalSound(Signal, vec2((float)X, (float)Y));
}

void CPingWheel::Ping(int Signal, vec2 WorldPos)
{
	if(!g_Config.m_TcPingwheel)
		return;

	const bool Alive = GameClient()->m_Snap.m_pLocalCharacter != nullptr;
	char aBuf[24];
	EncodeBinary(Signal, (int)std::round(WorldPos.x), (int)std::round(WorldPos.y), aBuf);
	if(g_Config.m_TcRelay && GameClient()->m_RelayClient.Online())
	{
		// 信令广播：旁观/死亡也能发，不占用游戏服的聊天与表情，原版玩家无感知
		GameClient()->m_RelayClient.Broadcast(aBuf);
	}
	else
	{
		if(!Alive)
		{
			// 表情在服务器侧有活体硬检查（旁观者无 character 直接丢弃），
			// 部分服务器对旁观者聊天/私聊限制极严（实测：私聊 4 条即封）
			GameClient()->m_Chat.Echo("旁观/死亡状态：信令未连接，信号仅本地显示");
			AddMarker(Signal, WorldPos);
			PlaySignalSound(Signal, WorldPos);
			return;
		}
		if(g_Config.m_TcPingwheelEmote)
		{
			// 表情通道 fallback：帧头+9 个表情全服广播，间隔 tc_pingwheel_emote_ms
			// （默认 60ms，必须 ≥ 服务器表情冷却）。对所有人可见但只有 TClient
			// 知道语义。
			EmotePing(Signal, (int)std::round(WorldPos.x), (int)std::round(WorldPos.y));
		}
		else
		{
			GameClient()->m_Chat.Echo("信令未连接且表情/聊天通道均已关闭，信号仅本地显示");
			AddMarker(Signal, WorldPos);
			PlaySignalSound(Signal, WorldPos);
			return;
		}
	}

	AddMarker(Signal, WorldPos);
	PlaySignalSound(Signal, WorldPos);
}

int CPingWheel::LayoutType(int Sector) const
{
	if(Sector < 0 || Sector >= NUM_SIGNALS)
		return 0;
	int aLayout[NUM_SIGNALS];
	PingWheelParseLayout(g_Config.m_TcPingwheelLayout, aLayout);
	return aLayout[Sector];
}

void CPingWheel::LayoutMove(int Sector, int Dir)
{
	if(Sector < 0 || Sector >= NUM_SIGNALS)
		return;
	const int Other = Sector + Dir;
	if(Other < 0 || Other >= NUM_SIGNALS)
		return;
	int aLayout[NUM_SIGNALS];
	PingWheelParseLayout(g_Config.m_TcPingwheelLayout, aLayout);
	const int Tmp = aLayout[Sector];
	aLayout[Sector] = aLayout[Other];
	aLayout[Other] = Tmp;
	char aBuf[16];
	int Pos = 0;
	for(int i = 0; i < NUM_SIGNALS; i++)
	{
		if(i)
			aBuf[Pos++] = ',';
		aBuf[Pos++] = (char)('0' + aLayout[i]);
	}
	aBuf[Pos] = '\0';
	str_copy(g_Config.m_TcPingwheelLayout, aBuf, sizeof(g_Config.m_TcPingwheelLayout));
}

void CPingWheel::OnMessage(int MsgType, void *pRawMsg)
{
	if(GameClient()->m_SuppressEvents)
		return;

	// 表情通道接收：帧头 + 9 个数据表情 → 信号+坐标。收集中的表情一律按数据
	// 处理（OOP=0 也是合法数据位），空闲时遇到帧头才开始收集，1.5s 超时重置。
	if(MsgType == NETMSGTYPE_SV_EMOTICON && g_Config.m_TcPingwheel)
	{
		CNetMsg_Sv_Emoticon *pMsg = (CNetMsg_Sv_Emoticon *)pRawMsg;
		// 自回显：丢失检测计数后跳过（发送时已本地出标记）
		if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
			pMsg->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
		{
			m_EmoteEchoTotal++;
			return;
		}

		const float Now = Client()->LocalTime();
		if(m_EmoteRecvCount >= 0 && Now - m_EmoteRecvStart <= 1.5f)
		{
			m_EmoteRecvPayload = (m_EmoteRecvPayload << 4) | (uint64_t)(pMsg->m_Emoticon & 0xF);
			m_EmoteRecvCount++;
			if(m_EmoteRecvCount >= 9)
			{
				const uint64_t Payload = m_EmoteRecvPayload >> 1;
				const int Sig = (int)((Payload >> 32) & 7);
				const int Px = (int)((Payload >> 16) & 0xFFFF) - 32768;
				const int Py = (int)(Payload & 0xFFFF) - 32768;
				m_EmoteRecvCount = -1;
				if(Sig >= 0 && Sig < NUM_SIGNALS)
				{
					AddMarker(Sig, vec2((float)Px, (float)Py));
					PlaySignalSound(Sig, vec2((float)Px, (float)Py));
				}
			}
		}
		else
		{
			m_EmoteRecvCount = -1;
			if(pMsg->m_Emoticon == EMOTICON_OOP)
			{
				m_EmoteRecvCount = 0;
				m_EmoteRecvPayload = 0;
				m_EmoteRecvStart = Now;
			}
		}
		return;
	}

	if(MsgType != NETMSGTYPE_SV_CHAT || !g_Config.m_TcPingwheel)
		return;

	CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

	// 新紧凘格式（⟨B:...）
	int Signal, X, Y;
	if(DecodeBinary(pMsg->m_pMessage, &Signal, &X, &Y))
	{
		if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
			pMsg->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
			return; // 自己在线时的回声不重复显示（发送时已本地出标记）
		AddMarker(Signal, vec2((float)X, (float)Y));
		PlaySignalSound(Signal, vec2((float)X, (float)Y));
		return;
	}

	// 旧明文格式（⟨P:id:x:y:名⟩，旧版 TClient 发的，接收兼容）
	if(!str_startswith(pMsg->m_pMessage, CHAT_PREFIX))
		return;

	// 自己在线时的回声不重复显示（发送时已本地出标记）；demo 回放里 LocalClientId 无效，全收
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK &&
		pMsg->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
		return;

	const char *pCur = pMsg->m_pMessage + str_length(CHAT_PREFIX);
	auto NextInt = [](const char *&p) -> int {
		int Value = str_toint(p);
		while(*p && *p != ':')
			p++;
		if(*p == ':')
			p++;
		return Value;
	};
	Signal = NextInt(pCur);
	X = NextInt(pCur);
	Y = NextInt(pCur);
	if(Signal < 0 || Signal >= NUM_SIGNALS || absolute(X) > 100000 || absolute(Y) > 100000)
		return;

	AddMarker(Signal, vec2((float)X, (float)Y));
	if(pMsg->m_ClientId != GameClient()->m_Snap.m_LocalClientId)
		PlaySignalSound(Signal, vec2((float)X, (float)Y));
}

void CPingWheel::RenderMarkers()
{
	const vec2 Center = GameClient()->m_Camera.m_Center;
	const CScreenRect ScreenRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), GameClient()->m_Camera.m_Zoom);
	// 保存并还原投影：这个组件后面还有 WarList / 灵动岛 / 聊天等按屏幕坐标绘制的东西，
	// 不还原会把世界投影漏给它们。
	const CScreenRect PrevRect = Graphics()->GetScreen();
	Graphics()->MapScreen(ScreenRect);

	const float Now = Client()->LocalTime();
	for(const CMarker &Marker : m_aMarkers)
	{
		if(Marker.m_StartTime < 0.0f)
			continue;
		const float Age = Now - Marker.m_StartTime;
		if(Age < 0.0f || Age > MARKER_LIFE)
			continue;
		const float Fade = Age > MARKER_LIFE - 1.0f ? 1.0f - (Age - (MARKER_LIFE - 1.0f)) : 1.0f;
		const vec3 &Color = s_aSignalColors[Marker.m_Type];
		const float Phase = std::fmod(Age, 0.9f) / 0.9f;

		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(Color.r, Color.g, Color.b, (1.0f - Phase) * 0.6f * Fade);
		Graphics()->DrawCircle(Marker.m_WorldPos.x, Marker.m_WorldPos.y, 18.0f + Phase * 46.0f, 48);
		Graphics()->QuadsEnd();

		if(m_aSignalTextures[Marker.m_Type].IsValid())
		{
			// 自定义图标
			Graphics()->TextureSet(m_aSignalTextures[Marker.m_Type]);
			Graphics()->QuadsSetSubset(0, 0, 1, 1);
			Graphics()->QuadsBegin();
			IGraphics::CQuadItem QuadItem(Marker.m_WorldPos.x, Marker.m_WorldPos.y, 56.0f, 56.0f);
			Graphics()->QuadsDraw(&QuadItem, 1);
			Graphics()->QuadsEnd();
		}
		else
		{
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(Color.r, Color.g, Color.b, 0.85f * Fade);
			Graphics()->DrawCircle(Marker.m_WorldPos.x, Marker.m_WorldPos.y, 18.0f, 48);
			Graphics()->QuadsEnd();

			TextRender()->TextColor(0.08f, 0.08f, 0.08f, Fade);
			TextRender()->Text(Marker.m_WorldPos.x - 13.0f, Marker.m_WorldPos.y - 13.0f, 26.0f, SignalShort(Marker.m_Type), -1.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
	}

	Graphics()->MapScreen(PrevRect);
}

void CPingWheel::RenderSpecCursor()
{
	CGameClient *pGC = GameClient();
	if(!pGC->m_Snap.m_SpecInfo.m_Active || m_Active || Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return;
	if(pGC->m_Menus.IsActive() || pGC->m_Chat.IsActive() || pGC->m_Scoreboard.IsActive() || pGC->m_Emoticon.IsActive())
		return;
	const vec2 Mouse = pGC->m_Controls.m_aMousePos[g_Config.m_ClDummy];
	const float Now = Client()->LocalTime();
	if(Mouse != m_LastSpecMouse)
	{
		m_LastSpecMouse = Mouse;
		m_LastSpecMouseMove = Now;
	}
	// 1.5s 不动开始淡出（0.5s 淡完），比系统准星贴图低调得多
	const float Idle = Now - m_LastSpecMouseMove;
	const float Alpha = Idle < 1.5f ? 1.0f : std::max(0.0f, 1.0f - (Idle - 1.5f) / 0.5f);
	if(Alpha <= 0.0f)
		return;
	const vec2 Pos = WorldToUi(pGC->m_Controls.m_aTargetPos[g_Config.m_ClDummy]);
	Ui()->MapScreen();
	// 小圆圈光标：淡晕外圈 + 半透明内核，白色低透明度，只标个位置不抢视线
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.22f * Alpha);
	Graphics()->DrawCircle(Pos.x, Pos.y, 6.5f, 24);
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.55f * Alpha);
	Graphics()->DrawCircle(Pos.x, Pos.y, 2.8f, 24);
	Graphics()->QuadsEnd();
}

void CPingWheel::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	TryLoadIcons();
	TryLoadSounds();
	RenderMarkers();
	RenderSpecCursor();
	PumpEmoteQueue();

	// [TEMP-DIAG] tc_emote_diag=1：进服活着 5 秒后自动发一次测试 ping
	if(g_Config.m_TcEmoteDiag == 1 && Client()->State() == IClient::STATE_ONLINE &&
		GameClient()->m_Snap.m_pLocalCharacter && !m_EmoteTestFired &&
		GameClient()->m_RelayClient.Online()) // 等信令注册完成再发
	{
		m_EmoteTestFired = true;
		log_info("relaydiag", "auto test ping sent (relay online)");
		Ping(0, GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy]);
	}

	static const auto QuadEaseInOut = [](float t) -> float {
		if(t == 0.0f)
			return 0.0f;
		if(t == 1.0f)
			return 1.0f;
		return (t < 0.5f) ? (2.0f * t * t) : (1.0f - std::pow(-2.0f * t + 2.0f, 2) / 2.0f);
	};
	static const auto PositiveMod = [](float x, float y) -> float {
		return std::fmod(x + y, y);
	};

	const float AnimationTime = (float)g_Config.m_TcAnimateWheelTime / 1000.0f;

	if(!m_Active)
	{
		if(m_WasActive && m_SelectedSignal != -1)
			Ping(m_SelectedSignal, m_PingWorldPos);
		m_WasActive = false;

		if(AnimationTime == 0.0f)
			return;

		m_AnimationTime -= Client()->RenderFrameTime() * 3.0f; // 关闭动画 3 倍速
		if(m_AnimationTime <= 0.0f)
		{
			m_AnimationTime = 0.0f;
			return;
		}
	}
	else
	{
		if(AnimationTime != 0.0f)
		{
			m_AnimationTime += Client()->RenderFrameTime();
			if(m_AnimationTime > AnimationTime)
				m_AnimationTime = AnimationTime;
		}
		m_WasActive = true;
	}

	// [TClient] 观察模式也允许轮盘；仅「非观察且无本地角色」（死亡/未进图）时强制关闭
	if(!GameClient()->m_Snap.m_SpecInfo.m_Active && !GameClient()->m_Snap.m_pLocalCharacter)
	{
		m_Active = false;
		m_WasActive = false;
	}

	if(length(m_SelectorMouse) > 170.0f)
		m_SelectorMouse = normalize(m_SelectorMouse) * 170.0f;

	const float SelectorAngle = angle(m_SelectorMouse);

	int aLayout[NUM_SIGNALS];
	PingWheelParseLayout(g_Config.m_TcPingwheelLayout, aLayout);

	m_SelectedSignal = -1;
	if(length(m_SelectorMouse) > 40.0f)
		m_SelectedSignal = aLayout[(int)PositiveMod(std::round(SelectorAngle / (2.0f * pi) * (float)NUM_SIGNALS), NUM_SIGNALS)];

	const float Phase = AnimationTime == 0.0f ? 1.0f : QuadEaseInOut(m_AnimationTime / AnimationTime);
	const vec2 ScreenCenter = WorldToUi(m_PingWorldPos);

	Ui()->MapScreen();

	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.35f * Phase);
	Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, 190.0f * Phase, 64);
	Graphics()->QuadsEnd();

	for(int Signal = 0; Signal < NUM_SIGNALS; Signal++)
	{
		const float ItemAngle = 2.0f * pi * Signal / (float)NUM_SIGNALS;
		const vec2 Position = ScreenCenter + direction(ItemAngle) * 115.0f * Phase;
		const int Type = aLayout[Signal];
		const bool Selected = m_SelectedSignal == Type;

		if(m_aSignalTextures[Type].IsValid())
		{
			// 自定义图标（data/tclient/pings/signalN.png）
			Graphics()->TextureSet(m_aSignalTextures[Type]);
			Graphics()->QuadsSetSubset(0, 0, 1, 1);
			Graphics()->QuadsBegin();
			const float ImgSize = (Selected ? 64.0f : 50.0f) * Phase;
			IGraphics::CQuadItem QuadItem(Position.x, Position.y, ImgSize, ImgSize);
			Graphics()->QuadsDraw(&QuadItem, 1);
			Graphics()->QuadsEnd();
		}
		else
		{
			const float Size = (Selected ? 34.0f : 26.0f) * Phase;

			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			const vec3 &Color = s_aSignalColors[Type];
			Graphics()->SetColor(Color.r, Color.g, Color.b, (Selected ? 0.95f : 0.7f) * Phase);
			Graphics()->DrawCircle(Position.x, Position.y, Size, 48);
			Graphics()->QuadsEnd();

			CUIRect Label;
			Label.x = Position.x - Size;
			Label.y = Position.y - Size;
			Label.w = Size * 2.0f;
			Label.h = Size * 2.0f;
			TextRender()->TextColor(0.05f, 0.05f, 0.05f, Phase);
			Ui()->DoLabel(&Label, SignalName(Type), 20.0f * Phase, TEXTALIGN_MC);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
	}

	RenderTools()->RenderCursor(ScreenCenter + m_SelectorMouse, 24.0f, Phase);
}
