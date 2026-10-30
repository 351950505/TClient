#include "player_display_privacy.h"
#include "streamer_word_filter.h"

#include <base/str.h>

#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/client/gameclient.h>

#include <cstdlib>
#include <string>
#include <vector>

bool CGameClient::StreamerModeActive() const
{
	return g_Config.m_MeStreamerMode != 0;
}

static bool IsStreamerLocal(const CGameClient &GameClient, int ClientId)
{
	return ClientId == GameClient.m_aLocalIds[0] || ClientId == GameClient.m_aLocalIds[1] || ClientId == GameClient.m_Snap.m_LocalClientId;
}

static QmPlayerDisplayPrivacy::SDecisions ResolveStreamerPrivacy(const CGameClient &GameClient, int ClientId)
{
	if(!GameClient.StreamerModeActive() || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return {};
	QmPlayerDisplayPrivacy::SSettings Settings{g_Config.m_MeStreamerHideNames != 0, g_Config.m_MeStreamerHideSkins != 0, false};
	QmPlayerDisplayPrivacy::SPlayer Player;
	Player.m_ClientId = ClientId;
	Player.m_Local = IsStreamerLocal(GameClient, ClientId);
	Player.m_Friend = GameClient.m_aClients[ClientId].m_Friend;
	return QmPlayerDisplayPrivacy::Resolve(Settings, Player);
}

bool CGameClient::ShouldHideStreamerIdentity(int ClientId) const
{
	return ResolveStreamerPrivacy(*this, ClientId).m_HideIdentity;
}

bool CGameClient::ShouldHideStreamerSkin(int ClientId) const
{
	return ResolveStreamerPrivacy(*this, ClientId).m_HideSkin;
}

bool CGameClient::StreamerDefaultFlags() const
{
	// 国旗是身份信息的一部分：主播模式一开就必须整块不画。
	// 旧实现挂在 me_streamer_default_flags 子开关上且默认关闭，于是"开了主播模式
	// 国旗还露"；即使勾上也只画一张灰色默认旗，反而更显眼。现在直接跟随主开关。
	return StreamerModeActive();
}

bool CGameClient::StreamerFilterText(char *pText) const
{
	return QmStreamerWordFilter::Filter(pText, StreamerModeActive() && g_Config.m_MeStreamerFilterWords != 0);
}

// 服务器浏览器用：没有 client id，只能按名字判定好友；不是好友就遮掉。
// 这里不做「显示成编号」——浏览器里没有编号可言，用 * 占位。
void CGameClient::StreamerNameForServerBrowser(const char *pName, char *pBuf, int BufSize)
{
	if(!pBuf || BufSize <= 0)
		return;
	pBuf[0] = '\0';
	if(!pName)
		return;
	if(!StreamerModeActive() || g_Config.m_MeStreamerHideNames == 0)
	{
		str_copy(pBuf, pName, BufSize);
		return;
	}
	if(Friends() != nullptr && Friends()->IsFriend(pName, "", true))
	{
		str_copy(pBuf, pName, BufSize);
		StreamerFilterText(pBuf);
		return;
	}
	str_copy(pBuf, "***", BufSize);
}

void CGameClient::StreamerClanForServerBrowser(const char *pClan, char *pBuf, int BufSize)
{
	if(!pBuf || BufSize <= 0)
		return;
	pBuf[0] = '\0';
	if(!pClan)
		return;
	if(!StreamerModeActive() || g_Config.m_MeStreamerHideNames == 0)
	{
		str_copy(pBuf, pClan, BufSize);
		return;
	}
	if(Friends() != nullptr && Friends()->IsFriend("", pClan, true))
	{
		str_copy(pBuf, pClan, BufSize);
		StreamerFilterText(pBuf);
	}
}

void CGameClient::StreamerName(int ClientId, char *pBuf, int BufSize) const
{
	if(!pBuf || BufSize <= 0)
		return;
	pBuf[0] = '\0';
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	if(!StreamerModeActive())
	{
		str_copy(pBuf, m_aClients[ClientId].m_aName, BufSize);
		return;
	}
	if(ShouldHideStreamerIdentity(ClientId))
		str_format(pBuf, BufSize, "%d", ClientId);
	else
		str_copy(pBuf, m_aClients[ClientId].m_aName, BufSize);
	StreamerFilterText(pBuf);
}

void CGameClient::StreamerClan(int ClientId, char *pBuf, int BufSize) const
{
	if(!pBuf || BufSize <= 0)
		return;
	pBuf[0] = '\0';
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	if(!StreamerModeActive())
	{
		str_copy(pBuf, m_aClients[ClientId].m_aClan, BufSize);
		return;
	}
	if(ShouldHideStreamerIdentity(ClientId))
		return;
	str_copy(pBuf, m_aClients[ClientId].m_aClan, BufSize);
	StreamerFilterText(pBuf);
}

const char *CGameClient::StreamerName(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return "";
	if(!StreamerModeActive())
		return m_aClients[ClientId].m_aName;
	char *pBuf = m_aaStreamerTextBuf[m_StreamerTextBufIndex];
	m_StreamerTextBufIndex = (m_StreamerTextBufIndex + 1) % (int)std::size(m_aaStreamerTextBuf);
	if(ShouldHideStreamerIdentity(ClientId))
		str_format(pBuf, sizeof(m_aaStreamerTextBuf[0]), "%d", ClientId);
	else
		str_copy(pBuf, m_aClients[ClientId].m_aName, sizeof(m_aaStreamerTextBuf[0]));
	StreamerFilterText(pBuf);
	return pBuf;
}

const char *CGameClient::StreamerClan(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return "";
	if(!StreamerModeActive())
		return m_aClients[ClientId].m_aClan;
	if(ShouldHideStreamerIdentity(ClientId))
		return "";
	char *pBuf = m_aaStreamerTextBuf[m_StreamerTextBufIndex];
	m_StreamerTextBufIndex = (m_StreamerTextBufIndex + 1) % (int)std::size(m_aaStreamerTextBuf);
	str_copy(pBuf, m_aClients[ClientId].m_aClan, sizeof(m_aaStreamerTextBuf[0]));
	StreamerFilterText(pBuf);
	return pBuf;
}

void CGameClient::UpdateStreamerMode()
{
	if(str_comp(m_aStreamerWordsFileLoaded, g_Config.m_MeStreamerWordsFile) != 0)
	{
		str_copy(m_aStreamerWordsFileLoaded, g_Config.m_MeStreamerWordsFile);
		std::vector<std::string> vWords;
		if(g_Config.m_MeStreamerWordsFile[0] != '\0')
		{
			char *pData = Storage()->ReadFileStr(g_Config.m_MeStreamerWordsFile, IStorage::TYPE_ALL_OR_ABSOLUTE);
			if(pData)
			{
				const char *pLine = pData;
				while(*pLine)
				{
					const char *pEnd = pLine;
					while(*pEnd && *pEnd != '\n' && *pEnd != '\r')
						++pEnd;
					std::string Word(pLine, pEnd - pLine);
					const char *pTrim = str_utf8_skip_whitespaces(Word.c_str());
					std::string Trimmed(pTrim);
					while(!Trimmed.empty() && (Trimmed.back() == ' ' || Trimmed.back() == '\t'))
						Trimmed.pop_back();
					if(!Trimmed.empty() && Trimmed[0] != '#')
						vWords.push_back(Trimmed);
					pLine = *pEnd ? pEnd + 1 : pEnd;
				}
				free(pData);
			}
		}
		QmStreamerWordFilter::SetAdditionalWords(vWords);
	}

	bool Changed = false;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		const bool Hide = m_aClients[i].m_Active && ShouldHideStreamerSkin(i);
		if(Hide != m_aStreamerLastHideSkin[i])
		{
			m_aStreamerLastHideSkin[i] = Hide;
			if(m_aClients[i].m_Active)
			{
				m_aClients[i].UpdateSkinInfo();
				Changed = true;
			}
		}
	}
	(void)Changed;
}
