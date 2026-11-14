#include <game/client/components/tclient/frozen_hud_logic.h>

#ifdef TC_FROZEN_HUD_STANDALONE
#include <cstdio>
#include <cstdlib>
#define TEST(Suite, Name) static void Suite##_##Name()
#define EXPECT_TRUE(Value) \
	do \
	{ \
		if(!(Value)) \
		{ \
			std::fprintf(stderr, "failed at line %d: %s\n", __LINE__, #Value); \
			std::abort(); \
		} \
	} while(false)
#define EXPECT_FALSE(Value) EXPECT_TRUE(!(Value))
#define EXPECT_EQ(Left, Right) EXPECT_TRUE((Left) == (Right))
#else
#include <gtest/gtest.h>
#endif

TEST(FrozenHud, FreshSnapshotClassifiesFirstVisibleFrame)
{
	// Two-frame render m_Active may still be false on entry into view. The
	// current Character plus current extended data already suffice for HUD.
	EXPECT_EQ(TcFrozenHudState(true, true, 100), ETcFrozenHudState::FROZEN);
	EXPECT_EQ(TcFrozenHudState(true, true, 0), ETcFrozenHudState::UNFROZEN);
	EXPECT_EQ(TcFrozenHudState(true, true, -1), ETcFrozenHudState::FROZEN);
}

TEST(FrozenHud, MissingFreshOrExtendedIsUnknownNotCachedFrozen)
{
	for(int OldFreezeEnd : {-1, 0, 100})
	{
		EXPECT_EQ(TcFrozenHudState(false, false, OldFreezeEnd), ETcFrozenHudState::UNKNOWN);
		EXPECT_EQ(TcFrozenHudState(false, true, OldFreezeEnd), ETcFrozenHudState::UNKNOWN);
		EXPECT_EQ(TcFrozenHudState(true, false, OldFreezeEnd), ETcFrozenHudState::UNKNOWN);
	}
	// Ordinary/deep values only; don't turn other negative sentinel values
	// into deep freeze by replacing this predicate with FreezeEnd != 0.
	EXPECT_EQ(TcFrozenHudState(true, true, -2), ETcFrozenHudState::UNFROZEN);
}

TEST(FrozenHud, LiveFreezeAndInFreezeAreNotOrdinaryOrDeepFreeze)
{
	// Flags deliberately never enter the production helper. A character in a
	// freeze tile, or only live-frozen, still has FreezeEnd == 0 for this HUD.
	struct SExtendedFixture
	{
		int m_FreezeEnd;
		bool m_LiveFrozen;
		bool m_InFreeze;
	};
	for(bool LiveFrozen : {false, true})
		for(bool InFreeze : {false, true})
		{
			const SExtendedFixture Character{0, LiveFrozen, InFreeze};
			EXPECT_EQ(TcFrozenHudState(true, true, Character.m_FreezeEnd), ETcFrozenHudState::UNFROZEN);
		}
}

TEST(FrozenHud, CountsAndNotificationUseSameThreeStates)
{
	STcFrozenHudFrame<8> Frame;
	Frame.AddPlayer(0, TcFrozenHudState(true, true, 0));
	Frame.AddPlayer(1, TcFrozenHudState(true, true, 200));
	Frame.AddPlayer(2, TcFrozenHudState(true, true, -1));
	EXPECT_EQ(Frame.m_Counts.m_NumInTeam, 3);
	EXPECT_EQ(Frame.m_Counts.m_KnownUnfrozen, 1);
	EXPECT_EQ(Frame.m_Counts.m_Frozen, 2);
	EXPECT_EQ(Frame.m_Counts.m_Unknown, 0);
	EXPECT_TRUE(Frame.m_Counts.NotifyWhenLast());

	// An out-of-view or spectator PlayerInfo stays in the filtered team list,
	// but is neither known unfrozen nor frozen and must suppress last notify.
	Frame.AddPlayer(3, TcFrozenHudState(false, false, 200));
	EXPECT_EQ(Frame.m_Counts.m_NumInTeam, 4);
	EXPECT_EQ(Frame.m_Counts.m_KnownUnfrozen, 1);
	EXPECT_EQ(Frame.m_Counts.m_Frozen, 2);
	EXPECT_EQ(Frame.m_Counts.m_Unknown, 1);
	EXPECT_FALSE(Frame.m_Counts.NotifyWhenLast());
	EXPECT_EQ(Frame.m_Counts.m_NumInTeam,
		Frame.m_Counts.m_KnownUnfrozen + Frame.m_Counts.m_Frozen + Frame.m_Counts.m_Unknown);
	EXPECT_EQ(Frame.m_aStates[3], ETcFrozenHudState::UNKNOWN);
	EXPECT_TRUE(Frame.m_aIncluded[3]);
	EXPECT_FALSE(Frame.m_aIncluded[4]);
}

TEST(FrozenHud, LastNotifyNeedsTeamAndExactlyOneKnownUnfrozen)
{
	STcFrozenHudCounts Counts;
	EXPECT_FALSE(Counts.NotifyWhenLast());
	Counts.Add(ETcFrozenHudState::UNFROZEN);
	EXPECT_FALSE(Counts.NotifyWhenLast()); // Existing only-one-player gate.
	Counts.Add(ETcFrozenHudState::FROZEN);
	EXPECT_TRUE(Counts.NotifyWhenLast());
	Counts.Add(ETcFrozenHudState::UNFROZEN);
	EXPECT_FALSE(Counts.NotifyWhenLast());
	STcFrozenHudCounts AllFrozen;
	AllFrozen.Add(ETcFrozenHudState::FROZEN);
	AllFrozen.Add(ETcFrozenHudState::FROZEN);
	EXPECT_FALSE(AllFrozen.NotifyWhenLast());
	STcFrozenHudCounts AllUnknown;
	AllUnknown.Add(ETcFrozenHudState::UNKNOWN);
	AllUnknown.Add(ETcFrozenHudState::UNKNOWN);
	EXPECT_EQ(AllUnknown.m_KnownUnfrozen, 0);
	EXPECT_EQ(AllUnknown.m_Frozen, 0);
	EXPECT_FALSE(AllUnknown.NotifyWhenLast());
}

TEST(FrozenHud, TeamClientIdValidatesLocalAndTargetIndependently)
{
	constexpr int MaxClients = 8;
	EXPECT_EQ(TcFrozenHudTeamClientId(-1, true, 3, MaxClients), 3); // Demo has no local.
	EXPECT_EQ(TcFrozenHudTeamClientId(2, false, -1, MaxClients), 2); // Normal play.
	EXPECT_EQ(TcFrozenHudTeamClientId(2, true, 3, MaxClients), 3);
	EXPECT_EQ(TcFrozenHudTeamClientId(2, false, 3, MaxClients), 2);
	EXPECT_EQ(TcFrozenHudTeamClientId(2, true, -1, MaxClients), 2); // Freeview fallback.
	EXPECT_EQ(TcFrozenHudTeamClientId(2, true, MaxClients, MaxClients), 2);
	EXPECT_EQ(TcFrozenHudTeamClientId(MaxClients, true, 0, MaxClients), 0);
	EXPECT_EQ(TcFrozenHudTeamClientId(-1, true, MaxClients - 1, MaxClients), MaxClients - 1);
	EXPECT_EQ(TcFrozenHudTeamClientId(-1, false, 3, MaxClients), -1);
	EXPECT_EQ(TcFrozenHudTeamClientId(-1, true, -1, MaxClients), -1);
	EXPECT_EQ(TcFrozenHudTeamClientId(MaxClients, true, MaxClients, MaxClients), -1);
}

TEST(FrozenHud, PendingFrameIsAStableCopyAndGuardsDuplicateIds)
{
	STcFrozenHudFrame<4> Frame;
	Frame.AddPlayer(-1, ETcFrozenHudState::FROZEN);
	Frame.AddPlayer(4, ETcFrozenHudState::FROZEN);
	EXPECT_EQ(Frame.m_Counts.m_NumInTeam, 0);
	Frame.AddPlayer(0, ETcFrozenHudState::UNFROZEN);
	Frame.AddPlayer(0, ETcFrozenHudState::FROZEN);
	EXPECT_EQ(Frame.m_Counts.m_NumInTeam, 1);
	EXPECT_EQ(Frame.m_aStates[0], ETcFrozenHudState::UNFROZEN);
	const auto Pending = Frame;
	Frame = {};
	Frame.AddPlayer(0, ETcFrozenHudState::UNKNOWN);
	EXPECT_EQ(Pending.m_Counts.m_KnownUnfrozen, 1);
	EXPECT_EQ(Pending.m_Counts.m_Unknown, 0);
	EXPECT_EQ(Pending.m_aStates[0], ETcFrozenHudState::UNFROZEN);
	EXPECT_EQ(Frame.m_Counts.m_Unknown, 1);
}

TEST(FrozenHud, StableClientOrderAndOverflowPriority)
{
	STcFrozenHudFrame<8> Frame;
	Frame.AddPlayer(0, ETcFrozenHudState::FROZEN);
	Frame.AddPlayer(1, ETcFrozenHudState::UNKNOWN);
	Frame.AddPlayer(3, ETcFrozenHudState::UNFROZEN);
	Frame.AddPlayer(4, ETcFrozenHudState::FROZEN);
	Frame.AddPlayer(6, ETcFrozenHudState::UNFROZEN);
	const int aNormal[] = {0, 1, 3, 4, 6};
	const int aOverflow[] = {3, 6, 1, 0, 4};
	for(int Capacity : {1, 2, 3, 4, 5, 6, 8})
	{
		const bool Overflow = Frame.m_Counts.m_NumInTeam > Capacity;
		int Displayed = 0;
		for(int Pass = 0; Pass < (Overflow ? 3 : 1); ++Pass)
			for(int Id = 0; Id < 8 && Displayed < Capacity; ++Id)
			{
				if(!Frame.m_aIncluded[Id] || (Overflow && TcFrozenHudOverflowPass(Frame.m_aStates[Id]) != Pass))
					continue;
				EXPECT_EQ(Id, Overflow ? aOverflow[Displayed] : aNormal[Displayed]);
				++Displayed;
			}
		EXPECT_EQ(Displayed, Capacity < 5 ? Capacity : 5);
	}
}

#ifdef TC_FROZEN_HUD_STANDALONE
int main()
{
	FrozenHud_FreshSnapshotClassifiesFirstVisibleFrame();
	FrozenHud_MissingFreshOrExtendedIsUnknownNotCachedFrozen();
	FrozenHud_LiveFreezeAndInFreezeAreNotOrdinaryOrDeepFreeze();
	FrozenHud_CountsAndNotificationUseSameThreeStates();
	FrozenHud_LastNotifyNeedsTeamAndExactlyOneKnownUnfrozen();
	FrozenHud_TeamClientIdValidatesLocalAndTargetIndependently();
	FrozenHud_PendingFrameIsAStableCopyAndGuardsDuplicateIds();
	FrozenHud_StableClientOrderAndOverflowPriority();
	std::puts("PASS: 8 frozen HUD state/count/notify/team/pending/overflow regression tests");
}
#endif
