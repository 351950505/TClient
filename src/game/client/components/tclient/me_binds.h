#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_ME_BINDS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_ME_BINDS_H

#include <generated/protocol.h>

#include <string_view>

namespace tclient_me_binds
{
	inline constexpr char HAMMER_GUN[] = "me_toggle_hammer_gun";
	inline constexpr char QUICK_ZOOM[] = "tc_quick_zoom";
	inline constexpr char STACK_TEE_HINT[] = "toggle me_stack_tee_hint 0 1";

	constexpr bool ShouldExecute(std::string_view Bind, bool Repeat)
	{
		// Limit this to the exact binds written by the ME settings page.
		return !Repeat || (Bind != HAMMER_GUN && Bind != QUICK_ZOOM && Bind != STACK_TEE_HINT);
	}

	constexpr int HammerGunInput(int CurrentWeapon)
	{
		return (CurrentWeapon == WEAPON_HAMMER ? WEAPON_GUN : WEAPON_HAMMER) + 1;
	}
} // namespace tclient_me_binds

#endif
