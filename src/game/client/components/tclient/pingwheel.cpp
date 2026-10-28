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

void CPingWheel::OnConsoleInit()
{
	Console()->Register("+tc_pingwheel", "", CFGFLAG_CLIENT, ConKeyPingWheel, this, "Open ping selector");
}

void CPingWheel::OnReset()
{
	m_WasActive = false;
	m_Active = false;
	m_SelectedSignal = -1;
	m_RateWindowStart = -1e9f;
	m_RateSentCount = 0;
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

bool CPingWheel::RateLimitedAndRecord()
{
	const float Now = Client()->LocalTime();
	if(Now - m_RateWindowStart >= RATE_WINDOW)
	{
		m_RateWindowStart = Now;
		m_RateSentCount = 0;
	}
	if(m_RateSentCount >= RATE_COUNT)
		return true;
	m_RateSentCount++;
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

void CPingWheel::PlaySignalSound(int Signal)
{
	TryLoadSounds();
	const int SoundId = m_aSoundIds[Signal];
	if(SoundId >= 0)
		GameClient()->m_Sounds.PlaySample(CSounds::CHN_GUI, SoundId, 0, 1.0f);
}

vec2 CPingWheel::GetPingWorldPos() const
{
	CGameClient *pGC = GameClient();
	// 观察模式：锁定跟观→被观战者准星；自由视角/无扩展数据→鼠标所在世界位置
	if(pGC->m_Snap.m_SpecInfo.m_Active)
	{
		if(pGC->m_CursorInfo.IsAvailable())
			return pGC->m_CursorInfo.WorldTarget();
		const CUIRect *pUiScreen = Ui()->Screen();
		const vec2 Fraction = (Ui()->MousePos() - vec2(pUiScreen->x, pUiScreen->y)) / vec2(pUiScreen->w, pUiScreen->h);
		CScreenRect ScreenRect = Graphics()->MapScreenToWorld(pGC->m_Camera.m_Center.x, pGC->m_Camera.m_Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), 1.0f);
		return pGC->m_Camera.m_Center + (Fraction - vec2(0.5f, 0.5f)) * (ScreenRect.m_BottomRight - ScreenRect.m_TopLeft) * pGC->m_Camera.m_Zoom;
	}
	// 正常游戏：准星世界坐标
	return pGC->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
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

void CPingWheel::Ping(int Signal, vec2 WorldPos)
{
	if(!g_Config.m_TcPingwheel)
		return;
	if(RateLimitedAndRecord())
	{
		// 限频：只本地提示，不发送（防服务器禁言）
		char aBuf[128];
		const int Wait = std::max(1, (int)std::ceil(RATE_WINDOW - (Client()->LocalTime() - m_RateWindowStart)));
		str_format(aBuf, sizeof(aBuf), "信号发送过于频繁：%d 秒内最多 %d 次，%d 秒后可再发", (int)RATE_WINDOW, RATE_COUNT, Wait);
		GameClient()->m_Chat.Echo(aBuf);
		return;
	}

	char aBuf[96];
	str_format(aBuf, sizeof(aBuf), "%s%d:%d:%d:%s⟩", CHAT_PREFIX, Signal,
		(int)std::round(WorldPos.x), (int)std::round(WorldPos.y), SignalName(Signal));
	GameClient()->m_Chat.SendChat(g_Config.m_TcPingwheelTeam, aBuf);

	AddMarker(Signal, WorldPos);
	PlaySignalSound(Signal);
}

int CPingWheel::LayoutType(int Sector) const
{
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
	if(MsgType != NETMSGTYPE_SV_CHAT || !g_Config.m_TcPingwheel || GameClient()->m_SuppressEvents)
		return;

	CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
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
	const int Signal = NextInt(pCur);
	const int X = NextInt(pCur);
	const int Y = NextInt(pCur);
	if(Signal < 0 || Signal >= NUM_SIGNALS || absolute(X) > 100000 || absolute(Y) > 100000)
		return;

	AddMarker(Signal, vec2((float)X, (float)Y));
	if(pMsg->m_ClientId != GameClient()->m_Snap.m_LocalClientId)
		PlaySignalSound(Signal);
}

void CPingWheel::RenderMarkers()
{
	const vec2 Center = GameClient()->m_Camera.m_Center;
	CScreenRect ScreenRect = Graphics()->MapScreenToWorld(Center.x, Center.y, 100.0f, 100.0f, 100.0f, 0, 0, Graphics()->ScreenAspect(), 1.0f);
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
}

void CPingWheel::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	TryLoadIcons();
	TryLoadSounds();
	RenderMarkers();

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
	const vec2 ScreenCenter = Ui()->Screen()->Center();

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
