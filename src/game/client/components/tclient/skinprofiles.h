#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_SKINPROFILES_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_SKINPROFILES_H

#include <base/str.h>

#include <engine/console.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include <game/client/component.h>

#include <generated/protocol.h>

#include <cstring>
#include <vector>

class CProfile
{
public:
	int m_BodyColor;
	int m_FeetColor;
	int m_CountryFlag;
	int m_Emote;
	char m_SkinName[24];
	char m_Name[MAX_NAME_LENGTH];
	char m_Clan[MAX_CLAN_LENGTH];
	CProfile(int BodyColor, int FeetColor, int CountryFlag, int Emote, const char *pSkinName, const char *pName, const char *pClan)
	{
		m_BodyColor = BodyColor;
		m_FeetColor = FeetColor;
		m_CountryFlag = CountryFlag;
		m_Emote = Emote;
		str_copy(m_SkinName, pSkinName);
		str_copy(m_Name, pName);
		str_copy(m_Clan, pClan);
	}

	void ApplyToConfig(int Dummy, CConfig &Config) const
	{
		if(Config.m_TcProfileSkin && strlen(m_SkinName) != 0)
			str_copy(Dummy ? Config.m_ClDummySkin : Config.m_ClPlayerSkin, m_SkinName);
		if(Config.m_TcProfileColors && m_BodyColor != -1 && m_FeetColor != -1)
		{
			(Dummy ? Config.m_ClDummyUseCustomColor : Config.m_ClPlayerUseCustomColor) = 1;
			(Dummy ? Config.m_ClDummyColorBody : Config.m_ClPlayerColorBody) = m_BodyColor;
			(Dummy ? Config.m_ClDummyColorFeet : Config.m_ClPlayerColorFeet) = m_FeetColor;
		}
		if(Config.m_TcProfileEmote && m_Emote >= EMOTE_NORMAL && m_Emote < NUM_EMOTES)
			(Dummy ? Config.m_ClDummyDefaultEyes : Config.m_ClPlayerDefaultEyes) = m_Emote;
		if(Config.m_TcProfileName && strlen(m_Name) != 0)
			str_copy(Dummy ? Config.m_ClDummyName : Config.m_PlayerName, m_Name); // TODO m_ClPlayerName
		if(Config.m_TcProfileClan && (strlen(m_Clan) != 0 || Config.m_TcProfileOverwriteClanWithEmpty))
			str_copy(Dummy ? Config.m_ClDummyClan : Config.m_PlayerClan, m_Clan); // TODO m_ClPlayerClan
		if(Config.m_TcProfileFlag && m_CountryFlag != -2)
			(Dummy ? Config.m_ClDummyCountry : Config.m_PlayerCountry) = m_CountryFlag;
	}
};

class CSkinProfiles : public CComponent
{
	static void ConAddProfile(IConsole::IResult *pResult, void *pUserData);

	static void ConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData);

public:
	std::vector<CProfile> m_Profiles;
	void AddProfile(int BodyColor, int FeetColor, int CountryFlag, int Emote, const char *pSkinName, const char *pName, const char *pClan);
	void ApplyProfile(int Dummy, const CProfile &Profile);

	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
};
#endif
