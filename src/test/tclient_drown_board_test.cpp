#include <game/client/components/tclient/drown_board.h>

#include <gtest/gtest.h>

namespace
{
	void Observe(CTClientDrownBoard &Tracker, int ClientId, bool Frozen, bool Hooking = false, int Team = 1, bool IncludeTeamZero = true)
	{
		Tracker.Observe(ClientId, true, "player", "clan", Team, true, Frozen, Hooking, false, IncludeTeamZero);
	}

	void ObserveOnStart(CTClientDrownBoard &Tracker, int ClientId, int Team = 1)
	{
		Tracker.Observe(ClientId, true, "player", "clan", Team, true, false, false, true, true);
	}
}

TEST(ClientDrownBoard, TeamZeroCountsAndAppearsWhenIncluded)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false, false, TEAM_FLOCK);
	Observe(Tracker, 1, true, false, TEAM_FLOCK);
	Observe(Tracker, 1, true, true, TEAM_FLOCK);
	EXPECT_EQ(Tracker.Count(1), 1);
	EXPECT_TRUE(CTClientDrownBoard::IsBoardVisible(true, true, TEAM_FLOCK, true));
	EXPECT_TRUE(CTClientDrownBoard::IsSameTrackedTeam(TEAM_FLOCK, TEAM_FLOCK, true));
	EXPECT_FALSE(CTClientDrownBoard::IsSameTrackedTeam(TEAM_FLOCK, 1, true));
	Observe(Tracker, 1, false, false, TEAM_FLOCK);
	Observe(Tracker, 1, false, true, TEAM_FLOCK);
	Observe(Tracker, 1, true, false, TEAM_FLOCK);
	EXPECT_EQ(Tracker.Count(1), 2);
}

TEST(ClientDrownBoard, TeamZeroDoesNotCountOrAppearWhenExcluded)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false, false, TEAM_FLOCK, false);
	Observe(Tracker, 1, true, false, TEAM_FLOCK, false);
	EXPECT_EQ(Tracker.Count(1), 0);
	EXPECT_FALSE(CTClientDrownBoard::IsBoardVisible(true, true, TEAM_FLOCK, false));
	EXPECT_FALSE(CTClientDrownBoard::IsSameTrackedTeam(TEAM_FLOCK, TEAM_FLOCK, false));
}

TEST(ClientDrownBoard, FirstFallCountsOnceWhileFreezeContinues)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, true, true);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, InitiallyFrozenPlayerEstablishesBaseline)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, RescueNeedsNewHookBeforeNextFall)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 2);
}

TEST(ClientDrownBoard, HookStartedWhileFrozenDoesNotRearmAfterRescue)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, true, true);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, false, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 2);
}

TEST(ClientDrownBoard, TeammatesHaveIndependentCounts)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 2, false);
	Observe(Tracker, 1, true);
	Observe(Tracker, 2, true);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 2);
	EXPECT_EQ(Tracker.Count(2), 1);
}

TEST(ClientDrownBoard, ChangingDdraceTeamStartsNewCount)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, true, false, 2);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false, false, 2);
	Observe(Tracker, 1, false, true, 2);
	Observe(Tracker, 1, true, false, 2);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, EnteringAndStayingOnStartPreservesCount)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	ASSERT_EQ(Tracker.Count(1), 1);
	ObserveOnStart(Tracker, 1);
	EXPECT_EQ(Tracker.Count(1), 1);
	ObserveOnStart(Tracker, 1);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, LeavingStartClearsOnlyThatPlayersCount)
{
	for(int Team : {static_cast<int>(TEAM_FLOCK), 1})
	{
		SCOPED_TRACE(Team);
		CTClientDrownBoard Tracker;
		Observe(Tracker, 1, false, false, Team);
		Observe(Tracker, 2, false, false, Team);
		Observe(Tracker, 1, true, false, Team);
		Observe(Tracker, 2, true, false, Team);
		ASSERT_EQ(Tracker.Count(1), 1);
		ASSERT_EQ(Tracker.Count(2), 1);
		ObserveOnStart(Tracker, 1, Team);
		Observe(Tracker, 1, false, false, Team);
		EXPECT_EQ(Tracker.Count(1), 0);
		EXPECT_EQ(Tracker.Count(2), 1);
	}
}

TEST(ClientDrownBoard, FirstFallAfterLeavingStartCountsWithoutNewHook)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	ObserveOnStart(Tracker, 1);
	Observe(Tracker, 1, false);
	ASSERT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 2);
}

TEST(ClientDrownBoard, LeavingStartWhileFrozenEstablishesNewBaseline)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	ASSERT_EQ(Tracker.Count(1), 1);
	ObserveOnStart(Tracker, 1);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, ReturningToStartClearsCountOnlyOnNextDeparture)
{
	CTClientDrownBoard Tracker;
	ObserveOnStart(Tracker, 1);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	ASSERT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	ObserveOnStart(Tracker, 1);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, false);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, MissingCharacterDoesNotImplyLeavingStart)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	ObserveOnStart(Tracker, 1);
	Tracker.Observe(1, true, "player", "clan", 1, false, false, false, false, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, false);
	EXPECT_EQ(Tracker.Count(1), 1);
	ObserveOnStart(Tracker, 1);
	Observe(Tracker, 1, false);
	EXPECT_EQ(Tracker.Count(1), 0);
}

TEST(ClientDrownBoard, ReusedClientIdCannotInheritDepartedPlayersCount)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Tracker.Observe(1, false, "player", "clan", 1, false, false, false, false, true);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 0);
}

TEST(ClientDrownBoard, IdentityChangeCannotInheritPreviousCount)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Tracker.Observe(1, true, "replacement", "clan", 1, true, true, false, false, true);
	EXPECT_EQ(Tracker.Count(1), 0);
}

TEST(ClientDrownBoard, MissingCharacterKeepsCountAndRebuildsFreezeBaseline)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Tracker.Observe(1, true, "player", "clan", 1, false, false, false, false, true);
	EXPECT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, true);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, ResetClearsCountsAndFirstObservationState)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	Tracker.Reset();
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 1);
}

TEST(ClientDrownBoard, BoardHidesOutsideGoresOrWithoutLocalPlayer)
{
	for(bool IncludeTeamZero : {false, true})
	{
		SCOPED_TRACE(IncludeTeamZero);
		EXPECT_FALSE(CTClientDrownBoard::IsBoardVisible(false, true, 1, IncludeTeamZero));
		EXPECT_FALSE(CTClientDrownBoard::IsBoardVisible(false, true, TEAM_FLOCK, IncludeTeamZero));
		EXPECT_FALSE(CTClientDrownBoard::IsBoardVisible(true, false, 1, IncludeTeamZero));
		EXPECT_FALSE(CTClientDrownBoard::IsBoardVisible(true, true, -1, IncludeTeamZero));
		EXPECT_FALSE(CTClientDrownBoard::IsBoardVisible(true, true, NUM_DDRACE_TEAMS, IncludeTeamZero));
		EXPECT_TRUE(CTClientDrownBoard::IsBoardVisible(true, true, 1, IncludeTeamZero));
		EXPECT_TRUE(CTClientDrownBoard::IsBoardVisible(true, true, NUM_DDRACE_TEAMS - 1, IncludeTeamZero));
	}
}

TEST(ClientDrownBoard, InvalidTeamsCannotCountOrMatchWithEitherTeamZeroPolicy)
{
	for(bool IncludeTeamZero : {false, true})
	{
		SCOPED_TRACE(IncludeTeamZero);
		for(int Team : {-1, static_cast<int>(NUM_DDRACE_TEAMS)})
		{
			SCOPED_TRACE(Team);
			CTClientDrownBoard Tracker;
			Observe(Tracker, 1, false, false, Team, IncludeTeamZero);
			Observe(Tracker, 1, true, false, Team, IncludeTeamZero);
			EXPECT_EQ(Tracker.Count(1), 0);
			EXPECT_FALSE(CTClientDrownBoard::IsSameTrackedTeam(Team, Team, IncludeTeamZero));
		}
	}
}

TEST(ClientDrownBoard, MovingBetweenTeamZeroAndNonZeroStartsNewBaseline)
{
	CTClientDrownBoard Tracker;
	Observe(Tracker, 1, false, false, TEAM_FLOCK);
	Observe(Tracker, 1, true, false, TEAM_FLOCK);
	ASSERT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, true);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false);
	Observe(Tracker, 1, false, true);
	Observe(Tracker, 1, true);
	ASSERT_EQ(Tracker.Count(1), 1);
	Observe(Tracker, 1, true, false, TEAM_FLOCK);
	EXPECT_EQ(Tracker.Count(1), 0);
	Observe(Tracker, 1, false, false, TEAM_FLOCK);
	Observe(Tracker, 1, false, true, TEAM_FLOCK);
	Observe(Tracker, 1, true, false, TEAM_FLOCK);
	EXPECT_EQ(Tracker.Count(1), 1);
}
