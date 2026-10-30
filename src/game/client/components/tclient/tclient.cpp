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
static bool ParseAppVersion(const char *pVersion, int aOut[3])
{
	aOut[0] = aOut[1] = aOut[2] = 0;
	if(!pVersion || !pVersion[0])
		return false;
	const char *p = pVersion;
	for(int i = 0; i < 3; i++)
	{
		const char *pStart = p;
		while(*p >= '0' && *p <= '9')
			p++;
		if(p == pStart)
			return false;
		if(p - pStart > 6)
			return false;
		int Value = 0;
		for(const char *q = pStart; q < p; q++)
			Value = Value * 10 + (*q - '0');
		aOut[i] = Value;
		if(i < 2)
		{
			if(*p != '.')
				return false;
			p++;
		}
	}
	return *p == '\0';
}

// >0 表示 A 比 B 新
static int CompareAppVersion(const char *pA, const char *pB)
{
	int a[3], b[3];
	if(!ParseAppVersion(pA, a) || !ParseAppVersion(pB, b))
		return 0;
	for(int i = 0; i < 3; i++)
	{
		if(a[i] != b[i])
			return a[i] > b[i] ? 1 : -1;
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

		if(ClientId < 0 || ClientId > MAX_CLIENTS)
			return;
		int LocalId = GameClient()->m_Snap.m_LocalClientId;
		if(ClientId == LocalId)
			str_copy(m_PreviousOwnMessage, pMsg->m_pMessage);

		bool PingMessage = false;

		bool ValidIds = !(GameClient()->m_aLocalIds[0] < 0 || (GameClient()->Client()->DummyConnected() && GameClient()->m_aLocalIds[1] < 0));

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

void CTClient::SpecId(int ClientId)
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(Client()->State() == IClient::STATE_DEMOPLAYBACK || GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		GameClient()->m_Spectator.Spectate(ClientId);
		return;
	}

	if(ClientId < 0 || ClientId > (int)std::size(GameClient()->m_aClients))
		return;
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
		GameClient()->Echo("You are not in practice");
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

	GameClient()->Echo("No safe position found");
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
			((CTClient *)pUserData)->SetForcedAspect();
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
	if(!NearTile(Player.m_RenderPos, 10, TILE_FINISH))
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
	m_UpdateRelease.m_Valid = false;
	// 失败即中止所有在途任务，避免 PollUpdateTasks 下一帧重复触发
	ResetUpdateTasks();
	log_warn("tclient-update", "%s", m_aUpdateStatus);
}

void CTClient::ResetUpdateTasks()
{
	if(m_pUpdateManifestTask)
	{
		m_pUpdateManifestTask->Abort();
		m_pUpdateManifestTask = nullptr;
	}
	if(m_pUpdateDownloadTask)
	{
		m_pUpdateDownloadTask->Abort();
		m_pUpdateDownloadTask = nullptr;
	}
}

// 更新清单 Ed25519 验签：公钥内嵌（update_public_key.h），签名由发布脚本
// 用私钥生成。验签不过 = 清单被篡改或来源不对，直接拒绝，不进入下载。
bool CTClient::VerifyUpdateManifestSignature(const unsigned char *pManifest, size_t ManifestSize, const unsigned char *pSignature, size_t SignatureSize)
{
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
	m_LastUpdateCheck = time_timestamp();
	m_UpdateState = EUpdateState::CHECKING;
	SetUpdateStatus(TCLocalize("Checking for updates..."));
	log_info("tclient-update", "checking for update (force=%d, current=%s)", Force ? 1 : 0, TCLIENT_APP_VERSION);
	FetchUpdateManifest();
}



void CTClient::FetchUpdateManifest()
{
	char aUrl[512];
	str_format(aUrl, (int)sizeof(aUrl), "%s%s", TCLIENT_UPDATE_LATEST_URL, TCLIENT_UPDATE_MANIFEST_ASSET);
	m_pUpdateManifestTask = HttpGet(aUrl);
	m_pUpdateManifestTask->Timeout(CTimeout{15000, 0, 800, 10});
	m_pUpdateManifestTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateManifestTask->MaxResponseSize(64 * 1024);
	m_pUpdateManifestTask->FailOnErrorStatus(true);
	Http()->Run(m_pUpdateManifestTask);
}

void CTClient::FinishUpdateManifest()
{
	// 验签优先于任何解析：清单字节 + Ed25519 签名（公钥内嵌），签名不符直接拒绝
	{
		unsigned char *pManifest;
		size_t ManifestSize;
		m_pUpdateManifestTask->Result(&pManifest, &ManifestSize);
		unsigned char *pSig;
		size_t SigSize;
		m_pUpdateManifestSigTask->Result(&pSig, &SigSize);
		if(!VerifyUpdateManifestSignature(pManifest, ManifestSize, pSig, SigSize))
		{
			UpdateFail(TCLocalize("Update rejected: manifest signature is invalid"));
			return;
		}
		log_info("tclient-update", "manifest signature verified");
	}

	json_value *pJson = m_pUpdateManifestTask->ResultJson();
	if(!pJson || pJson->type != json_object)
	{
		if(pJson)
			json_value_free(pJson);
		UpdateFail(TCLocalize("Update check failed: the manifest is not valid JSON"));
		return;
	}

	// 清单必须证明自己是"这个产品、这个渠道、这个平台、这个版本"的包，
	// 否则一律不下载——防止拿到别的项目或别的平台的资产。
	const char *pProduct = JsonStr(JsonObj(*pJson, "product"));
	const char *pChannel = JsonStr(JsonObj(*pJson, "channel"));
	const char *pVersion = JsonStr(JsonObj(*pJson, "version"));
	const char *pPlatform = JsonStr(JsonObj(*pJson, "platform"));
	const char *pAsset = JsonStr(JsonObj(*pJson, "asset"));
	const char *pSha256 = JsonStr(JsonObj(*pJson, "sha256"));
	const json_value &SizeValue = JsonObj(*pJson, "size");
	const int64_t Size = JsonGetInt(SizeValue, -1);

	const char *pError = nullptr;
	if(!pProduct || str_comp(pProduct, TCLIENT_UPDATE_PRODUCT) != 0)
		pError = TCLocalize("Update rejected: wrong product");
	else if(!pChannel || str_comp(pChannel, TCLIENT_UPDATE_CHANNEL) != 0)
		pError = TCLocalize("Update rejected: wrong channel");
	else if(!pPlatform || str_comp(pPlatform, TCLIENT_UPDATE_PLATFORM) != 0)
		pError = TCLocalize("Update rejected: wrong platform");
	else if(!pVersion)
		pError = TCLocalize("Update rejected: version does not match the release");
	else if(!pAsset || str_comp(pAsset, TCLIENT_UPDATE_ZIP_ASSET) != 0)
		pError = TCLocalize("Update rejected: unexpected asset name");
	else if(!IsHexSha256(pSha256))
		pError = TCLocalize("Update rejected: missing or malformed SHA-256");
	else if(Size <= 0)
		pError = TCLocalize("Update rejected: missing file size");
	else if(CompareAppVersion(pVersion, TCLIENT_APP_VERSION) <= 0)
		pError = TCLocalize("Update rejected: not newer than the current version");

	if(pError)
	{
		json_value_free(pJson);
		UpdateFail("%s", pError);
		return;
	}

	str_copy(m_UpdateRelease.m_aAsset, pAsset);
	str_copy(m_UpdateRelease.m_aSha256, pSha256);
	m_UpdateRelease.m_Size = Size;
	// 自举：清单同时携带新 helper 的哈希，客户端下载后验哈希再用它安装
	const json_value &Helper = JsonObj(*pJson, "helper");
	const char *pHelperSha = JsonStr(JsonObj(Helper, "sha256"));
	const json_value &HelperSize = JsonObj(Helper, "size");
	if(!pHelperSha || !IsHexSha256(pHelperSha) || JsonGetInt(HelperSize, -1) <= 0)
	{
		json_value_free(pJson);
		UpdateFail(TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	str_copy(m_UpdateRelease.m_aHelperSha256, pHelperSha);
	m_UpdateRelease.m_HelperSize = JsonGetInt(HelperSize, -1);
	str_format(m_UpdateRelease.m_aTag, sizeof(m_UpdateRelease.m_aTag), "v%s", pVersion);
	str_copy(m_UpdateRelease.m_aVersion, pVersion);
	str_format(m_UpdateRelease.m_aZipUrl, sizeof(m_UpdateRelease.m_aZipUrl), "%s%s", TCLIENT_UPDATE_LATEST_URL, TCLIENT_UPDATE_ZIP_ASSET);
	str_format(m_aUpdateHelperUrl, sizeof(m_aUpdateHelperUrl), "%s%s", TCLIENT_UPDATE_LATEST_URL, TCLIENT_UPDATE_HELPER_ASSET);
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
static void StopOwnHelperProcesses(const wchar_t *pInstallDir)
{
	HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if(Snapshot == INVALID_HANDLE_VALUE)
		return;
	std::wstring LowerInstall = pInstallDir;
	for(wchar_t &c : LowerInstall)
		c = towlower(c);

	PROCESSENTRY32W Entry;
	Entry.dwSize = sizeof(Entry);
	if(Process32FirstW(Snapshot, &Entry))
	{
		do
		{
			if(_wcsicmp(Entry.szExeFile, L"island_media_monitor.exe") != 0 &&
				_wcsicmp(Entry.szExeFile, L"netease-watcher.exe") != 0)
				continue;
			HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, Entry.th32ProcessID);
			if(!Process)
				continue;
			wchar_t aPath[MAX_PATH + 8];
			DWORD PathSize = std::size(aPath);
			if(QueryFullProcessImageNameW(Process, 0, aPath, &PathSize))
			{
				std::wstring LowerPath = aPath;
				for(wchar_t &c : LowerPath)
					c = towlower(c);
				if(LowerPath.rfind(LowerInstall, 0) == 0)
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
	ResetUpdateTasks();
	m_UpdateState = EUpdateState::DOWNLOADING;
	SetUpdateStatus(TCLocalize("Downloading update..."));
	log_info("tclient-update", "downloading %s (%lld bytes)", m_UpdateRelease.m_aZipUrl, (long long)m_UpdateRelease.m_Size);

	m_pUpdateDownloadTask = HttpGetFile(m_UpdateRelease.m_aZipUrl, Storage(), TCLIENT_UPDATE_ZIP_PATH, IStorage::TYPE_SAVE);
	m_pUpdateDownloadTask->Timeout(CTimeout{20000, 0, 800, 60});
	m_pUpdateDownloadTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateDownloadTask->FailOnErrorStatus(true);
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
	if(m_pUpdateDownloadTask->State() != EHttpState::DONE)
	{
		// 哈希不符或传输中断：临时文件由 HTTP 层删掉，这里只报告。
		UpdateFail(TCLocalize("Download failed or the file is corrupted"));
		return;
	}
	// 校验通过后 HTTP 层才会把临时文件搬到 update/ 下，这里确认一次大小。
	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_ZIP_PATH, aPath, sizeof(aPath));
	IOHANDLE File = io_open(aPath, IOFLAG_READ);
	if(!File)
	{
		UpdateFail(TCLocalize("Download failed: the file could not be saved"));
		return;
	}
	const int64_t Length = io_length(File);
	io_close(File);
	if(m_UpdateRelease.m_Size > 0 && Length != m_UpdateRelease.m_Size)
	{
		UpdateFail(TCLocalize("Download failed: the file size does not match the manifest"));
		return;
	}
	// 自举：从服务器取新版 helper（清单内含其哈希），装包由它执行 ——
	// 运行的永远是验签链上的新 helper，而不是安装目录里的旧版
	ResetUpdateTasks();
	m_UpdateState = EUpdateState::DOWNLOADING;
	SetUpdateStatus(TCLocalize("Fetching the update helper..."));
	SHA256_DIGEST HelperDigest;
	if(sha256_from_str(&HelperDigest, m_UpdateRelease.m_aHelperSha256) != 0)
	{
		UpdateFail(TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_BOOTSTRAP_PATH, m_aUpdateBootstrapPath, sizeof(m_aUpdateBootstrapPath));
	m_pUpdateHelperTask = HttpGetFile(m_aUpdateHelperUrl, Storage(), TCLIENT_UPDATE_BOOTSTRAP_PATH, IStorage::TYPE_SAVE);
	m_pUpdateHelperTask->Timeout(CTimeout{20000, 0, 800, 60});
	m_pUpdateHelperTask->IpResolve(IPRESOLVE::V4);
	m_pUpdateHelperTask->FailOnErrorStatus(true);
	m_pUpdateHelperTask->ExpectSha256(HelperDigest);
	m_pUpdateHelperTask->AllowResume(true);
	Http()->Run(m_pUpdateHelperTask);
}

void CTClient::FinishUpdateBootstrap()
{
	char aPath[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TCLIENT_UPDATE_BOOTSTRAP_PATH, aPath, sizeof(aPath));
	IOHANDLE File = io_open(aPath, IOFLAG_READ);
	if(!File)
	{
		UpdateFail(TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	const int64_t Length = io_length(File);
	io_close(File);
	if(m_UpdateRelease.m_HelperSize > 0 && Length != m_UpdateRelease.m_HelperSize)
	{
		UpdateFail(TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
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
		"--target", aTarget,
		"--restart", aExe,
		"--log", aLog};
#if defined(CONF_FAMILY_WINDOWS)
	{
		wchar_t aWideTarget[IO_MAX_PATH_LENGTH];
		MultiByteToWideChar(CP_UTF8, 0, aTarget, -1, aWideTarget, std::size(aWideTarget));
		StopOwnHelperProcesses(aWideTarget);
		Sleep(300);
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
	if(m_pUpdateManifestTask && m_pUpdateManifestTask->Done())
	{
		// 清单下载完成后先拉签名，验签通过才解析
		if(m_pUpdateManifestTask->State() != EHttpState::DONE)
		{
			UpdateFail(TCLocalize("Update check failed: the manifest could not be downloaded"));
			ResetUpdateTasks();
			return;
		}
		if(!m_pUpdateManifestSigTask)
		{
			m_pUpdateManifestSigTask = HttpGet(m_aUpdateManifestSigUrl);
			m_pUpdateManifestSigTask->Timeout(CTimeout{15000, 0, 800, 10});
			m_pUpdateManifestSigTask->MaxResponseSize(1024);
			m_pUpdateManifestSigTask->FailOnErrorStatus(true);
			Http()->Run(m_pUpdateManifestSigTask);
			return;
		}
	}
	if(m_pUpdateHelperTask && m_pUpdateHelperTask->Done())
	{
		const bool HelperOk = m_pUpdateHelperTask->State() == EHttpState::DONE;
		m_pUpdateHelperTask = nullptr;
		if(HelperOk)
		{
			FinishUpdateBootstrap();
			return;
		}
		// helper 很小，失败直接重试一次链条（zip 已在本地，不会重新下载）
		if(m_UpdateDownloadAttempt < 3)
		{
			++m_UpdateDownloadAttempt;
			char aStatus[128];
			str_format(aStatus, sizeof(aStatus), TCLocalize("Download failed, retrying (%d/%d)..."), m_UpdateDownloadAttempt, 3);
			SetUpdateStatus(aStatus);
			m_UpdateRetryAt = Client()->LocalTime() + 2.0f;
			return;
		}
		UpdateFail(TCLocalize("Update rejected: missing bootstrap helper"));
		return;
	}
	if(m_UpdateRetryAt > 0 && Client()->LocalTime() >= m_UpdateRetryAt && m_UpdateState == EUpdateState::DOWNLOADING && !m_pUpdateDownloadTask && !m_pUpdateHelperTask)
	{
		// zip 已在本地时 ShouldSkipRequest 会秒过，重试代价极小
		StartDownloadTask();
		return;
	}
	if(m_pUpdateManifestSigTask && m_pUpdateManifestSigTask->Done())
	{
		const bool SigOk = m_pUpdateManifestSigTask->State() == EHttpState::DONE;
		m_pUpdateManifestSigTask = nullptr;
		if(!SigOk)
		{
			UpdateFail(TCLocalize("Update check failed: the manifest signature could not be downloaded"));
			return;
		}
		FinishUpdateManifest();
		return;
	}
	if(m_pUpdateDownloadTask && m_pUpdateDownloadTask->Done())
	{
		// 注意顺序：FinishUpdateDownload 会读取任务状态，必须先调用再置空，
		// 否则下载失败（这条线路很常见）时这里就是必然的空指针闪退。
		const bool Ok = m_pUpdateDownloadTask->State() == EHttpState::DONE;
		if(Ok)
			FinishUpdateDownload();
		m_pUpdateDownloadTask = nullptr;
		if(Ok)
			return;
		// 下载失败自动重试：这条 GitHub 大文件线路不稳定，中断是常态。
		// 断点续传由 HTTP 层负责（AllowResume），这里只管发起下一轮。
		if(m_UpdateDownloadAttempt < 3)
		{
			++m_UpdateDownloadAttempt;
			char aStatus[128];
			str_format(aStatus, sizeof(aStatus), TCLocalize("Download failed, retrying (%d/%d)..."), m_UpdateDownloadAttempt, 3);
			SetUpdateStatus(aStatus);
			log_warn("tclient-update", "download attempt %d failed, retrying", m_UpdateDownloadAttempt);
			m_UpdateRetryAt = Client()->LocalTime() + 2.0f;
			return;
		}
		FinishUpdateDownload();
		return;
	}
	if(m_UpdateRetryAt > 0 && Client()->LocalTime() >= m_UpdateRetryAt)
	{
		m_UpdateRetryAt = -1.0f;
		StartDownloadTask();
	}
}

void CTClient::ConUpdateCheck(IConsole::IResult *pResult, void *pUserData)
{
	((CTClient *)pUserData)->UpdateCheck(true);
}

void CTClient::OnShutdown()
{
	ResetUpdateTasks();
}

void CTClient::SetForcedAspect()
{
	// TODO: Fix flashing on windows
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

void CTClient::OnStateChange(int OldState, int NewState)
{
	SetForcedAspect();
	for(auto &AirRescuePositions : m_aAirRescuePositions)
		AirRescuePositions = {};
}

void CTClient::OnNewSnapshot()
{
	SetForcedAspect();
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
	CUIRect View = {0.0f, 60.0f, 70.0f, 35.0f};
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

	// F3 / F4
	View.HSplitTop(3.0f, nullptr, &View);
	View.HSplitTop(0.5f, &Row, &View);
	Row.VSplitMid(&LeftColumn, &RightColumn, 4.0f);

	char aKey[64];
	GameClient()->m_Binds.GetKey("vote yes", aKey, sizeof(aKey));
	TextRender()->TextColor(GameClient()->m_Voting.TakenChoice() == 1 ? ColorRGBA(0.2f, 0.9f, 0.2f, 0.85f) : TextRender()->DefaultTextColor());
	Ui()->DoLabel(&LeftColumn, aKey[0] == '\0' ? "yes" : aKey, 0.5f, TEXTALIGN_ML);

	GameClient()->m_Binds.GetKey("vote no", aKey, sizeof(aKey));
	TextRender()->TextColor(GameClient()->m_Voting.TakenChoice() == -1 ? ColorRGBA(0.95f, 0.25f, 0.25f, 0.85f) : TextRender()->DefaultTextColor());
	Ui()->DoLabel(&RightColumn, aKey[0] == '\0' ? "no" : aKey, 0.5f, TEXTALIGN_MR);

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
