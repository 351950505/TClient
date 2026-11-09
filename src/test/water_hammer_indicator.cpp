#include <game/client/components/tclient/water_hammer_indicator_logic.h>

#ifdef TC_WATER_HAMMER_STANDALONE
#include <cstdio>
#include <cstdlib>
#define TEST(Suite, Name) static void Suite##_##Name()
#define EXPECT_TRUE(Value) do { if(!(Value)) { std::fprintf(stderr, "failed at line %d: %s\n", __LINE__, #Value); std::abort(); } } while(false)
#define EXPECT_FALSE(Value) EXPECT_TRUE(!(Value))
#define EXPECT_EQ(Left, Right) EXPECT_TRUE((Left) == (Right))
#else
#include <gtest/gtest.h>
#endif

TEST(WaterHammer, FreezeAndMissingInput)
{
	const STcWaterHammerInputState Held{true, WEAPON_HAMMER + 1, 1};
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_HAMMER, {}, nullptr));
	EXPECT_FALSE(TcShouldMarkWaterHammer(0, false, WEAPON_HAMMER, Held, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_HAMMER, Held, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Held, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(0, true, WEAPON_HAMMER, Held, nullptr));
}

TEST(WaterHammer, WeaponAndFireCounter)
{
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, WEAPON_HAMMER + 1, 1}, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_HAMMER, {true, WEAPON_GUN + 1, 1}, nullptr));
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, 0, 1}, nullptr));
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_NINJA, {true, WEAPON_NINJA + 1, 1}, nullptr));
	for(int Fire = 0; Fire < 64; ++Fire)
		EXPECT_EQ(TcShouldMarkWaterHammer(100, false, WEAPON_HAMMER, {true, 0, Fire}, nullptr), (Fire & 1) != 0);
}

TEST(WaterHammer, LocalInputTakesPriority)
{
	CNetObj_PlayerInput Local{};
	Local.m_WantedWeapon = WEAPON_HAMMER + 1;
	Local.m_Fire = 0;
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_HAMMER, {true, 1, 1}, &Local));
	Local.m_Fire = 3;
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {}, &Local));
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, WEAPON_GUN + 1, 2}, &Local));
	Local.m_WantedWeapon = WEAPON_GUN + 1;
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, 1, 1}, &Local));
	Local.m_Hook = 0;
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, 0, 0, 1}, &Local));
	Local.m_Hook = 1;
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {}, &Local));
}

TEST(WaterHammer, TargetFilters)
{
	EXPECT_TRUE(TcIsWaterHammerTarget(1, 0, true, false, true, true));
	const int aInvalidIds[] = {-1, MAX_CLIENTS};
	for(const int Invalid : aInvalidIds)
	{
		EXPECT_FALSE(TcIsWaterHammerTarget(Invalid, 0, true, false, true, true));
		EXPECT_FALSE(TcIsWaterHammerTarget(1, Invalid, true, false, true, true));
	}
	EXPECT_TRUE(TcIsWaterHammerTarget(0, 0, true, false, true, true));
	EXPECT_FALSE(TcIsWaterHammerTarget(1, 0, false, false, true, true));
	EXPECT_FALSE(TcIsWaterHammerTarget(1, 0, true, true, true, true));
	EXPECT_FALSE(TcIsWaterHammerTarget(1, 0, true, false, false, true));
	EXPECT_FALSE(TcIsWaterHammerTarget(1, 0, true, false, true, false));
}

TEST(WaterHammer, HookAndHammerBothMarkFrozenCharacters)
{
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, WEAPON_GUN + 1, 0, 1}, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_NINJA, {true, WEAPON_NINJA + 1, 0, 1}, nullptr));
	EXPECT_FALSE(TcShouldMarkWaterHammer(0, false, WEAPON_GUN, {true, 0, 0, 1}, nullptr));
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, 0, 0, 0}, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_HAMMER, {true, 0, 1, 0}, nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, 0, 0, 1}, nullptr));
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {}, nullptr));

	CNetObj_PlayerInput Local{};
	Local.m_Hook = 0;
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {true, 0, 0, 1}, &Local));
	Local.m_Hook = 1;
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, {}, &Local));
}

TEST(WaterHammer, LocalInputSourceIncludesCurrentAndConnectedDummy)
{
	EXPECT_EQ(TcWaterHammerInputSource(10, 10, 11, true), ETcWaterHammerInputSource::CURRENT_LOCAL);
	EXPECT_EQ(TcWaterHammerInputSource(11, 10, 11, true), ETcWaterHammerInputSource::DUMMY_LOCAL);
	EXPECT_EQ(TcWaterHammerInputSource(11, 10, 11, false), ETcWaterHammerInputSource::REMOTE);
	EXPECT_EQ(TcWaterHammerInputSource(12, 10, 11, true), ETcWaterHammerInputSource::REMOTE);
	EXPECT_EQ(TcWaterHammerInputSource(11, 11, 10, true), ETcWaterHammerInputSource::CURRENT_LOCAL);
	EXPECT_EQ(TcWaterHammerInputSource(10, 11, 10, true), ETcWaterHammerInputSource::DUMMY_LOCAL);
}

TEST(WaterHammer, PreInputHookPersistsUntilReleaseOrClientReset)
{
	CTcWaterHammerInputCache Inputs;
	Inputs.StorePreInput(0, 2, 105, WEAPON_GUN + 1, 0, 1);
	EXPECT_EQ(Inputs.PreInput(0, 2).m_Hook, 1);
	EXPECT_TRUE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, Inputs.PreInput(0, 2), nullptr));
	Inputs.StorePreInput(0, 2, 106, WEAPON_GUN + 1, 0, 0);
	EXPECT_EQ(Inputs.PreInput(0, 2).m_Hook, 0);
	EXPECT_FALSE(TcShouldMarkWaterHammer(100, false, WEAPON_GUN, Inputs.PreInput(0, 2), nullptr));
	Inputs.StorePreInput(0, 2, 107, WEAPON_GUN + 1, 0, 1);
	Inputs.ResetClient(0, 2);
	EXPECT_FALSE(Inputs.PreInput(0, 2).m_Received);
}

TEST(WaterHammer, PreInputBeforeSnapshots)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	Inputs.StorePreInput(0, 2, 105, WEAPON_HAMMER + 1, 1);
	Inputs.OnRoster(0, 100, aPresent); // Input can precede the owner's first roster/character.
	EXPECT_TRUE(Inputs.PreInput(0, 2).m_Received);
	aPresent[2] = true;
	Inputs.OnRoster(0, 101, aPresent);
	EXPECT_FALSE(TcIsWaterHammerTarget(2, 0, true, false, false, true));
	Inputs.OnRoster(0, 102, aPresent); // Two visible character snapshots, without another pre-input.
	EXPECT_TRUE(TcIsWaterHammerTarget(2, 0, true, false, true, true));
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));

	Inputs.StorePreInput(0, 2, 110, WEAPON_HAMMER + 1, 3);
	aPresent[2] = false;
	Inputs.OnRoster(0, 101, aPresent); // An already processed snapshot cannot remove held input.
	EXPECT_EQ(Inputs.PreInput(0, 2).m_Fire, 3);
	aPresent[2] = true;
	Inputs.OnRoster(0, 110, aPresent);
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
}

TEST(WaterHammer, VisibilityDeathAndSpectatingKeepHeldInput)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	aPresent[2] = true;
	Inputs.OnRoster(0, 100, aPresent);
	Inputs.StorePreInput(0, 2, 101, WEAPON_HAMMER + 1, 1);
	Inputs.OnRoster(0, 200, aPresent); // Roster remains while the character is out of view or dead.
	EXPECT_FALSE(TcIsWaterHammerTarget(2, 0, true, false, false, true));
	EXPECT_FALSE(TcIsWaterHammerTarget(2, 0, true, true, true, true));
	EXPECT_TRUE(Inputs.PreInput(0, 2).m_Received);
	Inputs.OnRoster(0, 300, aPresent);
	EXPECT_TRUE(TcIsWaterHammerTarget(2, 0, true, false, true, true));
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
}

TEST(WaterHammer, SwitchingConnectionsKeepsIndependentInputs)
{
	CTcWaterHammerInputCache Inputs;
	Inputs.StorePreInput(0, 2, 100, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 100, WEAPON_HAMMER + 1, 2);
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
	Inputs.ResetLocalInput(); // Swap changes only which physical connection is read.
	EXPECT_FALSE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(1, 2), nullptr));
	Inputs.ResetLocalInput();
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
}

TEST(WaterHammer, DummyDisconnectPreservesMainConnection)
{
	CTcWaterHammerInputCache Inputs;
	Inputs.StorePreInput(0, 2, 100, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 100, WEAPON_HAMMER + 1, 1);
	Inputs.RecordLocalInput(1, 3, true);
	Inputs.ResetConnection(1);
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
	EXPECT_FALSE(Inputs.PreInput(1, 2).m_Received);
	EXPECT_FALSE(Inputs.HasLocalInput(1, 3, true));
}

TEST(WaterHammer, DummyReconnectStartsWithoutOldInput)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	aPresent[2] = true;
	Inputs.OnRoster(1, 100, aPresent);
	Inputs.StorePreInput(0, 2, 101, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 101, WEAPON_HAMMER + 1, 1);
	Inputs.ResetConnection(1);
	Inputs.OnRoster(1, 102, aPresent);
	EXPECT_FALSE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(1, 2), nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
	Inputs.StorePreInput(1, 2, 103, WEAPON_HAMMER + 1, 2);
	EXPECT_EQ(Inputs.PreInput(1, 2).m_Fire, 2);
	EXPECT_EQ(Inputs.PreInput(0, 2).m_Fire, 1);
}

TEST(WaterHammer, ValidDropClearsOnlyItsConnectionAndClient)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	aPresent[2] = true;
	Inputs.OnRoster(0, 100, aPresent);
	Inputs.StorePreInput(0, 2, 110, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(0, 3, 110, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 110, WEAPON_HAMMER + 1, 1);
	Inputs.RecordLocalInput(0, 2, false);
	Inputs.ResetClient(0, 2); // Explicit valid drop is authoritative, even before IntendedTick.
	Inputs.OnRoster(0, 101, aPresent); // The same slot is reused without fresh input.
	EXPECT_FALSE(Inputs.PreInput(0, 2).m_Received);
	EXPECT_FALSE(Inputs.HasLocalInput(0, 2, false));
	EXPECT_TRUE(Inputs.PreInput(0, 3).m_Received);
	EXPECT_TRUE(Inputs.PreInput(1, 2).m_Received);
	Inputs.ResetClient(-1, 2);
	Inputs.ResetClient(2, 2);
	Inputs.StorePreInput(0, -1, 101, 1, 1);
	Inputs.StorePreInput(0, MAX_CLIENTS, 101, 1, 1);
	EXPECT_FALSE(Inputs.PreInput(0, -1).m_Received);
	EXPECT_FALSE(Inputs.PreInput(2, 2).m_Received);
}

TEST(WaterHammer, InactiveRosterRemovalAndSlotReuse)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	aPresent[2] = true;
	Inputs.OnRoster(1, 200, aPresent);
	Inputs.StorePreInput(0, 2, 205, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 205, WEAPON_HAMMER + 1, 1);
	aPresent[2] = false;
	Inputs.OnRoster(1, 201, aPresent);
	EXPECT_FALSE(Inputs.PreInput(1, 2).m_Received); // A confirmed removal ends this occupant's input.
	Inputs.OnRoster(1, 205, aPresent); // Continued absence must not restore the old input.
	EXPECT_FALSE(Inputs.PreInput(1, 2).m_Received);
	aPresent[2] = true;
	Inputs.OnRoster(1, 206, aPresent);
	EXPECT_FALSE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(1, 2), nullptr));
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(0, 2), nullptr));
	Inputs.StorePreInput(1, 2, 207, WEAPON_HAMMER + 1, 1);
	aPresent[2] = false;
	Inputs.OnRoster(1, 205, aPresent); // Replayed/older snapshots cannot remove a new occupant.
	EXPECT_TRUE(Inputs.PreInput(1, 2).m_Received);
}

TEST(WaterHammer, ReusedSlotDoesNotInheritFutureInput)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	aPresent[2] = true;
	Inputs.OnRoster(1, 100, aPresent);
	Inputs.StorePreInput(1, 2, 110, WEAPON_HAMMER + 1, 1);
	Inputs.RecordLocalInput(1, 2, true);
	aPresent[2] = false;
	Inputs.OnRoster(1, 101, aPresent);
	EXPECT_FALSE(Inputs.PreInput(1, 2).m_Received);
	EXPECT_FALSE(Inputs.HasLocalInput(1, 2, true));
	aPresent[2] = true;
	Inputs.OnRoster(1, 102, aPresent); // Reuse before the old input's intended tick.
	Inputs.OnRoster(1, 110, aPresent);
	EXPECT_FALSE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(1, 2), nullptr));
	Inputs.StorePreInput(1, 2, 111, WEAPON_HAMMER + 1, 1);
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, Inputs.PreInput(1, 2), nullptr));
}

TEST(WaterHammer, NewSessionDoesNotInheritEitherConnection)
{
	CTcWaterHammerInputCache Inputs;
	std::array<bool, MAX_CLIENTS> aPresent{};
	aPresent[2] = true;
	Inputs.OnRoster(0, 100, aPresent);
	Inputs.OnRoster(1, 100, aPresent);
	Inputs.StorePreInput(0, 2, 101, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 101, WEAPON_HAMMER + 1, 1);
	Inputs.RecordLocalInput(1, 3, true);
	Inputs.Reset(); // Map/session reset also resets the snapshot tick guards.
	Inputs.OnRoster(0, 1, aPresent);
	Inputs.OnRoster(1, 1, aPresent);
	EXPECT_FALSE(Inputs.PreInput(0, 2).m_Received);
	EXPECT_FALSE(Inputs.PreInput(1, 2).m_Received);
	EXPECT_FALSE(Inputs.HasLocalInput(1, 3, true));
	Inputs.StorePreInput(0, 2, 2, WEAPON_HAMMER + 1, 1);
	aPresent[2] = false;
	Inputs.OnRoster(0, 2, aPresent);
	EXPECT_FALSE(Inputs.PreInput(0, 2).m_Received);
}

TEST(WaterHammer, LocalSwapRecordsCopiedInputWithoutActiveSnapshot)
{
	CTcWaterHammerInputCache Inputs;
	Inputs.StorePreInput(0, 2, 100, WEAPON_HAMMER + 1, 1);
	Inputs.StorePreInput(1, 2, 100, WEAPON_HAMMER + 1, 1);
	Inputs.RecordLocalInput(1, 3, true);
	Inputs.ResetLocalInput(); // Snapshot has been invalidated before OnDummySwap.
	Inputs.RecordLocalInput(0, 4, false); // The copied m_DummyInput still belongs to the old controlled tee.
	EXPECT_TRUE(Inputs.HasLocalInput(0, 4, false));
	EXPECT_FALSE(Inputs.HasLocalInput(0, 4, true));
	EXPECT_FALSE(Inputs.HasLocalInput(1, 3, true));
	EXPECT_FALSE(Inputs.HasLocalInput(1, 4, false));
	EXPECT_TRUE(Inputs.PreInput(0, 2).m_Received);
	EXPECT_TRUE(Inputs.PreInput(1, 2).m_Received);
	CNetObj_PlayerInput CopiedInput{};
	CopiedInput.m_WantedWeapon = WEAPON_HAMMER + 1;
	CopiedInput.m_Fire = 1;
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, WEAPON_HAMMER, {}, &CopiedInput));
	Inputs.RecordLocalInput(0, 4, true); // Only an actual new hammer input grants this receipt.
	EXPECT_TRUE(Inputs.HasLocalInput(0, 4, true));
	Inputs.ResetLocalInput();
	Inputs.RecordLocalInput(1, 3, false);
	EXPECT_TRUE(Inputs.HasLocalInput(1, 3, false));
	EXPECT_FALSE(Inputs.HasLocalInput(1, 3, true));
}

TEST(WaterHammer, FrozenKatanaDoesNotChangeRealWeapon)
{
	CNetObj_Character Character{};
	Character.m_Weapon = WEAPON_HAMMER;
	Character.m_AttackTick = 123;
	const auto Katana = TcFrozenKatanaRenderCharacter(Character, true);
	EXPECT_EQ(Katana.m_Weapon, WEAPON_NINJA);
	EXPECT_EQ(Katana.m_AttackTick, Character.m_AttackTick);
	EXPECT_EQ(Character.m_Weapon, WEAPON_HAMMER);
	EXPECT_TRUE(TcShouldMarkWaterHammer(-1, false, Character.m_Weapon, {true, 0, 1}, nullptr));
	EXPECT_EQ(TcFrozenKatanaRenderCharacter(Character, false).m_Weapon, WEAPON_HAMMER);
	Character.m_Weapon = WEAPON_GUN;
	EXPECT_EQ(TcFrozenKatanaRenderCharacter(Character, true).m_Weapon, WEAPON_NINJA);
	EXPECT_FALSE(TcShouldMarkWaterHammer(-1, false, Character.m_Weapon, {true, 0, 1}, nullptr));
}

#ifdef TC_WATER_HAMMER_STANDALONE
int main()
{
	WaterHammer_FreezeAndMissingInput();
	WaterHammer_WeaponAndFireCounter();
	WaterHammer_LocalInputTakesPriority();
	WaterHammer_TargetFilters();
	WaterHammer_HookAndHammerBothMarkFrozenCharacters();
	WaterHammer_LocalInputSourceIncludesCurrentAndConnectedDummy();
	WaterHammer_PreInputHookPersistsUntilReleaseOrClientReset();
	WaterHammer_PreInputBeforeSnapshots();
	WaterHammer_VisibilityDeathAndSpectatingKeepHeldInput();
	WaterHammer_SwitchingConnectionsKeepsIndependentInputs();
	WaterHammer_DummyDisconnectPreservesMainConnection();
	WaterHammer_DummyReconnectStartsWithoutOldInput();
	WaterHammer_ValidDropClearsOnlyItsConnectionAndClient();
	WaterHammer_InactiveRosterRemovalAndSlotReuse();
	WaterHammer_ReusedSlotDoesNotInheritFutureInput();
	WaterHammer_NewSessionDoesNotInheritEitherConnection();
	WaterHammer_LocalSwapRecordsCopiedInputWithoutActiveSnapshot();
	WaterHammer_FrozenKatanaDoesNotChangeRealWeapon();
	std::puts("WaterHammer: 18 test groups passed");
}
#endif
