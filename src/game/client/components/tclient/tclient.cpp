#include "tclient.h"

#include "data_version.h"

#include <base/fs.h>
#include <base/hash.h>
#include <base/io.h>
#include <base/log.h>
#include <base/process.h>
#include <base/time.h>



#include <engine/client.h>
#include <engine/client/enums.h>
#include <engine/external/regex.h>
#include <engine/external/tinyexpr.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>
#include <engine/storage.h>

#include <generated/client_data.h>

#include <game/client/animstate.h>
#include <game/client/components/chat.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/ui.h>
#include <game/localization.h>
#include <game/version.h>

#if defined(CONF_FAMILY_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <base/windows.h>
#include <windows.h>
#include <tlhelp32.h>
#endif
#include <engine/external/ed25519/ed25519.h>
#include "update_public_key.h"

#include <cstdarg>

static constexpr const char *TCLIENT_INFO_URL = "https://update.tclient.app/info.json";

// ---- v1.4 客户端自动更新 ----------------------------------------------------
// 只认自有仓库的 GitHub Releases：https://github.com/351950505/TClient
// 更新资产契约（发布脚本 tclient_scripts/make_release_assets.py 生成）：
//   TClient-update-win64.json  清单（product/channel/version/platform/asset/size/sha256）
//   TClient-update-win64.zip   整包，由独立的 TClientUpdater.exe 在主进程退出后替换
// 旧的 update.tclient.app 逐文件更新器与本流程无关，也不受这里影响。
static constexpr const char *TCLIENT_UPDATE_LATEST_URL = "https://github.com/351950505/TClient/releases/latest/download/";
// 清单/签名的最后备用源：raw 不经过 release-assets 域名，但可能落后于
// Release；只有它返回的新版本能形成候选，不能单独证明本机已是最新。
static constexpr const char *TCLIENT_UPDATE_RAW_BASE = "https://raw.githubusercontent.com/351950505/TClient/master/update/";
static constexpr const char *TCLIENT_UPDATE_GH_BASE = "https://github.com/351950505/TClient/releases/download/";
static constexpr const char *TCLIENT_UPDATE_GH_REPO_PATH = "351950505/TClient";
// 2026-11-08：github.com 的 releases/download 入口大面积 504（ghfast 与自建镜像的
// 回源都走它，一起挂），唯一稳定的官方通道是 api.github.com（octet-stream 302 到
// release-assets Azure blob，实测可用）。自建镜像已换名 fast1/fast2 → github1/github2
// 并把回源改为 api.github.com 解析（不再碰 releases/download 入口）。
// 候选清单源，按顺序尝试：自建镜像 github1/github2（回源走 api，最稳）→ ghfast 加速
// → github 直链 → raw 仓库。
// 2026-11-08 晚：ghfast 从首位下移。它在故障期间持续超时，放在第一位会让每次检查
// 都先白等一整个 15 秒超时；镜像已验证可用且走 api 回源，先试它们能把最常见的
// 「有更新」路径缩短到一次请求。
#if defined(CONF_TCLIENT_RELEASE_MIRRORS)
#include <generated/tclient_update_mirrors.h>
#else
static const char *const TCLIENT_UPDATE_MANIFEST_SOURCES[] = {
	"https://github1.me3.dpdns.org/https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json",
	"https://github2.me3.dpdns.org/https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json",
	"https://ghfast.top/https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json",
	"https://github.com/351950505/TClient/releases/latest/download/TClient-update-win64.json",
	"https://raw.githubusercontent.com/351950505/TClient/master/update/TClient-update-win64.json",
};
// 与清单源一一对应的下载前缀："" = releases 直链，"@api" = api.github.com
// （先解析 release-by-tag 拿 asset url 再下载），nullptr = raw。
// 下载换源只改变前缀；版本、哈希和大小始终来自同一份已验签清单。
static const char *const TCLIENT_UPDATE_SOURCE_SIBLING_BASES[] = {
	"https://github1.me3.dpdns.org/",
	"https://github2.me3.dpdns.org/",
	"https://ghfast.top/",
	"",
	"@api",
	nullptr,
};
#endif
// @api 不是清单源（api 不回 manifest），下载入口比清单源多一个。
static_assert(std::size(TCLIENT_UPDATE_SOURCE_SIBLING_BASES) == std::size(TCLIENT_UPDATE_MANIFEST_SOURCES) + 1);
// raw 不托管大文件；每个 Release 入口每个阶段最多尝试一次。
static constexpr int TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT = std::size(TCLIENT_UPDATE_SOURCE_SIBLING_BASES) - 1;
static_assert(std::size(TCLIENT_UPDATE_SOURCE_SIBLING_BASES) == TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT + 1);
static bool UpdateSourceIsApi(const char *pBase)
{
	return pBase && str_comp(pBase, "@api") == 0;
}

static void FormatUpdateAssetUrl(char *pBuf, int BufSize, const char *pBase, const char *pTag, const char *pAsset)
{
	str_format(pBuf, BufSize, "%s%s%s/%s", pBase, TCLIENT_UPDATE_GH_BASE, pTag, pAsset);
}
static constexpr const char *TCLIENT_UPDATE_PRODUCT = "TClient";
static constexpr const char *TCLIENT_UPDATE_CHANNEL = "stable";
static constexpr const char *TCLIENT_UPDATE_PLATFORM = "win64";
static constexpr const char *TCLIENT_UPDATE_MANIFEST_ASSET = "TClient-update-win64.json";
static constexpr const char *TCLIENT_UPDATE_ZIP_ASSET = "TClient-update-win64.zip";
static constexpr const char *TCLIENT_UPDATE_HELPER = "TClientUpdater.exe";
static constexpr const char *TCLIENT_UPDATE_MANIFEST_SIG_ASSET = "TClient-update-win64.json.sig";
static constexpr const char *TCLIENT_UPDATE_HELPER_ASSET = "TClientUpdater.exe";
static constexpr const char *TCLIENT_UPDATE_BOOTSTRAP_PATH = "update/TClientUpdater.boot.exe";
static constexpr const char *TCLIENT_UPDATE_ZIP_PATH = "update/TClient-update-win64.zip";
// 自动检查节奏：启动后先等 12 秒（别和首帧/地图加载抢带宽），之后每 6 小时最多一次。
static constexpr float TCLIENT_UPDATE_FIRST_CHECK_DELAY = 12.0f;
static constexpr int64_t TCLIENT_UPDATE_CHECK_INTERVAL = 6 * 60 * 60;

// 严格三段版本号：任何一段不是纯数字就视为无效（不用 atoi 的宽松解析）。
// 版本号：任意段数（1.7.8 / 1.7.8.1 / 1.7.8.1.2 都合法），
// 逐段整数比较，缺失段按 0 处理。每段最多 6 位，超出视为非法。
static constexpr int APP_VERSION_MAX_SEGMENTS = 8;

static int ParseAppVersion(const char *pVersion, int *pOut, int MaxSegments)
{
	for(int i = 0; i < MaxSegments; i++)
		pOut[i] = 0;
	if(!pVersion || !pVersion[0])
		return -1;
	const char *p = pVersion;
	int Segment = 0;
	for(;;)
	{
		if(Segment >= MaxSegments)
			return -1;
		const char *pStart = p;
		while(*p >= '0' && *p <= '9')
			p++;
		if(p == pStart || p - pStart > 6)
			return -1;
		int Value = 0;
		for(const char *q = pStart; q < p; q++)
			Value = Value * 10 + (*q - '0');
		pOut[Segment++] = Value;
		if(*p == '\0')
			break;
		if(*p != '.')
			return -1;
		p++;
	}
	return Segment;
}

// >0 表示 A 比 B 新（段数不同时，多出的段与 0 比较）
static int CompareAppVersion(const char *pA, const char *pB)
{
	int a[APP_VERSION_MAX_SEGMENTS], b[APP_VERSION_MAX_SEGMENTS];
	const int CountA = ParseAppVersion(pA, a, APP_VERSION_MAX_SEGMENTS);
	const int CountB = ParseAppVersion(pB, b, APP_VERSION_MAX_SEGMENTS);
	if(CountA < 0 || CountB < 0)
		return 0;
	const int Count = CountA > CountB ? CountA : CountB;
	for(int i = 0; i < Count; i++)
	{
		const int Left = i < CountA ? a[i] : 0;
		const int Right = i < CountB ? b[i] : 0;
		if(Left != Right)
			return Left > Right ? 1 : -1;
	}
	return 0;
}

static const json_value &JsonObj(const json_value &Obj, const char *pKey)
{
	return Obj[pKey];
}

static const char *JsonStr(const json_value &Value)
{
	return Value.type == json_string ? Value.u.string.ptr : nullptr;
}

// 注意：不能叫 JsonBool，engine/shared/json.h 里已有同名函数（bool -> "true"/"false"）。
static bool JsonGetBool(const json_value &Value, bool Default)
{
	if(Value.type == json_boolean)
		return Value.u.boolean != 0;
	return Default;
}

static int64_t JsonGetInt(const json_value &Value, int64_t Default)
{
	if(Value.type == json_integer)
		return (int64_t)Value.u.integer;
	if(Value.type == json_double)
		return (int64_t)Value.u.dbl;
	return Default;
}

static bool IsHexSha256(const char *pStr)
{
	if(!pStr || str_length(pStr) != 64)
		return false;
	for(const char *p = pStr; *p; p++)
	{
		if(!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
			return false;
	}
	return true;
}


CTClient::CTClient()
{
	OnReset();
}

void CTClient::ConRandomTee(IConsole::IResult *pResult, void *pUserData) {}

void CTClient::ConchainRandomColor(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CTClient *pThis = static_cast<CTClient *>(pUserData);
	// Resolve type to randomize
	// Check length of type (0 = all, 1 = body, 2 = feet, 3 = skin, 4 = flag)
	bool RandomizeBody = false;
	bool RandomizeFeet = false;
	bool RandomizeSkin = false;
	bool RandomizeFlag = false;

	if(pResult->NumArguments() == 0)
	{
		RandomizeBody = true;
		RandomizeFeet = true;
		RandomizeSkin = true;
		RandomizeFlag = true;
	}
	else if(pResult->NumArguments() == 1)
	{
		const char *Type = pResult->GetString(0);
		int Length = Type ? str_length(Type) : 0;
		if(Length == 1 && Type[0] == '0')
		{ // Randomize all
			RandomizeBody = true;
			RandomizeFeet = true;
			RandomizeSkin = true;
			RandomizeFlag = true;
		}
		else if(Length == 1)
		{
			// Randomize body
			RandomizeBody = Type[0] == '1';
		}
		else if(Length == 2)
		{
			// Check for body and feet
			RandomizeBody = Type[0] == '1';
			RandomizeFeet = Type[1] == '1';
		}
		else if(Length == 3)
		{
			// Check for body, feet and skin
			RandomizeBody = Type[0] == '1';
			RandomizeFeet = Type[1] == '1';
			RandomizeSkin = Type[2] == '1';
		}
		else if(Length == 4)
		{
			// Check for body, feet, skin and flag
			RandomizeBody = Type[0] == '1';
			RandomizeFeet = Type[1] == '1';
			RandomizeSkin = Type[2] == '1';
			RandomizeFlag = Type[3] == '1';
		}
	}

	if(RandomizeBody)
		RandomBodyColor();
	if(RandomizeFeet)
		RandomFeetColor();
	if(RandomizeSkin)
		RandomSkin(pUserData);
	if(RandomizeFlag)
		RandomFlag(pUserData);
	pThis->GameClient()->SendInfo(false);
}

void CTClient::OnInit()
{
	// 独立标记供更新器只读校验，不加载 exe，也不依赖 DDNet 协议版本。
	log_info("tclient", "TClientAppVersion=" TCLIENT_APP_VERSION);
#if defined(CONF_FAMILY_WINDOWS)
	// TClient(2026-11-01):CPU 尖峰(网易云 webview GC、Defender 扫描、监控进程
	// 的秒级轮询)抢占游戏线程会造成瞬时卡顿 —— 把进程提到高于普通一档,尖峰
	// 来时调度器优先保游戏。tc_process_priority:F1 可改,重启生效。
	const DWORD PriorityClass = g_Config.m_TcProcessPriority == 2 ? HIGH_PRIORITY_CLASS :
		g_Config.m_TcProcessPriority == 1 ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS;
	SetPriorityClass(GetCurrentProcess(), PriorityClass);
#endif
	TextRender()->SetCustomFace(g_Config.m_TcCustomFont);
	m_pGraphics = Kernel()->RequestInterface<IEngineGraphics>();
	FetchTClientInfo();

	char aError[512] = "";
	if(!Storage()->FileExists("tclient/gui_logo.png", IStorage::TYPE_ALL))
		str_format(aError, sizeof(aError), TCLocalize("%s not found", DATA_VERSION_PATH), "data/tclient/gui_logo.png");
	if(aError[0] == '\0')
		CheckDataVersion(aError, sizeof(aError), Storage()->OpenFile(DATA_VERSION_PATH, IOFLAG_READ, IStorage::TYPE_ALL));
	if(aError[0] != '\0')
	{
		SWarning Warning(aError, TCLocalize("You have probably only installed the TClient DDNet.exe which is not supported, please use the entire TClient folder", "data_version.h"));
		Client()->AddWarning(Warning);
	}
}

static bool LineShouldHighlight(const char *pLine, const char *pName)
{
	const char *pHL = str_utf8_find_nocase(pLine, pName);
	if(pHL)
	{
		int Length = str_length(pName);
		if(Length > 0 && (pLine == pHL || pHL[-1] == ' ') && (pHL[Length] == 0 || pHL[Length] == ' ' || pHL[Length] == '.' || pHL[Length] == '!' || pHL[Length] == ',' || pHL[Length] == '?' || pHL[Length] == ':'))
			return true;
	}
	return false;
}

bool CTClient::SendNonDuplicateMessage(int Team, const char *pLine)
{
	if(str_comp(pLine, m_PreviousOwnMessage) != 0)
	{
		GameClient()->m_Chat.SendChat(Team, pLine);
		return true;
	}
	str_copy(m_PreviousOwnMessage, pLine);
	return false;
}

void CTClient::OnMessage(int MsgType, void *pRawMsg)
{
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return;

	if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
		int ClientId = pMsg->m_ClientId;

		if(ClientId < 0 || ClientId >= MAX_CLIENTS)
			return;
		int LocalId = GameClient()->m_Snap.m_LocalClientId;
		if(ClientId == LocalId)
			str_copy(m_PreviousOwnMessage, pMsg->m_pMessage);

		bool PingMessage = false;

		bool ValidIds = GameClient()->m_aLocalIds[0] >= 0 && GameClient()->m_aLocalIds[0] < MAX_CLIENTS &&
			(!GameClient()->Client()->DummyConnected() || (GameClient()->m_aLocalIds[1] >= 0 && GameClient()->m_aLocalIds[1] < MAX_CLIENTS));

		if(ValidIds && ClientId >= 0 && ClientId != GameClient()->m_aLocalIds[0] && (!GameClient()->Client()->DummyConnected() || ClientId != GameClient()->m_aLocalIds[1]))
		{
			PingMessage |= LineShouldHighlight(pMsg->m_pMessage, GameClient()->m_aClients[GameClient()->m_aLocalIds[0]].m_aName);
			PingMessage |= GameClient()->Client()->DummyConnected() && LineShouldHighlight(pMsg->m_pMessage, GameClient()->m_aClients[GameClient()->m_aLocalIds[1]].m_aName);
		}

		if(pMsg->m_Team == TEAM_WHISPER_RECV)
			PingMessage = true;

		if(!PingMessage)
			return;

		char aPlayerName[MAX_NAME_LENGTH];
		str_copy(aPlayerName, GameClient()->m_aClients[ClientId].m_aName, sizeof(aPlayerName));

		bool PlayerMuted = GameClient()->m_aClients[ClientId].m_Foe || GameClient()->m_aClients[ClientId].m_ChatIgnore;
		if(g_Config.m_TcAutoReplyMuted && PlayerMuted)
		{
			char aBuf[256];
			if(pMsg->m_Team == TEAM_WHISPER_RECV || ServerCommandExists("w"))
				str_format(aBuf, sizeof(aBuf), "/w %s %s", aPlayerName, g_Config.m_TcAutoReplyMutedMessage);
			else
				str_format(aBuf, sizeof(aBuf), "%s: %s", aPlayerName, g_Config.m_TcAutoReplyMutedMessage);
			SendNonDuplicateMessage(0, aBuf);
			return;
		}

		bool WindowActive = m_pGraphics && m_pGraphics->WindowActive();
		if(g_Config.m_TcAutoReplyMinimized && !WindowActive && m_pGraphics)
		{
			char aBuf[256];
			if(pMsg->m_Team == TEAM_WHISPER_RECV || ServerCommandExists("w"))
				str_format(aBuf, sizeof(aBuf), "/w %s %s", aPlayerName, g_Config.m_TcAutoReplyMinimizedMessage);
			else
				str_format(aBuf, sizeof(aBuf), "%s: %s", aPlayerName, g_Config.m_TcAutoReplyMinimizedMessage);
			SendNonDuplicateMessage(0, aBuf);
			return;
		}
	}

	if(MsgType == NETMSGTYPE_SV_VOTESET)
	{
		const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy]; // Do not care about spec behaviour
		const bool Afk = LocalId >= 0 && GameClient()->m_aClients[LocalId].m_Afk; // TODO Depends on server afk time
		CNetMsg_Sv_VoteSet *pMsg = (CNetMsg_Sv_VoteSet *)pRawMsg;
		if(pMsg->m_Timeout && !Afk)
		{
			char aDescription[VOTE_DESC_LENGTH];
			char aReason[VOTE_REASON_LENGTH];
			str_copy(aDescription, pMsg->m_pDescription);
			str_copy(aReason, pMsg->m_pReason);
			bool KickVote = str_startswith(aDescription, "Kick ") != nullptr ? true : false;
			bool SpecVote = str_startswith(aDescription, "Pause ") != nullptr ? true : false;
			bool SettingVote = !KickVote && !SpecVote;
			bool RandomMapVote = SettingVote && str_find_nocase(aDescription, "random");
			bool MapCoolDown = SettingVote && (str_find_nocase(aDescription, "change map") || str_find_nocase(aDescription, "no not change map"));
			bool CategoryVote = SettingVote && (str_find_nocase(aDescription, "☐") || str_find_nocase(aDescription, "☒"));
			bool FunVote = SettingVote && str_find_nocase(aDescription, "funvote");
			bool MapVote = SettingVote && !RandomMapVote && !MapCoolDown && !CategoryVote && !FunVote && (str_find_nocase(aDescription, "Map:") || str_find_nocase(aDescription, "★") || str_find_nocase(aDescription, "✰"));

			if(g_Config.m_TcAutoVoteWhenFar && (MapVote || RandomMapVote))
			{
				int RaceTime = 0;
				if(GameClient()->m_Snap.m_pGameInfoObj && GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME)
					RaceTime = (Client()->GameTick(g_Config.m_ClDummy) + GameClient()->m_Snap.m_pGameInfoObj->m_WarmupTimer) / Client()->GameTickSpeed();

				if(RaceTime / 60 >= g_Config.m_TcAutoVoteWhenFarTime)
				{
					CGameClient::CClientData *pVoteCaller = nullptr;
					int CallerId = -1;
					for(int i = 0; i < MAX_CLIENTS; i++)
					{
						if(!GameClient()->m_aStats[i].IsActive())
							continue;

						char aBuf[MAX_NAME_LENGTH + 4];
						str_format(aBuf, sizeof(aBuf), "\'%s\'", GameClient()->m_aClients[i].m_aName);
						if(str_find_nocase(pMsg->m_pDescription, aBuf) != nullptr)
						{
							pVoteCaller = &GameClient()->m_aClients[i];
							CallerId = i;
							break;
						}
					}
					if(pVoteCaller)
					{
						bool Friend = pVoteCaller->m_Friend;
						bool SameTeam = GameClient()->m_Teams.Team(GameClient()->m_Snap.m_LocalClientId) == pVoteCaller->m_Team && pVoteCaller->m_Team != 0;
						bool MySelf = CallerId == LocalId;

						if(!Friend && !SameTeam && !MySelf)
						{
							GameClient()->m_Voting.Vote(-1);
							if(str_comp(g_Config.m_TcAutoVoteWhenFarMessage, "") != 0)
								SendNonDuplicateMessage(0, g_Config.m_TcAutoVoteWhenFarMessage);
						}
					}
				}
			}
		}
	}

	auto &vServerCommands = GameClient()->m_Chat.m_vServerCommands;
	auto AddSpecId = [&](bool Enable) {
		static const CChat::CCommand SpecId("specid", "v[id]", "Spectate a player");
		vServerCommands.erase(std::remove_if(vServerCommands.begin(), vServerCommands.end(), [](const CChat::CCommand &Command) { return Command == SpecId; }), vServerCommands.end());
		if(Enable)
			vServerCommands.push_back(SpecId);
		GameClient()->m_Chat.m_ServerCommandsNeedSorting = true;
	};
	if(MsgType == NETMSGTYPE_SV_COMMANDINFO)
	{
		CNetMsg_Sv_CommandInfo *pMsg = (CNetMsg_Sv_CommandInfo *)pRawMsg;
		if(str_comp_nocase(pMsg->m_pName, "spec") == 0)
			AddSpecId(!ServerCommandExists("specid"));
		else if(str_comp_nocase(pMsg->m_pName, "specid") == 0)
			AddSpecId(false);
		return;
	}
	if(MsgType == NETMSGTYPE_SV_COMMANDINFOREMOVE)
	{
		CNetMsg_Sv_CommandInfoRemove *pMsg = (CNetMsg_Sv_CommandInfoRemove *)pRawMsg;
		if(str_comp_nocase(pMsg->m_pName, "spec") == 0)
			AddSpecId(false);
		else if(str_comp_nocase(pMsg->m_pName, "specid") == 0)
			AddSpecId(ServerCommandExists("spec"));
		return;
	}
}

void CTClient::ConSpecId(IConsole::IResult *pResult, void *pUserData)
{
	((CTClient *)pUserData)->SpecId(pResult->GetInteger(0));
}

bool CTClient::ChatDoSpecId(const char *pInput)
{
	const char *pNumber = str_startswith_nocase(pInput, "/specid ");
	if(!pNumber)
		return false;

	const int Length = str_length(pInput);
	CChat::CHistoryEntry *pEntry = GameClient()->m_Chat.m_History.Allocate(sizeof(CChat::CHistoryEntry) + Length);
	pEntry->m_Team = 0;
	str_copy(pEntry->m_aText, pInput, Length + 1);

	int ClientId = 0;
	if(!str_toint(pNumber, &ClientId))
		return true;

	SpecId(ClientId);
	return true;
}

static bool IsValidSpecClientId(int ClientId)
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS;
}

void CTClient::SpecId(int ClientId)
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	if(!IsValidSpecClientId(ClientId))
		return;

	if(Client()->State() == IClient::STATE_DEMOPLAYBACK || GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		GameClient()->m_Spectator.Spectate(ClientId);
		return;
	}

	const auto &Player = GameClient()->m_aClients[ClientId];
	if(!Player.m_Active)
		return;
	char aBuf[256];
	str_copy(aBuf, "/spec \"");
	char *pDst = aBuf + strlen(aBuf);
	str_escape(&pDst, Player.m_aName, aBuf + sizeof(aBuf));
	str_append(aBuf, "\"");
	GameClient()->m_Chat.SendChat(0, aBuf);
}

void CTClient::ConEmoteCycle(IConsole::IResult *pResult, void *pUserData)
{
	CTClient &This = *(CTClient *)pUserData;
	This.m_EmoteCycle += 1;
	if(This.m_EmoteCycle > 15)
		This.m_EmoteCycle = 0;
	This.GameClient()->m_Emoticon.Emote(This.m_EmoteCycle);
}

void CTClient::AirRescue()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;
	const int ClientId = GameClient()->m_Snap.m_LocalClientId;
	if(ClientId < 0 || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		return;
	if(GameClient()->m_Snap.m_aCharacters[ClientId].m_HasExtendedDisplayInfo && (GameClient()->m_Snap.m_aCharacters[ClientId].m_ExtendedData.m_Flags & CHARACTERFLAG_PRACTICE_MODE) == 0)
	{
		GameClient()->Echo(TCLocalize("You are not in practice"));
		return;
	}

	auto IsIndexAirLike = [&](int Index) {
		const auto Tile = Collision()->GetTileIndex(Index);
		return Tile == TILE_AIR || Tile == TILE_UNFREEZE || Tile == TILE_DUNFREEZE;
	};
	auto IsPosAirLike = [&](vec2 Pos) {
		const int Index = Collision()->GetPureMapIndex(Pos);
		return IsIndexAirLike(Index);
	};
	auto IsRadiusAirLike = [&](vec2 Pos, int Radius) {
		for(int y = -Radius; y <= Radius; ++y)
			for(int x = -Radius; x <= Radius; ++x)
				if(!IsPosAirLike(Pos + vec2(x, y) * 32.0f))
					return false;
		return true;
	};

	auto &AirRescuePositions = m_aAirRescuePositions[g_Config.m_ClDummy];
	while(!AirRescuePositions.empty())
	{
		// Get latest pos from positions
		const vec2 NewPos = AirRescuePositions.front();
		AirRescuePositions.pop_front();
		// Check for safety
		if(!IsRadiusAirLike(NewPos, 2))
			continue;
		// Do it
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "/tpxy %f %f", NewPos.x / 32.0f, NewPos.y / 32.0f);
		GameClient()->m_Chat.SendChat(0, aBuf);
		return;
	}

	GameClient()->Echo(TCLocalize("No safe position found"));
}

void CTClient::ConAirRescue(IConsole::IResult *pResult, void *pUserData)
{
	((CTClient *)pUserData)->AirRescue();
}

void CTClient::ConCalc(IConsole::IResult *pResult, void *pUserData)
{
	int Error = 0;
	double Out = te_interp(pResult->GetString(0), &Error);
	if(Out == NAN || Error != 0)
		log_info("tclient", "Calc error: %d", Error);
	else
		log_info("tclient", "Calc result: %lf", Out);
}

void CTClient::OnConsoleInit()
{
	Console()->Register("calc", "r[expression]", CFGFLAG_CLIENT, ConCalc, this, "Evaluate an expression");
	Console()->Register("airrescue", "", CFGFLAG_CLIENT, ConAirRescue, this, "Rescue to a nearby air tile");

	Console()->Register("tc_random_player", "s[type]", CFGFLAG_CLIENT, ConRandomTee, this, "Randomize player color (0 = all, 1 = body, 2 = feet, 3 = skin, 4 = flag) example: 0011 = randomize skin and flag [number is position]");
	Console()->Chain("tc_random_player", ConchainRandomColor, this);

	Console()->Register("spec_id", "v[id]", CFGFLAG_CLIENT, ConSpecId, this, "Spectate a player by Id");

	Console()->Register("emote_cycle", "", CFGFLAG_CLIENT, ConEmoteCycle, this, "Cycle through emotes");

	Console()->Register("tc_update_check", "", CFGFLAG_CLIENT, ConUpdateCheck, this, "Check GitHub releases for a newer TClient version");

	Console()->Chain(
		"tc_allow_any_res", [](IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData) {
			pfnCallback(pResult, pCallbackUserData);
			((CTClient *)pUserData)->QueueForcedAspectUpdate();
		},
		this);

	Console()->Chain(
		"tc_regex_chat_ignore", [](IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData) {
			if(pResult->NumArguments() == 1)
			{
				auto Re = Regex(pResult->GetString(0));
				if(!Re.error().empty())
				{
					log_error("tclient", "Invalid regex: %s", Re.error().c_str());
					return;
				}
				((CTClient *)pUserData)->m_RegexChatIgnore = std::move(Re);
			}
			pfnCallback(pResult, pCallbackUserData);
		},
		this);
}

void CTClient::RandomBodyColor()
{
	g_Config.m_ClPlayerColorBody = ColorHSLA((std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, 1.0f).Pack(false);
}

void CTClient::RandomFeetColor()
{
	g_Config.m_ClPlayerColorFeet = ColorHSLA((std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, 1.0f).Pack(false);
}

void CTClient::RandomSkin(void *pUserData)
{
	CTClient *pThis = static_cast<CTClient *>(pUserData);
	const auto &Skins = pThis->GameClient()->m_Skins.SkinList().Skins();
	str_copy(g_Config.m_ClPlayerSkin, Skins[std::rand() % (int)Skins.size()].SkinContainer()->Name());
}

void CTClient::RandomFlag(void *pUserData)
{
	CTClient *pThis = static_cast<CTClient *>(pUserData);
	// get the flag count
	int FlagCount = pThis->GameClient()->m_CountryFlags.Num();

	// get a random flag number
	int FlagNumber = std::rand() % FlagCount;

	// get the flag name
	const CCountryFlags::CCountryFlag &Flag = pThis->GameClient()->m_CountryFlags.GetByIndex(FlagNumber);

	// set the flag code as number
	g_Config.m_PlayerCountry = Flag.m_CountryCode;
}

void CTClient::DoFinishCheck()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	if(g_Config.m_TcChangeNameNearFinish <= 0)
		return;
	m_FinishTextTimeout -= Client()->RenderFrameTime();
	if(m_FinishTextTimeout > 0.0f)
		return;
	m_FinishTextTimeout = 1.0f;
	// Check for finish tile
	const auto &NearTile = [this](vec2 Pos, int RadiusInTiles, int Tile) -> bool {
		const CCollision *pCollision = GameClient()->Collision();
		for(int i = 0; i <= RadiusInTiles * 2; ++i)
		{
			const float h = std::ceil(std::pow(std::sin((float)i * pi / 2.0f / (float)RadiusInTiles), 0.5f) * pi / 2.0f * (float)RadiusInTiles);
			const vec2 Pos1 = vec2(Pos.x + (float)(i - RadiusInTiles) * 32.0f, Pos.y - h);
			const vec2 Pos2 = vec2(Pos.x + (float)(i - RadiusInTiles) * 32.0f, Pos.y + h);
			std::vector<int> vIndices = pCollision->GetMapIndices(Pos1, Pos2);
			if(vIndices.empty())
				vIndices.push_back(pCollision->GetPureMapIndex(Pos1));
			for(int &Index : vIndices)
			{
				if(pCollision->GetTileIndex(Index) == Tile)
					return true;
				if(pCollision->GetFrontTileIndex(Index) == Tile)
					return true;
			}
		}
		return false;
	};
	const auto &SendUrgentRename = [this](int Conn, const char *pNewName) {
		CNetMsg_Cl_ChangeInfo Msg;
		Msg.m_pName = pNewName;
		Msg.m_pClan = Conn == 0 ? g_Config.m_PlayerClan : g_Config.m_ClDummyClan;
		Msg.m_Country = Conn == 0 ? g_Config.m_PlayerCountry : g_Config.m_ClDummyCountry;
		Msg.m_pSkin = Conn == 0 ? g_Config.m_ClPlayerSkin : g_Config.m_ClDummySkin;
		Msg.m_UseCustomColor = Conn == 0 ? g_Config.m_ClPlayerUseCustomColor : g_Config.m_ClDummyUseCustomColor;
		Msg.m_ColorBody = Conn == 0 ? g_Config.m_ClPlayerColorBody : g_Config.m_ClDummyColorBody;
		Msg.m_ColorFeet = Conn == 0 ? g_Config.m_ClPlayerColorFeet : g_Config.m_ClDummyColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		GameClient()->m_aCheckInfo[Conn] = Client()->GameTickSpeed(); // 1 second
	};
	int Dummy = g_Config.m_ClDummy;
	const auto &Player = GameClient()->m_aClients[GameClient()->m_aLocalIds[Dummy]];
	if(!Player.m_Active)
		return;
	const char *NewName = g_Config.m_TcFinishName;
	if(str_comp(Player.m_aName, NewName) == 0)
		return;
	if(!NearTile(Player.m_RenderPos, g_Config.m_TcFinishNameScanRadius, TILE_FINISH))
		return;
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), TCLocalize("Changing name to %s near finish"), NewName);
	GameClient()->Echo(aBuf);
	SendUrgentRename(Dummy, NewName);
}

bool CTClient::ServerCommandExists(const char *pCommand)
{
	for(const auto &Command : GameClient()->m_Chat.m_vServerCommands)
		if(str_comp_nocase(pCommand, Command.m_aName) == 0)
			return true;
	return false;
}

void CTClient::OnRender()
{
	if(m_pTClientInfoTask)
	{
		if(m_pTClientInfoTask->State() == EHttpState::DONE)
		{
			FinishTClientInfo();
			ResetTClientInfoTask();
		}
	}

	DoFinishCheck();

	// 更新器状态机推进 + 启动后延迟一次自动检查。检查只查版本，
	// 不下载、不安装、不退出——用户点了按钮才会继续。
	PollUpdateTasks();
	if(m_UpdateState == EUpdateState::IDLE && UpdateShouldAutoCheck())
	{
		static float s_FirstRenderTime = -1.0f;
		if(s_FirstRenderTime < 0.0f)
			s_FirstRenderTime = Client()->LocalTime();
		if(Client()->LocalTime() - s_FirstRenderTime >= TCLIENT_UPDATE_FIRST_CHECK_DELAY)
			UpdateCheck(false);
	}

	RenderPlayerMiddleMenu();
}

// 游戏内中键玩家菜单(2026-11-01):中键点到玩家弹出「偷取皮肤 / 加入队伍」。
// 光标世界坐标直接用准星 target(controls),不做相机换算 —— 本地角色不存在
// (纯观战)时菜单不响应;已打开的面板照常显示/可执行。
void CTClient::RenderPlayerMiddleMenu()
{
	if(Client()->State() != IClient::STATE_ONLINE)
	{
		m_PlayerMenuCid = -1;
		return;
	}

	const bool HaveLocalChar = GameClient()->m_Snap.m_pLocalCharacter != nullptr;
	if(HaveLocalChar)
	{
		const vec2 CursorWorld = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
		int Hover = -1;
		float BestDistSq = 70.0f * 70.0f;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(i == GameClient()->m_Snap.m_LocalClientId)
				continue;
			const CNetObj_PlayerInfo *pInfo = GameClient()->m_Snap.m_apPlayerInfos[i];
			if(!pInfo)
				continue;
			const float DistSq = distance_squared(GameClient()->m_aClients[i].m_Predicted.m_Pos, CursorWorld);
			if(DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Hover = i;
			}
		}

		if(m_PlayerMenuCid < 0)
		{
			if(Hover >= 0 && Ui()->MouseButtonClicked(2))
			{
				m_PlayerMenuCid = Hover;
				m_PlayerMenuPos = Ui()->MousePos();
			}
		}
		else if(Ui()->MouseButtonClicked(2))
		{
			// 已开面板时再中键:悬停到别的玩家就切换目标,否则关闭
			if(Hover >= 0 && Hover != m_PlayerMenuCid)
			{
				m_PlayerMenuCid = Hover;
				m_PlayerMenuPos = Ui()->MousePos();
			}
			else
				m_PlayerMenuCid = -1;
		}
	}

	if(m_PlayerMenuCid < 0)
		return;
	if(!GameClient()->m_Snap.m_apPlayerInfos[m_PlayerMenuCid])
	{
		m_PlayerMenuCid = -1;
		return;
	}

	CUIRect Panel;
	Panel.x = m_PlayerMenuPos.x + 8.0f;
	Panel.y = m_PlayerMenuPos.y + 8.0f;
	Panel.w = 160.0f;
	Panel.h = 68.0f;
	const CUIRect *pScreen = Ui()->Screen();
	if(Panel.x + Panel.w > pScreen->w)
		Panel.x = m_PlayerMenuPos.x - Panel.w - 8.0f;
	if(Panel.y + Panel.h > pScreen->h)
		Panel.y = m_PlayerMenuPos.y - Panel.h - 8.0f;

	Panel.Draw(ColorRGBA(0.06f, 0.06f, 0.08f, 0.96f), IGraphics::CORNER_ALL, 6.0f);
	const CUIRect PanelOuter = Panel;
	CUIRect Row, Use;
	Panel.Margin(6.0f, &Panel);

	char aTitle[96];
	str_format(aTitle, sizeof(aTitle), TCLocalize("Player: %s"), GameClient()->m_aClients[m_PlayerMenuCid].m_aName);
	Panel.HSplitTop(14.0f, &Use, &Panel);
	Ui()->DoLabel(&Use, aTitle, 10.0f, TEXTALIGN_ML);
	Panel.HSplitTop(2.0f, nullptr, &Panel);

	static CButtonContainer s_StealButton, s_TeamButton;
	Panel.HSplitTop(22.0f, &Row, &Panel);
	if(Ui()->DoButton_PopupMenu(&s_StealButton, TCLocalize("Steal skin"), &Row, 10.0f, TEXTALIGN_ML))
	{
		// 偷皮肤:名字走控制台(触发皮肤链刷新),颜色直接写配置
		const CGameClient::CClientData &Data = GameClient()->m_aClients[m_PlayerMenuCid];
		char aCmd[MAX_SKIN_LENGTH + 32];
		str_format(aCmd, sizeof(aCmd), "player_skin \"%s\"", Data.m_aSkinName);
		Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
		g_Config.m_ClPlayerUseCustomColor = Data.m_UseCustomColor;
		g_Config.m_ClPlayerColorBody = Data.m_ColorBody;
		g_Config.m_ClPlayerColorFeet = Data.m_ColorFeet;
		m_PlayerMenuCid = -1;
	}
	Panel.HSplitTop(4.0f, nullptr, &Panel);
	Panel.HSplitTop(22.0f, &Row, &Panel);
	if(Ui()->DoButton_PopupMenu(&s_TeamButton, TCLocalize("Join team"), &Row, 10.0f, TEXTALIGN_ML))
	{
		// 加入队伍:/team <队伍数字>(不是玩家 id);对方没进队时不发
		const int DDTeam = GameClient()->m_Teams.Team(m_PlayerMenuCid);
		if(DDTeam > 0)
		{
			char aCmd[32];
			str_format(aCmd, sizeof(aCmd), "say \"/team %d\"", DDTeam);
			Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			m_PlayerMenuCid = -1;
		}
	}

	// 点面板外左键 / 按右键关闭(中键的开关在上方分支处理)
	if(Ui()->MouseButtonClicked(1) || (Ui()->MouseButtonClicked(0) && !Ui()->MouseInside(&PanelOuter)))
		m_PlayerMenuCid = -1;
}

bool CTClient::NeedUpdate()
{
	return str_comp(m_aVersionStr, "0") != 0;
}

void CTClient::ResetTClientInfoTask()
{
	if(m_pTClientInfoTask)
	{
		m_pTClientInfoTask->Abort();
		m_pTClientInfoTask = nullptr;
	}
}

void CTClient::FetchTClientInfo()
{
	if(m_pTClientInfoTask && !m_pTClientInfoTask->Done())
		return;
	char aUrl[256];
	str_copy(aUrl, TCLIENT_INFO_URL);
	m_pTClientInfoTask = HttpGet(aUrl);
	m_pTClientInfoTask->Timeout(CTimeout{10000, 0, 500, 10});
	m_pTClientInfoTask->IpResolve(IPRESOLVE::V4);
	Http()->Run(m_pTClientInfoTask);
}

typedef std::tuple<int, int, int> TVersion;
static const TVersion gs_InvalidTCVersion = std::make_tuple(-1, -1, -1);

static TVersion ToTCVersion(char *pStr)
{
	int aVersion[3] = {0, 0, 0};
	const char *p = strtok(pStr, ".");

	for(int i = 0; i < 3 && p; ++i)
	{
		if(!str_isallnum(p))
			return gs_InvalidTCVersion;

		aVersion[i] = str_toint(p);
		p = strtok(nullptr, ".");
	}

	if(p)
		return gs_InvalidTCVersion;

	return std::make_tuple(aVersion[0], aVersion[1], aVersion[2]);
}

void CTClient::FinishTClientInfo()
{
	json_value *pJson = m_pTClientInfoTask->ResultJson();
	if(!pJson)
		return;
	const json_value &Json = *pJson;
	const json_value &CurrentVersion = Json["version"];

	if(CurrentVersion.type == json_string)
	{
		char aNewVersionStr[64];
		str_copy(aNewVersionStr, CurrentVersion);
		char aCurVersionStr[64];
		str_copy(aCurVersionStr, TCLIENT_VERSION);
		if(ToTCVersion(aNewVersionStr) > ToTCVersion(aCurVersionStr))
		{
			str_copy(m_aVersionStr, CurrentVersion);
		}
		else
		{
			m_aVersionStr[0] = '0';
			m_aVersionStr[1] = '\0';
		}
		m_FetchedTClientInfo = true;
	}

	json_value_free(pJson);
}

// ---- v1.4 客户端自动更新实现 ------------------------------------------------

void CTClient::SetUpdateStatus(const char *pStatus)
{
	str_copy(m_aUpdateStatus, pStatus ? pStatus : "");
}

void CTClient::UpdateFail(const char *pFormat, ...)
{
	va_list Args;
	va_start(Args, pFormat);
	str_format_v(m_aUpdateStatus, sizeof(m_aUpdateStatus), pFormat, Args);
	va_end(Args);
	m_UpdateState = EUpdateState::FAILED;
	m_UpdateRelease = SUpdateRelease();
	m_UpdateFoundCurrentRelease = false;
	// 失败即中止所有在途任务，避免 PollUpdateTasks 下一帧重复触发
	ResetUpdateTasks();
	log_warn("tclient-update", "%s", m_aUpdateStatus);
}

void CTClient::ResetUpdateTasks()
{
	m_UpdateRetryAt = -1.0f;
	m_UpdateRetryHelper = false;
	BootstrapHelperPending = false;
	// 必须四个任务全清：残留的 sig/helper 任务会让下一次检查读到旧结果，
	// 甚至在 abort 后仍被 Finish* 解引用（崩溃）。
	if(m_pUpdateManifestTask)
	{
		m_pUpdateManifestTask->Abort();
		m_pUpdateManifestTask = nullptr;
	}
	if(m_pUpdateManifestSigTask)
	{
		m_pUpdateManifestSigTask->Abort();
		m_pUpdateManifestSigTask = nullptr;
	}
	if(m_pUpdateDownloadTask)
	{
		m_pUpdateDownloadTask->Abort();
		m_pUpdateDownloadTask = nullptr;
	}
	if(m_pUpdateHelperTask)
	{
		m_pUpdateHelperTask->Abort();
		m_pUpdateHelperTask = nullptr;
	}
	if(m_pUpdateApiResolveTask)
	{
		m_pUpdateApiResolveTask->Abort();
		m_pUpdateApiResolveTask = nullptr;
	}
}

// 更新清单 Ed25519 验签：公钥内嵌（update_public_key.h），签名由发布脚本
// 用私钥生成。验签不过 = 清单被篡改或来源不对，直接拒绝，不进入下载。
bool CTClient::VerifyUpdateManifestSignature(const unsigned char *pManifest, size_t ManifestSize, const unsigned char *pSignature, size_t SignatureSize)
{
	// ed25519_verify always reads a full 64-byte signature; a short buffer
	// would be an out-of-bounds read.
	if(SignatureSize != 64)
		return false;
	return ed25519_verify(pSignature, pManifest, ManifestSize, s_aUpdateEd25519PublicKey) == 1;
}

bool CTClient::UpdateShouldAutoCheck() const
{
	if(m_UpdateState == EUpdateState::CHECKING || m_UpdateState == EUpdateState::DOWNLOADING || m_UpdateState == EUpdateState::DOWNLOADED)
		return false;
	if(m_UpdateState == EUpdateState::AVAILABLE)
		return false;
	const int64_t Now = time_timestamp();
	if(m_LastUpdateCheck == 0)
		return true;
	return Now - m_LastUpdateCheck >= TCLIENT_UPDATE_CHECK_INTERVAL;
}

void CTClient::UpdateCheck(bool Force)
{
	if(m_UpdateState == EUpdateState::CHECKING || m_UpdateState == EUpdateState::DOWNLOADING)
		return;
	ResetUpdateTasks();
	m_UpdateRelease = SUpdateRelease();
	m_aUpdateReleaseTag[0] = '\0';
	m_aUpdateManifestUrl[0] = '\0';
	m_aUpdateManifestSigUrl[0] = '\0';
	m_aUpdateHelperUrl[0] = '\0';
	m_aUpdateBootstrapPath[0] = '\0';
	BootstrapHelperPending = false;
	m_UpdateManifestSource = 0;
	m_UpdateFoundCurrentRelease = false;
	m_UpdateDownloadSource = 0;
	m_UpdateDownloadAttempt = 0;
	m_LastUpdateCheck = time_timestamp();
	m_UpdateState = EUpdateState::CHECKING;
	SetUpdateStatus(TCLocalize("Checking for updates..."));
	log_info("tclient-update", "checking for update (force=%d, current=%s)", Force ? 1 : 0, TCLIENT_APP_VERSION);
	FetchUpdateManifest();
}



void CTClient::FetchUpdateManifest()
{
	if(m_UpdateManifestSource >= (int)std::size(TCLIENT_UPDATE_MANIFEST_SOURCES))
	{
		if(m_UpdateFoundCurrentRelease)
		{
			m_UpdateState = EUpdateState::UP_TO_DATE;
			SetUpdateStatus(TCLocalize("You are on the latest version"));
			return;
		}
		UpdateFail(TCLocalize("Update check failed: the manifest could not be downloaded"));
		return;
	}
	str_copy(m_aUpdateManifestUrl, TCLIENT_UPDATE_MANIFEST_SOURCES[m_UpdateManifestSource]);
	str_format(m_aUpdateManifestSigUrl, sizeof(m_aUpdateManifestSigUrl), "%s.sig", m_aUpdateManifestUrl);
	m_pUpdateManifestTask = HttpGet(m_aUpdateManifestUrl);
	m_pUpdateManifestTask->Timeout(CTimeout{15000, 0, 800, 10});
	m_pUpdateManifestTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateManifestTask->MaxResponseSize(64 * 1024);
	m_pUpdateManifestTask->FailOnErrorStatus(true);
	Http()->Run(m_pUpdateManifestTask);
}

void CTClient::NextUpdateManifestSource(const char *pReason)
{
	log_warn("tclient-update", "manifest source %d rejected: %s", m_UpdateManifestSource + 1, pReason);
	// 签名和清单构成一个不可拆开的来源对；换源必须丢弃两者。
	ResetUpdateTasks();
	m_UpdateRelease = SUpdateRelease();
	++m_UpdateManifestSource;
	FetchUpdateManifest();
}

void CTClient::FinishUpdateManifest(std::shared_ptr<IHttpRequest> pSigTask, std::shared_ptr<IHttpRequest> pManifestTask)
{
	// 两个任务都是同一来源的快照；验签优先于任何 JSON 解析。
	if(!pManifestTask || !pSigTask)
	{
		NextUpdateManifestSource(TCLocalize("Update check failed: the manifest could not be downloaded"));
		return;
	}
	unsigned char *pManifest = nullptr;
	size_t ManifestSize = 0;
	unsigned char *pSig = nullptr;
	size_t SigSize = 0;
	pManifestTask->Result(&pManifest, &ManifestSize);
	pSigTask->Result(&pSig, &SigSize);
	if(!pManifest || !pSig || !VerifyUpdateManifestSignature(pManifest, ManifestSize, pSig, SigSize))
	{
		NextUpdateManifestSource(TCLocalize("Update rejected: manifest signature is invalid"));
		return;
	}
	log_info("tclient-update", "manifest signature verified");

	json_value *pJson = pManifestTask->ResultJson();
	if(!pJson || pJson->type != json_object)
	{
		if(pJson)
			json_value_free(pJson);
		NextUpdateManifestSource(TCLocalize("Update check failed: the manifest is not valid JSON"));
		return;
	}

	const char *pProduct = JsonStr(JsonObj(*pJson, "product"));
	const char *pChannel = JsonStr(JsonObj(*pJson, "channel"));
	const char *pVersion = JsonStr(JsonObj(*pJson, "version"));
	const char *pPlatform = JsonStr(JsonObj(*pJson, "platform"));
	const char *pAsset = JsonStr(JsonObj(*pJson, "asset"));
	const char *pSha256 = JsonStr(JsonObj(*pJson, "sha256"));
	const int64_t Size = JsonGetInt(JsonObj(*pJson, "size"), -1);
	const json_value &Helper = JsonObj(*pJson, "helper");
	const char *pHelperSha = JsonStr(JsonObj(Helper, "sha256"));
	const int64_t HelperSize = JsonGetInt(JsonObj(Helper, "size"), -1);
	int aVersion[APP_VERSION_MAX_SEGMENTS];
	const char *pError = nullptr;
	if(!pProduct || str_comp(pProduct, TCLIENT_UPDATE_PRODUCT) != 0)
		pError = TCLocalize("Update rejected: wrong product");
	else if(!pChannel || str_comp(pChannel, TCLIENT_UPDATE_CHANNEL) != 0)
		pError = TCLocalize("Update rejected: wrong channel");
	else if(!pPlatform || str_comp(pPlatform, TCLIENT_UPDATE_PLATFORM) != 0)
		pError = TCLocalize("Update rejected: wrong platform");
	else if(!pVersion || str_length(pVersion) >= (int)sizeof(m_UpdateRelease.m_aVersion) || ParseAppVersion(pVersion, aVersion, APP_VERSION_MAX_SEGMENTS) < 0)
		pError = TCLocalize("Update rejected: version does not match the release");
	else if(!pAsset || str_comp(pAsset, TCLIENT_UPDATE_ZIP_ASSET) != 0)
		pError = TCLocalize("Update rejected: unexpected asset name");
	else if(!IsHexSha256(pSha256))
		pError = TCLocalize("Update rejected: missing or malformed SHA-256");
	else if(Size <= 0)
		pError = TCLocalize("Update rejected: missing file size");
	else if(!IsHexSha256(pHelperSha) || HelperSize <= 0)
		pError = TCLocalize("Update rejected: missing bootstrap helper");
	if(pError)
	{
		json_value_free(pJson);
		NextUpdateManifestSource(pError);
		return;
	}

	const int VersionOrder = CompareAppVersion(pVersion, TCLIENT_APP_VERSION);
	if(VersionOrder <= 0)
	{
		// 旧镜像不能阻断后面的新版本。raw 可能长期滞后，不能证明已是最新。
		// 下载源多了 @api，不能再拿下载表的同下标判断最后一个 raw 清单源。
		if(VersionOrder == 0 && m_UpdateManifestSource < (int)std::size(TCLIENT_UPDATE_MANIFEST_SOURCES) - 1)
			m_UpdateFoundCurrentRelease = true;
		log_info("tclient-update", "source version %s is not newer than %s; checking remaining sources", pVersion, TCLIENT_APP_VERSION);
		json_value_free(pJson);
		NextUpdateManifestSource("manifest version is not newer");
		return;
	}

	str_copy(m_UpdateRelease.m_aAsset, pAsset);
	str_copy(m_UpdateRelease.m_aSha256, pSha256);
	m_UpdateRelease.m_Size = Size;
	str_copy(m_UpdateRelease.m_aHelperSha256, pHelperSha);
	m_UpdateRelease.m_HelperSize = HelperSize;
	str_format(m_UpdateRelease.m_aTag, sizeof(m_UpdateRelease.m_aTag), "v%s", pVersion);
	str_copy(m_UpdateRelease.m_aVersion, pVersion);
	// 从 raw 取得清单时先用 Release 直链，其余来源优先用自己的下载入口。
	m_UpdateDownloadSource = TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateManifestSource] == nullptr ? TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT - 1 : m_UpdateManifestSource;
	const char *pBase = TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource];
	FormatUpdateAssetUrl(m_UpdateRelease.m_aZipUrl, sizeof(m_UpdateRelease.m_aZipUrl), pBase, m_UpdateRelease.m_aTag, TCLIENT_UPDATE_ZIP_ASSET);
	FormatUpdateAssetUrl(m_aUpdateHelperUrl, sizeof(m_aUpdateHelperUrl), pBase, m_UpdateRelease.m_aTag, TCLIENT_UPDATE_HELPER_ASSET);
	m_UpdateRelease.m_Valid = true;
	json_value_free(pJson);

	m_UpdateState = EUpdateState::AVAILABLE;
	char aStatus[128];
	str_format(aStatus, sizeof(aStatus), TCLocalize("Version %s is available"), m_UpdateRelease.m_aVersion);
	SetUpdateStatus(aStatus);
	log_info("tclient-update", "update available: %s -> %s (%lld bytes)", TCLIENT_APP_VERSION, m_UpdateRelease.m_aVersion, (long long)m_UpdateRelease.m_Size);
}

#if defined(CONF_FAMILY_WINDOWS)
// 更新前结束本客户端拉起的后台监控进程：它们会锁住自己的 exe，导致 helper
// 替换失败并整体回滚（2026-10-31 实测 island_media_monitor.exe 拒绝访问）。
// 只杀安装目录匹配的进程，不碰其他安装/其他用户的同名进程。
static void StopOwnHelperProcesses(const wchar_t *pInstallDir, bool OnlyIfLastClient = false)
{
	std::wstring InstallDir = pInstallDir;
	if(InstallDir.empty())
		return;
	for(wchar_t &c : InstallDir)
	{
		if(c == L'/')
			c = L'\\';
	}
	if(InstallDir.back() != L'\\')
		InstallDir += L'\\';

	struct CShutdownLock
	{
		HANDLE m_Handle = nullptr;
		bool m_Locked = false;
		~CShutdownLock()
		{
			if(m_Locked)
				ReleaseMutex(m_Handle);
			if(m_Handle)
				CloseHandle(m_Handle);
		}
	} ShutdownLock;

	wchar_t aOwnPath[32768];
	DWORD OwnPathSize = std::size(aOwnPath);
	const wchar_t *pOwnName = nullptr;
	if(OnlyIfLastClient)
	{
		if(!QueryFullProcessImageNameW(GetCurrentProcess(), 0, aOwnPath, &OwnPathSize))
			return;
		const std::wstring OwnPath(aOwnPath, OwnPathSize);
		const size_t NameStart = OwnPath.find_last_of(L"\\/");
		if(NameStart == std::wstring::npos ||
			CompareStringOrdinal(aOwnPath, (int)NameStart + 1, InstallDir.c_str(), (int)InstallDir.size(), TRUE) != CSTR_EQUAL)
			return;
		pOwnName = aOwnPath + NameStart + 1;

		// Serialize normal shutdowns, including separate installs. Updates keep the force-cleanup default.
		ShutdownLock.m_Handle = CreateMutexW(nullptr, FALSE, L"Local\\TClientHelperShutdown");
		if(!ShutdownLock.m_Handle)
			return;
		const DWORD WaitResult = WaitForSingleObject(ShutdownLock.m_Handle, 5000);
		if(WaitResult != WAIT_OBJECT_0 && WaitResult != WAIT_ABANDONED)
			return;
		ShutdownLock.m_Locked = true;

		// Keep this signaled event until process teardown. Concurrent quitters can ignore
		// a PID already shutting down, so their last active instance performs the cleanup.
		static HANDLE s_ExitingEvent = nullptr;
		if(!s_ExitingEvent)
		{
			const std::wstring EventName = L"Local\\TClientHelperExit-" + std::to_wstring(GetCurrentProcessId());
			s_ExitingEvent = CreateEventW(nullptr, TRUE, TRUE, EventName.c_str());
		}
		if(!s_ExitingEvent)
			return;
	}

	HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(Snapshot == INVALID_HANDLE_VALUE)
		return;

	PROCESSENTRY32W Entry;
	Entry.dwSize = sizeof(Entry);
	if(OnlyIfLastClient)
	{
		bool OtherClientActive = !Process32FirstW(Snapshot, &Entry);
		if(!OtherClientActive)
		{
			do
			{
				if(Entry.th32ProcessID == GetCurrentProcessId() || _wcsicmp(Entry.szExeFile, pOwnName) != 0)
					continue;
				HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, Entry.th32ProcessID);
				if(!Process)
				{
					// A vanished PID is harmless; access denial or unknown state must preserve shared helpers.
					if(GetLastError() == ERROR_INVALID_PARAMETER)
						continue;
					OtherClientActive = true;
					break;
				}
				wchar_t aPath[32768];
				DWORD PathSize = std::size(aPath);
				if(WaitForSingleObject(Process, 0) != WAIT_OBJECT_0)
				{
					if(!QueryFullProcessImageNameW(Process, 0, aPath, &PathSize))
						OtherClientActive = WaitForSingleObject(Process, 0) != WAIT_OBJECT_0;
					else if(CompareStringOrdinal(aPath, PathSize, aOwnPath, OwnPathSize, TRUE) == CSTR_EQUAL)
					{
						const std::wstring EventName = L"Local\\TClientHelperExit-" + std::to_wstring(Entry.th32ProcessID);
						HANDLE ExitingEvent = OpenEventW(SYNCHRONIZE, FALSE, EventName.c_str());
						OtherClientActive = !ExitingEvent || WaitForSingleObject(ExitingEvent, 0) != WAIT_OBJECT_0;
						if(ExitingEvent)
							CloseHandle(ExitingEvent);
					}
				}
				CloseHandle(Process);
				if(OtherClientActive)
					break;
			} while(Process32NextW(Snapshot, &Entry));
			if(!OtherClientActive && GetLastError() != ERROR_NO_MORE_FILES)
				OtherClientActive = true;
		}
		if(OtherClientActive)
		{
			CloseHandle(Snapshot);
			return;
		}
	}

	if(Process32FirstW(Snapshot, &Entry))
	{
		do
		{
			if(_wcsicmp(Entry.szExeFile, L"island_media_monitor.exe") != 0 &&
				_wcsicmp(Entry.szExeFile, L"netease-watcher.exe") != 0)
				continue;
			HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Entry.th32ProcessID);
			if(!Process)
				continue;
			wchar_t aPath[32768];
			DWORD PathSize = std::size(aPath);
			if(QueryFullProcessImageNameW(Process, 0, aPath, &PathSize))
			{
				const std::wstring ExpectedPath = InstallDir + Entry.szExeFile;
				if(CompareStringOrdinal(aPath, PathSize, ExpectedPath.c_str(), (int)ExpectedPath.size(), TRUE) == CSTR_EQUAL)
				{
					TerminateProcess(Process, 0);
					WaitForSingleObject(Process, 2000);
				}
			}
			CloseHandle(Process);
		} while(Process32NextW(Snapshot, &Entry));
	}
	CloseHandle(Snapshot);
}
#endif

void CTClient::UpdateStartDownload()
{
	if(!m_UpdateRelease.m_Valid)
	{
		UpdateFail(TCLocalize("No verified update available"));
		return;
	}
	if(m_UpdateState == EUpdateState::DOWNLOADING)
		return;
	m_UpdateDownloadAttempt = 0;
	m_UpdateDownloadSource = TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateManifestSource] == nullptr ? TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT - 1 : m_UpdateManifestSource;
	m_UpdateRelease.m_aApiZipUrl[0] = '\0';
	m_aUpdateApiHelperUrl[0] = '\0';
	StartDownloadTask();
}

// "@api" 源第一步：api.github.com 的 release-by-tag JSON（匿名，响应很小），
// 解析出 zip/helper 两个 asset 的 api url（octet-stream 时 302 到 release-assets）。
void CTClient::StartApiResolveTask(bool Helper)
{
	ResetUpdateTasks();
	m_UpdateState = EUpdateState::DOWNLOADING;
	m_ApiResolvePending = Helper;
	SetUpdateStatus(TCLocalize("Resolving update source..."));
	char aUrl[256];
	str_format(aUrl, sizeof(aUrl), "https://api.github.com/repos/%s/releases/tags/%s", TCLIENT_UPDATE_GH_REPO_PATH, m_UpdateRelease.m_aTag);
	log_info("tclient-update", "resolving release assets via api.github.com (helper=%d)", Helper ? 1 : 0);
	m_pUpdateApiResolveTask = HttpGet(aUrl);
	m_pUpdateApiResolveTask->Timeout(CTimeout{15000, 0, 800, 10});
	m_pUpdateApiResolveTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateApiResolveTask->MaxResponseSize(2 * 1024 * 1024);
	m_pUpdateApiResolveTask->FailOnErrorStatus(true);
	m_pUpdateApiResolveTask->LogProgress(HTTPLOG::NONE);
	Http()->Run(m_pUpdateApiResolveTask);
}

void CTClient::FinishApiResolve()
{
	std::shared_ptr<IHttpRequest> pTask = m_pUpdateApiResolveTask;
	const bool Helper = m_ApiResolvePending;
	m_pUpdateApiResolveTask = nullptr;
	m_ApiResolvePending = false;
	const char *pResolveFail = TCLocalize("Download failed or the file is corrupted");
	if(!pTask || pTask->State() != EHttpState::DONE)
	{
		RetryUpdateDownload(Helper, pResolveFail);
		return;
	}
	json_value *pRoot = pTask->ResultJson();
	if(pRoot == nullptr || pRoot == &json_value_none || pRoot->type != json_object)
	{
		if(pRoot != nullptr && pRoot != &json_value_none)
			json_value_free(pRoot);
		RetryUpdateDownload(Helper, pResolveFail);
		return;
	}
	const json_value *pAssets = json_object_get(pRoot, "assets");
	bool GotZip = false, GotHelper = false;
	if(pAssets && pAssets->type == json_array)
	{
		for(unsigned i = 0; i < json_array_length(pAssets); ++i)
		{
			const json_value *pAsset = json_array_get(pAssets, i);
			if(!pAsset || pAsset->type != json_object)
				continue;
			const json_value *pName = json_object_get(pAsset, "name");
			const json_value *pUrl = json_object_get(pAsset, "url");
			if(!pName || pName->type != json_string || !pUrl || pUrl->type != json_string)
				continue;
			if(str_comp(pName->u.string.ptr, TCLIENT_UPDATE_ZIP_ASSET) == 0)
			{
				str_copy(m_UpdateRelease.m_aApiZipUrl, pUrl->u.string.ptr);
				GotZip = true;
			}
			else if(str_comp(pName->u.string.ptr, TCLIENT_UPDATE_HELPER_ASSET) == 0)
			{
				str_copy(m_aUpdateApiHelperUrl, pUrl->u.string.ptr);
				GotHelper = true;
			}
		}
	}
	json_value_free(pRoot);
	if(!GotZip || !GotHelper)
	{
		log_warn("tclient-update", "api resolve incomplete (zip=%d helper=%d)", GotZip ? 1 : 0, GotHelper ? 1 : 0);
		RetryUpdateDownload(Helper, pResolveFail);
		return;
	}
	if(Helper)
		StartUpdateHelperTask();
	else
		StartDownloadTask();
}

void CTClient::StartDownloadTask()
{
	SHA256_DIGEST Expected;
	if(sha256_from_str(&Expected, m_UpdateRelease.m_aSha256) != 0)
	{
		UpdateFail(TCLocalize("Update rejected: missing or malformed SHA-256"));
		return;
	}
	if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource]) && !m_UpdateRelease.m_aApiZipUrl[0])
	{
		StartApiResolveTask(false);
		return;
	}
	ResetUpdateTasks();
	m_UpdateState = EUpdateState::DOWNLOADING;
	if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource]))
		str_copy(m_UpdateRelease.m_aZipUrl, m_UpdateRelease.m_aApiZipUrl);
	else
		FormatUpdateAssetUrl(m_UpdateRelease.m_aZipUrl, sizeof(m_UpdateRelease.m_aZipUrl), TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource], m_UpdateRelease.m_aTag, TCLIENT_UPDATE_ZIP_ASSET);
	SetUpdateStatus(TCLocalize("Downloading update..."));
	log_info("tclient-update", "downloading %s (%lld bytes)", m_UpdateRelease.m_aZipUrl, (long long)m_UpdateRelease.m_Size);

	m_pUpdateDownloadTask = HttpGetFile(m_UpdateRelease.m_aZipUrl, Storage(), TCLIENT_UPDATE_ZIP_PATH, IStorage::TYPE_SAVE);
	m_pUpdateDownloadTask->Timeout(CTimeout{20000, 0, 800, 60});
	m_pUpdateDownloadTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateDownloadTask->FailOnErrorStatus(true);
	if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource]))
		m_pUpdateDownloadTask->HeaderString("Accept", "application/octet-stream");
	m_pUpdateDownloadTask->ExpectSha256(Expected);
	// 注意不能开 ValidateBeforeOverwrite：开了之后文件停在 .tmp，必须再调
	// OnValidation(true) 才落盘，而这里没有回调链。ExpectSha256 已经保证
	// 哈希不符时 HTTP 层报 ERROR 且不会把 tmp 搬进位，安全性等价。
	m_pUpdateDownloadTask->ValidateBeforeOverwrite(false);
	m_pUpdateDownloadTask->AllowResume(true);
	Http()->Run(m_pUpdateDownloadTask);
}

void CTClient::FinishUpdateDownload()
{
	std::shared_ptr<IHttpRequest> pDownloadTask = m_pUpdateDownloadTask;
	if(!pDownloadTask || pDownloadTask->State() != EHttpState::DONE)
	{
		RetryUpdateDownload(false, TCLocalize("Download failed or the file is corrupted"));
		return;
	}
	// HTTP 层已校验哈希；确认最终落盘大小后才允许取 helper。
	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_ZIP_PATH, aPath, sizeof(aPath));
	IOHANDLE File = io_open(aPath, IOFLAG_READ);
	if(!File)
	{
		RetryUpdateDownload(false, TCLocalize("Download failed: the file could not be saved"));
		return;
	}
	const int64_t Length = io_length(File);
	io_close(File);
	if(Length != m_UpdateRelease.m_Size)
	{
		RetryUpdateDownload(false, TCLocalize("Download failed: the file size does not match the manifest"));
		return;
	}
	m_UpdateDownloadAttempt = 0;
	StartUpdateHelperTask();
}

void CTClient::StartUpdateHelperTask()
{
	ResetUpdateTasks();
	m_UpdateState = EUpdateState::DOWNLOADING;
	SetUpdateStatus(TCLocalize("Fetching the update helper..."));
	SHA256_DIGEST HelperDigest;
	if(sha256_from_str(&HelperDigest, m_UpdateRelease.m_aHelperSha256) != 0)
	{
		UpdateFail(TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource]) && !m_aUpdateApiHelperUrl[0])
	{
		StartApiResolveTask(true);
		return;
	}
	ResetUpdateTasks();
	m_UpdateState = EUpdateState::DOWNLOADING;
	if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource]))
		str_copy(m_aUpdateHelperUrl, m_aUpdateApiHelperUrl);
	else
		FormatUpdateAssetUrl(m_aUpdateHelperUrl, sizeof(m_aUpdateHelperUrl), TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource], m_UpdateRelease.m_aTag, TCLIENT_UPDATE_HELPER_ASSET);
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_BOOTSTRAP_PATH, m_aUpdateBootstrapPath, sizeof(m_aUpdateBootstrapPath));
	m_pUpdateHelperTask = HttpGetFile(m_aUpdateHelperUrl, Storage(), TCLIENT_UPDATE_BOOTSTRAP_PATH, IStorage::TYPE_SAVE);
	m_pUpdateHelperTask->Timeout(CTimeout{20000, 0, 800, 60});
	m_pUpdateHelperTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateHelperTask->FailOnErrorStatus(true);
	if(UpdateSourceIsApi(TCLIENT_UPDATE_SOURCE_SIBLING_BASES[m_UpdateDownloadSource]))
		m_pUpdateHelperTask->HeaderString("Accept", "application/octet-stream");
	m_pUpdateHelperTask->ExpectSha256(HelperDigest);
	m_pUpdateHelperTask->ValidateBeforeOverwrite(false);
	m_pUpdateHelperTask->AllowResume(true);
	Http()->Run(m_pUpdateHelperTask);
}

void CTClient::RetryUpdateDownload(bool Helper, const char *pReason)
{
	ResetUpdateTasks();
	if(m_UpdateDownloadAttempt + 1 >= TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT)
	{
		UpdateFail("%s", pReason);
		return;
	}
	++m_UpdateDownloadAttempt;
	m_UpdateDownloadSource = (m_UpdateDownloadSource + 1) % TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT;
	m_UpdateRetryHelper = Helper;
	m_UpdateRetryAt = Client()->LocalTime() + 2.0f;
	char aStatus[128];
	str_format(aStatus, sizeof(aStatus), TCLocalize("Download failed, retrying (%d/%d)..."), m_UpdateDownloadAttempt, TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT - 1);
	SetUpdateStatus(aStatus);
	log_warn("tclient-update", "%s; retrying %s on source %d", pReason, Helper ? "helper" : "ZIP", m_UpdateDownloadSource + 1);
}

void CTClient::FinishUpdateBootstrap()
{
	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_BOOTSTRAP_PATH, aPath, sizeof(aPath));
	IOHANDLE File = io_open(aPath, IOFLAG_READ);
	if(!File)
	{
		RetryUpdateDownload(true, TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	const int64_t Length = io_length(File);
	io_close(File);
	if(Length != m_UpdateRelease.m_HelperSize)
	{
		RetryUpdateDownload(true, TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	ResetUpdateTasks();
	BootstrapHelperPending = true;
	m_UpdateState = EUpdateState::DOWNLOADED;
	SetUpdateStatus(TCLocalize("Update downloaded, ready to install"));
	log_info("tclient-update", "download verified: %s (%lld bytes), bootstrap helper ready", aPath, (long long)Length);
}

bool CTClient::UpdateHelperExists() const
{
	return Storage()->FileExists(TCLIENT_UPDATE_HELPER, IStorage::TYPE_ALL) != 0;
}

void CTClient::UpdateInstallAndRestart()
{
	if(m_UpdateState != EUpdateState::DOWNLOADED)
		return;
#if !defined(CONF_FAMILY_WINDOWS)
	UpdateFail(TCLocalize("Installing updates is only supported on Windows"));
#else
	char aZip[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_ZIP_PATH, aZip, sizeof(aZip));
	// 自举优先：用随包下载并验过哈希的新 helper；不存在时退回安装目录里的
	char aHelper[IO_MAX_PATH_LENGTH];
	if(BootstrapHelperPending)
		str_copy(aHelper, m_aUpdateBootstrapPath);
	else
		Storage()->GetBinaryPathAbsolute(TCLIENT_UPDATE_HELPER, aHelper, sizeof(aHelper));
	if(!fs_is_file(aHelper))
	{
		char aMsg[128];
		str_format(aMsg, sizeof(aMsg), TCLocalize("Update helper %s is missing"), TCLIENT_UPDATE_HELPER);
		UpdateFail("%s", aMsg);
		return;
	}
	// 安装目录 = 客户端 exe 所在目录；helper 只替换包内文件，不碰配置/录像/自定义字体。
	char aExe[IO_MAX_PATH_LENGTH];
	Storage()->GetBinaryPathAbsolute(CLIENT_EXEC PLAT_EXT, aExe, sizeof(aExe));
	char aTarget[IO_MAX_PATH_LENGTH];
	str_copy(aTarget, aExe);
	char *pSlash = strrchr(aTarget, '/');
	char *pBackslash = strrchr(aTarget, '\\');
	char *pSep = (pBackslash > pSlash) ? pBackslash : pSlash;
	if(pSep)
		*pSep = '\0';
	else
		str_copy(aTarget, ".");

	char aLog[IO_MAX_PATH_LENGTH];
	str_format(aLog, sizeof(aLog), "%s/update/updater.log", aTarget);
	char aPid[32];
	str_format(aPid, sizeof(aPid), "%d", process_id());

	const char *apArguments[] = {
		"--pid", aPid,
		"--zip", aZip,
		"--sha256", m_UpdateRelease.m_aSha256,
		"--version", m_UpdateRelease.m_aVersion,
		"--target", aTarget,
		"--restart", aExe,
		"--log", aLog};
#if defined(CONF_FAMILY_WINDOWS)
	{
		wchar_t aWideTarget[IO_MAX_PATH_LENGTH];
		if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, aTarget, -1, aWideTarget, std::size(aWideTarget)) > 0)
		{
			StopOwnHelperProcesses(aWideTarget);
			Sleep(300);
		}
	}
#endif
	log_info("tclient-update", "starting helper: %s (pid %s, target %s)", aHelper, aPid, aTarget);
	const PROCESS Helper = process_execute(aHelper, EShellExecuteWindowState::BACKGROUND, apArguments, std::size(apArguments));
	if(Helper == INVALID_PROCESS)
	{
		UpdateFail(TCLocalize("Could not start the update helper"));
		return;
	}
	SetUpdateStatus(TCLocalize("Closing the client to install the update..."));
	m_UpdateState = EUpdateState::IDLE;
	Client()->Quit();
#endif
}

const char *CTClient::UpdateStateText() const
{
	switch(m_UpdateState)
	{
	case EUpdateState::IDLE: return TCLocalize("Not checked yet");
	case EUpdateState::CHECKING: return TCLocalize("Checking for updates...");
	case EUpdateState::UP_TO_DATE: return TCLocalize("You are on the latest version");
	case EUpdateState::AVAILABLE: return TCLocalize("An update is available");
	case EUpdateState::DOWNLOADING: return TCLocalize("Downloading update...");
	case EUpdateState::DOWNLOADED: return TCLocalize("Update downloaded, ready to install");
	case EUpdateState::FAILED: return TCLocalize("Update failed");
	}
	return "";
}

int CTClient::UpdateDownloadPercent() const
{
	if(!m_pUpdateDownloadTask)
		return 0;
	return std::clamp(m_pUpdateDownloadTask->Progress(), 0, 100);
}

void CTClient::PollUpdateTasks()
{
	// 主动取消立即终止整个链条，不触发镜像回退或延迟重试。
	if((m_pUpdateManifestTask && m_pUpdateManifestTask->State() == EHttpState::ABORTED) ||
		(m_pUpdateManifestSigTask && m_pUpdateManifestSigTask->State() == EHttpState::ABORTED) ||
		(m_pUpdateDownloadTask && m_pUpdateDownloadTask->State() == EHttpState::ABORTED) ||
		(m_pUpdateHelperTask && m_pUpdateHelperTask->State() == EHttpState::ABORTED) ||
		(m_pUpdateApiResolveTask && m_pUpdateApiResolveTask->State() == EHttpState::ABORTED))
	{
		UpdateFail(TCLocalize("Update cancelled"));
		return;
	}
	if(m_pUpdateManifestTask && m_pUpdateManifestTask->Done() && !m_pUpdateManifestSigTask)
	{
		if(m_pUpdateManifestTask->State() != EHttpState::DONE)
		{
			NextUpdateManifestSource(TCLocalize("Update check failed: the manifest could not be downloaded"));
			return;
		}
		// 签名必须从本次清单的同一入口下载，不保留跨源数据。
		m_pUpdateManifestSigTask = HttpGet(m_aUpdateManifestSigUrl);
		m_pUpdateManifestSigTask->Timeout(CTimeout{15000, 0, 800, 10});
		m_pUpdateManifestSigTask->IpResolve(IPRESOLVE::V4);
		m_pUpdateManifestSigTask->MaxResponseSize(1024);
		m_pUpdateManifestSigTask->FailOnErrorStatus(true);
		Http()->Run(m_pUpdateManifestSigTask);
		return;
	}
	if(m_pUpdateManifestSigTask && m_pUpdateManifestSigTask->Done())
	{
		std::shared_ptr<IHttpRequest> pSigTask = m_pUpdateManifestSigTask;
		std::shared_ptr<IHttpRequest> pManifestTask = m_pUpdateManifestTask;
		m_pUpdateManifestSigTask = nullptr;
		m_pUpdateManifestTask = nullptr;
		if(pSigTask->State() != EHttpState::DONE)
			NextUpdateManifestSource(TCLocalize("Update check failed: the manifest signature could not be downloaded"));
		else
			FinishUpdateManifest(pSigTask, pManifestTask);
		return;
	}
	if(m_pUpdateApiResolveTask && m_pUpdateApiResolveTask->Done())
	{
		FinishApiResolve();
		return;
	}
	if(m_pUpdateHelperTask && m_pUpdateHelperTask->Done())
	{
		if(m_pUpdateHelperTask->State() == EHttpState::DONE)
			FinishUpdateBootstrap();
		else
			RetryUpdateDownload(true, TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	if(m_pUpdateDownloadTask && m_pUpdateDownloadTask->Done())
	{
		// Finish 会读取当前任务；不得在调用之前把成员置空。
		if(m_pUpdateDownloadTask->State() == EHttpState::DONE)
			FinishUpdateDownload();
		else
			RetryUpdateDownload(false, TCLocalize("Download failed or the file is corrupted"));
		return;
	}
	if(m_UpdateRetryAt >= 0 && Client()->LocalTime() >= m_UpdateRetryAt)
	{
		const bool Helper = m_UpdateRetryHelper;
		m_UpdateRetryAt = -1.0f;
		m_UpdateRetryHelper = false;
		if(m_UpdateState == EUpdateState::DOWNLOADING && !m_pUpdateDownloadTask && !m_pUpdateHelperTask)
		{
			if(Helper)
				StartUpdateHelperTask();
			else
				StartDownloadTask();
		}
	}
}

void CTClient::ConUpdateCheck(IConsole::IResult *pResult, void *pUserData)
{
	((CTClient *)pUserData)->UpdateCheck(true);
}

void CTClient::OnShutdown()
{
	ResetUpdateTasks();
#if defined(CONF_FAMILY_WINDOWS)
	char aInstallDir[IO_MAX_PATH_LENGTH];
	Storage()->GetBinaryPathAbsolute(CLIENT_EXEC PLAT_EXT, aInstallDir, sizeof(aInstallDir));
	if(fs_parent_dir(aInstallDir) == 0)
	{
		wchar_t aWideInstallDir[IO_MAX_PATH_LENGTH];
		if(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, aInstallDir, -1, aWideInstallDir, std::size(aWideInstallDir)) > 0)
			StopOwnHelperProcesses(aWideInstallDir, true);
	}
#endif
}

void CTClient::QueueForcedAspectUpdate()
{
	m_ForcedAspectQueued = true;
}

void CTClient::ApplyForcedAspect()
{
	// TODO: portrait 视口切换本身仍会闪（5:4 强制是设计行为）；
	// 本修复消除的是“快照分发内同步 flush + 宽屏无谓重建”。
	int State = Client()->State();
	bool Force = true;
	if(g_Config.m_TcAllowAnyRes == 0)
		;
	else if(State == CClient::EClientState::STATE_DEMOPLAYBACK)
		Force = false;
	else if(State == CClient::EClientState::STATE_ONLINE && GameClient()->m_GameInfo.m_AllowZoom && !GameClient()->m_Menus.IsActive())
		Force = false;
	Graphics()->SetForcedAspect(Force);
}

void CTClient::OnUpdate()
{
	// 在常规更新阶段统一应用排队的视口策略，绝不从快照分发内联触发。
	if(m_ForcedAspectQueued)
	{
		m_ForcedAspectQueued = false;
		ApplyForcedAspect();
	}
}

void CTClient::OnStateChange(int OldState, int NewState)
{
	QueueForcedAspectUpdate();
	for(auto &AirRescuePositions : m_aAirRescuePositions)
		AirRescuePositions = {};
}

void CTClient::OnNewSnapshot()
{
	QueueForcedAspectUpdate();
	// Update volleyball
	bool IsVolleyBall = false;
	if(g_Config.m_TcVolleyBallBetterBall > 0 && g_Config.m_TcVolleyBallBetterBallSkin[0] != '\0')
	{
		if(g_Config.m_TcVolleyBallBetterBall > 1)
			IsVolleyBall = true;
		else
			IsVolleyBall = str_startswith_nocase(GameClient()->Map()->BaseName(), "volleyball");
	};
	for(auto &Client : GameClient()->m_aClients)
	{
		Client.m_IsVolleyBall = IsVolleyBall && Client.m_DeepFrozen;
	}
	// Update air rescue
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
		{
			const int ClientId = GameClient()->m_aLocalIds[Dummy];
			if(ClientId == -1)
				continue;
			const auto &Char = GameClient()->m_Snap.m_aCharacters[ClientId];
			if(!Char.m_Active)
				continue;
			if(Client()->GameTick(Dummy) % 10 != 0) // Works for both 25tps and 50tps
				continue;
			const auto &Client = GameClient()->m_aClients[ClientId];
			if(Client.m_FreezeEnd == -1) // You aren't safe when frozen
				continue;
			const vec2 NewPos = vec2(Char.m_Cur.m_X, Char.m_Cur.m_Y);
			// If new pos is under 2 tiles from old pos, don't record a new position
			if(!m_aAirRescuePositions[Dummy].empty())
			{
				const vec2 OldPos = m_aAirRescuePositions[Dummy].front();
				if(distance(NewPos, OldPos) < 64.0f)
					continue;
			}
			if(m_aAirRescuePositions[Dummy].size() >= 256)
				m_aAirRescuePositions[Dummy].pop_back();
			m_aAirRescuePositions[Dummy].push_front(NewPos);
		}
	}
}

constexpr const char STRIP_CHARS[] = {'-', '=', '+', '_', ' '};
static bool IsStripChar(char c)
{
	return std::any_of(std::begin(STRIP_CHARS), std::end(STRIP_CHARS), [c](char s) {
		return s == c;
	});
}

static void StripStr(const char *pIn, char *pOut, const char *pEnd)
{
	if(!pIn)
	{
		*pOut = '\0';
		return;
	}

	while(*pIn && IsStripChar(*pIn))
		pIn++;

	// Special behaviour for empty checkbox
	if((unsigned char)*pIn == 0xE2 && (unsigned char)(*(pIn + 1)) == 0x98 && (unsigned char)(*(pIn + 2)) == 0x90)
	{
		pIn += 3;
		while(*pIn && IsStripChar(*pIn))
			pIn++;
	}

	char *pLastValid = nullptr;
	while(*pIn && pOut < pEnd - 1)
	{
		*pOut = *pIn;
		if(!IsStripChar(*pIn))
			pLastValid = pOut;
		pIn++;
		pOut++;
	}

	if(pLastValid)
		*(pLastValid + 1) = '\0';
	else
		*pOut = '\0';
}

void CTClient::RenderMiniVoteHud()
{
	CUIRect View = {0.0f, 60.0f, 70.0f, 47.0f};
	View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_R, 3.0f);
	View.Margin(3.0f, &View);

	SLabelProperties Props;
	Props.m_EllipsisAtEnd = true;
	Props.m_MaxWidth = View.w;

	CUIRect Row, LeftColumn, RightColumn, ProgressSpinner;
	char aBuf[256];

	// Vote description
	View.HSplitTop(6.0f, &Row, &View);
	StripStr(GameClient()->m_Voting.VoteDescription(), aBuf, aBuf + sizeof(aBuf));
	Ui()->DoLabel(&Row, aBuf, 6.0f, TEXTALIGN_ML, Props);

	// Vote reason
	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(4.0f, &Row, &View);
	Ui()->DoLabel(&Row, GameClient()->m_Voting.VoteReason(), 4.0f, TEXTALIGN_ML, Props);

	// Time left
	str_format(aBuf, sizeof(aBuf), Localize("%ds left"), GameClient()->m_Voting.SecondsLeft());
	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(3.0f, &Row, &View);
	Row.VSplitLeft(2.0f, nullptr, &Row);
	Row.VSplitLeft(3.0f, &ProgressSpinner, &Row);
	Row.VSplitLeft(2.0f, nullptr, &Row);

	SProgressSpinnerProperties ProgressProps;
	ProgressProps.m_Progress = std::clamp((time() - GameClient()->m_Voting.m_Opentime) / (float)(GameClient()->m_Voting.m_Closetime - GameClient()->m_Voting.m_Opentime), 0.0f, 1.0f);
	Ui()->RenderProgressSpinner(ProgressSpinner.Center(), ProgressSpinner.h / 2.0f, ProgressProps);

	Ui()->DoLabel(&Row, aBuf, 3.0f, TEXTALIGN_ML);

	// Bars
	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(3.0f, &Row, &View);
	GameClient()->m_Voting.RenderBars(Row);

	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(9.0f, &Row, &View);
	GameClient()->m_Voting.RenderVoteCounts(Row, 4.0f);

	// F3 / F4
	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(0.5f, &Row, &View);
	Row.VSplitMid(&LeftColumn, &RightColumn, 4.0f);

	char aKey[64];
	GameClient()->m_Binds.GetKey("vote yes", aKey, sizeof(aKey));
	TextRender()->TextColor(GameClient()->m_Voting.TakenChoice() == 1 ? ColorRGBA(0.2f, 0.9f, 0.2f, 0.85f) : TextRender()->DefaultTextColor());
	Ui()->DoLabel(&LeftColumn, aKey[0] == '\0' ? TCLocalize("yes") : aKey, 0.5f, TEXTALIGN_ML);

	GameClient()->m_Binds.GetKey("vote no", aKey, sizeof(aKey));
	TextRender()->TextColor(GameClient()->m_Voting.TakenChoice() == -1 ? ColorRGBA(0.95f, 0.25f, 0.25f, 0.85f) : TextRender()->DefaultTextColor());
	Ui()->DoLabel(&RightColumn, aKey[0] == '\0' ? TCLocalize("no") : aKey, 0.5f, TEXTALIGN_MR);

	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CTClient::RenderCenterLines()
{
	if(g_Config.m_TcShowCenter <= 0)
		return;

	if(GameClient()->m_Scoreboard.IsActive())
		return;

	Graphics()->TextureClear();

	const CScreenRect ScreenRect = Graphics()->GetScreen();
	const auto ScreenMid = (ScreenRect.m_BottomRight + ScreenRect.m_TopLeft) / 2.0f;

	if(g_Config.m_TcShowCenterWidth == 0)
	{
		Graphics()->LinesBegin();
		IGraphics::CLineItem aLines[2] = {
			{ScreenMid.x, ScreenRect.m_TopLeft.y, ScreenMid.x, ScreenRect.m_BottomRight.y},
			{ScreenRect.m_TopLeft.x, ScreenMid.y, ScreenRect.m_BottomRight.x, ScreenMid.y}};
		Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcShowCenterColor, true)));
		Graphics()->LinesDraw(aLines, std::size(aLines));
		Graphics()->LinesEnd();
	}
	else
	{
		const float W = g_Config.m_TcShowCenterWidth;
		const float X0 = ScreenRect.m_TopLeft.x;
		const float Y0 = ScreenRect.m_TopLeft.y;
		const float X1 = ScreenRect.m_BottomRight.x;
		const float Y1 = ScreenRect.m_BottomRight.y;
		Graphics()->QuadsBegin();
		IGraphics::CQuadItem aQuads[3] = {
			{ScreenMid.x, mix(Y0, Y1, 0.25f) - W / 4.0f, W, (Y1 - Y0 - W) / 2.0f},
			{ScreenMid.x, mix(Y0, Y1, 0.75f) + W / 4.0f, W, (Y1 - Y0 - W) / 2.0f},
			{ScreenMid.x, ScreenMid.y, X1 - X0, W}};
		Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_TcShowCenterColor, true)));
		Graphics()->QuadsDraw(aQuads, std::size(aQuads));
		Graphics()->QuadsEnd();
	}
}

void CTClient::RenderCtfFlag(vec2 Pos, float Alpha)
{
	// from CItems::RenderFlag
	float Size = 42.0f;
	int QuadOffset;
	if(g_Config.m_TcFakeCtfFlags == 1)
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagRed);
		QuadOffset = GameClient()->m_Items.m_RedFlagOffset;
	}
	else
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagBlue);
		QuadOffset = GameClient()->m_Items.m_BlueFlagOffset;
	}
	Graphics()->QuadsSetRotation(0.0f);
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
	Graphics()->RenderQuadContainerAsSprite(GameClient()->m_Items.m_ItemsQuadContainerIndex, QuadOffset, Pos.x, Pos.y - Size * 0.75f);
}
