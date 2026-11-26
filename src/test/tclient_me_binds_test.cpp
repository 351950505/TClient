#include <game/client/components/tclient/me_binds.h>

#include <gtest/gtest.h>

TEST(ClientMeBinds, SelectsHammerFromAnyOtherWeapon)
{
	for(int Weapon : {WEAPON_GUN, WEAPON_SHOTGUN, WEAPON_GRENADE, WEAPON_LASER, WEAPON_NINJA})
		EXPECT_EQ(tclient_me_binds::HammerGunInput(Weapon), WEAPON_HAMMER + 1);
}

TEST(ClientMeBinds, SelectsGunFromHammer)
{
	EXPECT_EQ(tclient_me_binds::HammerGunInput(WEAPON_HAMMER), WEAPON_GUN + 1);
}

TEST(ClientMeBinds, MeCommandsOnlyRunOnInitialPress)
{
	for(const char *pBind : {"me_toggle_hammer_gun", "tc_quick_zoom", "toggle me_stack_tee_hint 0 1"})
	{
		SCOPED_TRACE(pBind);
		EXPECT_TRUE(tclient_me_binds::ShouldExecute(pBind, false));
		EXPECT_FALSE(tclient_me_binds::ShouldExecute(pBind, true));
	}
}

TEST(ClientMeBinds, UnrelatedCommandsStillRepeat)
{
	for(const char *pBind : {"+fire", "+nextweapon", "zoom+", "toggle tc_show_others_ghosts 0 1", "+tc_pingwheel", "tc_translate_recent"})
	{
		SCOPED_TRACE(pBind);
		EXPECT_TRUE(tclient_me_binds::ShouldExecute(pBind, true));
	}
}
