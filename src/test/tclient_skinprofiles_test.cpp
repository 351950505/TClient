#include <game/client/components/tclient/skinprofiles.h>

#include <gtest/gtest.h>

class CProfileConfigTest : public ::testing::TestWithParam<int>
{
protected:
	CConfig m_Config{};

	void SetUp() override
	{
		m_Config.m_TcProfileColors = 1;
		m_Config.m_TcProfileEmote = 1;
		m_Config.m_ClPlayerColorBody = 11;
		m_Config.m_ClPlayerColorFeet = 12;
		m_Config.m_ClDummyColorBody = 21;
		m_Config.m_ClDummyColorFeet = 22;
		m_Config.m_ClPlayerDefaultEyes = EMOTE_ANGRY;
		m_Config.m_ClDummyDefaultEyes = EMOTE_ANGRY;
	}
};

TEST_P(CProfileConfigTest, ColorsEnableSelectedSlot)
{
	const CProfile Profile(0x123456, 0x654321, -2, -1, "", "", "");
	Profile.ApplyToConfig(GetParam(), m_Config);

	EXPECT_EQ(m_Config.m_ClPlayerUseCustomColor, GetParam() ? 0 : 1);
	EXPECT_EQ(m_Config.m_ClDummyUseCustomColor, GetParam() ? 1 : 0);
	EXPECT_EQ(m_Config.m_ClPlayerColorBody, GetParam() ? 11 : 0x123456);
	EXPECT_EQ(m_Config.m_ClPlayerColorFeet, GetParam() ? 12 : 0x654321);
	EXPECT_EQ(m_Config.m_ClDummyColorBody, GetParam() ? 0x123456 : 21);
	EXPECT_EQ(m_Config.m_ClDummyColorFeet, GetParam() ? 0x654321 : 22);
}

TEST_P(CProfileConfigTest, ColorSentinelsPreserveSettings)
{
	const int aaColors[][2] = {{-1, -1}, {-1, 0x654321}, {0x123456, -1}};
	for(const auto &aColors : aaColors)
	{
		SCOPED_TRACE(::testing::Message() << aColors[0] << ", " << aColors[1]);
		for(int Enabled : {0, 1})
		{
			m_Config.m_ClPlayerUseCustomColor = Enabled;
			m_Config.m_ClDummyUseCustomColor = Enabled;
			const CProfile Profile(aColors[0], aColors[1], -2, -1, "", "", "");
			Profile.ApplyToConfig(GetParam(), m_Config);

			EXPECT_EQ(m_Config.m_ClPlayerUseCustomColor, Enabled);
			EXPECT_EQ(m_Config.m_ClDummyUseCustomColor, Enabled);
			EXPECT_EQ(m_Config.m_ClPlayerColorBody, 11);
			EXPECT_EQ(m_Config.m_ClPlayerColorFeet, 12);
			EXPECT_EQ(m_Config.m_ClDummyColorBody, 21);
			EXPECT_EQ(m_Config.m_ClDummyColorFeet, 22);
		}
	}
}

TEST_P(CProfileConfigTest, DisabledColorsPreserveSettings)
{
	m_Config.m_TcProfileColors = 0;
	const CProfile Profile(0x123456, 0x654321, -2, -1, "", "", "");
	Profile.ApplyToConfig(GetParam(), m_Config);

	EXPECT_EQ(m_Config.m_ClPlayerUseCustomColor, 0);
	EXPECT_EQ(m_Config.m_ClDummyUseCustomColor, 0);
	EXPECT_EQ(m_Config.m_ClPlayerColorBody, 11);
	EXPECT_EQ(m_Config.m_ClPlayerColorFeet, 12);
	EXPECT_EQ(m_Config.m_ClDummyColorBody, 21);
	EXPECT_EQ(m_Config.m_ClDummyColorFeet, 22);
}

TEST_P(CProfileConfigTest, NormalEmoteResetsSelectedSlot)
{
	const CProfile Profile(-1, -1, -2, EMOTE_NORMAL, "", "", "");
	Profile.ApplyToConfig(GetParam(), m_Config);

	EXPECT_EQ(m_Config.m_ClPlayerDefaultEyes, GetParam() ? EMOTE_ANGRY : EMOTE_NORMAL);
	EXPECT_EQ(m_Config.m_ClDummyDefaultEyes, GetParam() ? EMOTE_NORMAL : EMOTE_ANGRY);
}

TEST_P(CProfileConfigTest, InvalidEmotesPreserveSettings)
{
	const int aInvalidEmotes[] = {-1, NUM_EMOTES, NUM_EMOTES + 1};
	for(int Emote : aInvalidEmotes)
	{
		SCOPED_TRACE(Emote);
		const CProfile Profile(-1, -1, -2, Emote, "", "", "");
		Profile.ApplyToConfig(GetParam(), m_Config);

		EXPECT_EQ(m_Config.m_ClPlayerDefaultEyes, EMOTE_ANGRY);
		EXPECT_EQ(m_Config.m_ClDummyDefaultEyes, EMOTE_ANGRY);
	}
}

TEST_P(CProfileConfigTest, DisabledEmotePreservesSettings)
{
	m_Config.m_TcProfileEmote = 0;
	const CProfile Profile(-1, -1, -2, EMOTE_NORMAL, "", "", "");
	Profile.ApplyToConfig(GetParam(), m_Config);

	EXPECT_EQ(m_Config.m_ClPlayerDefaultEyes, EMOTE_ANGRY);
	EXPECT_EQ(m_Config.m_ClDummyDefaultEyes, EMOTE_ANGRY);
}

TEST_P(CProfileConfigTest, IdentitySettingsApplyOnlyToSelectedSlot)
{
	m_Config.m_TcProfileSkin = 1;
	m_Config.m_TcProfileName = 1;
	m_Config.m_TcProfileClan = 1;
	m_Config.m_TcProfileFlag = 1;
	m_Config.m_PlayerCountry = 10;
	m_Config.m_ClDummyCountry = 20;
	const CProfile Profile(-1, -1, 42, -1, "default", "name", "clan");
	Profile.ApplyToConfig(GetParam(), m_Config);

	EXPECT_STREQ(m_Config.m_ClPlayerSkin, GetParam() ? "" : "default");
	EXPECT_STREQ(m_Config.m_ClDummySkin, GetParam() ? "default" : "");
	EXPECT_STREQ(m_Config.m_PlayerName, GetParam() ? "" : "name");
	EXPECT_STREQ(m_Config.m_ClDummyName, GetParam() ? "name" : "");
	EXPECT_STREQ(m_Config.m_PlayerClan, GetParam() ? "" : "clan");
	EXPECT_STREQ(m_Config.m_ClDummyClan, GetParam() ? "clan" : "");
	EXPECT_EQ(m_Config.m_PlayerCountry, GetParam() ? 10 : 42);
	EXPECT_EQ(m_Config.m_ClDummyCountry, GetParam() ? 42 : 20);
}

TEST_P(CProfileConfigTest, EmptyClanRequiresOverwriteOption)
{
	m_Config.m_TcProfileClan = 1;
	str_copy(m_Config.m_PlayerClan, "main");
	str_copy(m_Config.m_ClDummyClan, "dummy");
	const CProfile Profile(-1, -1, -2, -1, "", "", "");
	Profile.ApplyToConfig(GetParam(), m_Config);
	EXPECT_STREQ(m_Config.m_PlayerClan, "main");
	EXPECT_STREQ(m_Config.m_ClDummyClan, "dummy");

	m_Config.m_TcProfileOverwriteClanWithEmpty = 1;
	Profile.ApplyToConfig(GetParam(), m_Config);
	EXPECT_STREQ(m_Config.m_PlayerClan, GetParam() ? "main" : "");
	EXPECT_STREQ(m_Config.m_ClDummyClan, GetParam() ? "" : "dummy");
}

INSTANTIATE_TEST_SUITE_P(MainAndDummy, CProfileConfigTest, ::testing::Values(0, 1));
