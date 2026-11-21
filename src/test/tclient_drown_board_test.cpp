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

// —— 多列统计（ObserveStats / FeedDeath，估算口径见 docs 落水榜多列语义 2026-11-19）——

namespace
{
	using CTickArray = std::array<CTClientDrownBoard::SDrownStatTick, MAX_CLIENTS>;

	CTClientDrownBoard::SDrownStatTick MakeStatTick(bool Frozen = false, bool OnStart = false, int HookState = HOOK_IDLE, int JumpedTotal = -1, float X = 0.0f, float Y = 0.0f, float HookX = 0.0f, float HookY = 0.0f, bool InFreezeTile = false, int FreezeEndTick = -1, int HookedPlayerId = -1)
	{
		CTClientDrownBoard::SDrownStatTick Tick;
		Tick.m_Active = true;
		Tick.m_HasCharacter = true;
		Tick.m_Frozen = Frozen;
		Tick.m_OnStart = OnStart;
		Tick.m_HookState = HookState;
		Tick.m_HookedPlayerId = HookedPlayerId;
		Tick.m_JumpedTotal = JumpedTotal;
		Tick.m_X = X;
		Tick.m_Y = Y;
		Tick.m_HookX = HookX;
		Tick.m_HookY = HookY;
		Tick.m_InFreezeTile = InFreezeTile;
		Tick.m_FreezeEndTick = FreezeEndTick;
		return Tick;
	}

	// 同时喂 Observe（冻结计数基线）与 StatTick 数组，模拟一个快照 tick。
	void FeedTick(CTClientDrownBoard &Tracker, CTickArray &aTicks, int ClientId, int Team, const CTClientDrownBoard::SDrownStatTick &Tick, bool IncludeTeamZero = true)
	{
		aTicks[ClientId] = Tick;
		Tracker.Observe(ClientId, true, "player", "clan", Team, true, Tick.m_Frozen,
			Tick.m_HookState == HOOK_FLYING || Tick.m_HookState == HOOK_GRABBED, Tick.m_OnStart, IncludeTeamZero);
	}

	void RunStats(CTClientDrownBoard &Tracker, const CTickArray &aTicks, int LocalTeam = 1, bool IncludeTeamZero = true, bool ResetOnWipe = false, int GameTick = 100)
	{
		Tracker.ObserveStats(aTicks.data(), LocalTeam, IncludeTeamZero, ResetOnWipe, 1, GameTick);
	}
}

TEST(ClientDrownBoard, DeathsCountPerKillAndClearOnNewRound)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	Tracker.FeedDeath(1);
	Tracker.FeedDeath(1);
	EXPECT_EQ(Tracker.Stats(1).m_Deaths, 2);
	// 回到起点再离开 = 新一轮，全部计数清零。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, true));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Deaths, 0);
}

TEST(ClientDrownBoard, HookLaunchesAndPlayerHooks)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	// 队友站在 (100,100)：出钩 +1（钩尖指向队友）。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_FLYING, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_HooksLaunched, 1);
	EXPECT_EQ(Tracker.Stats(1).m_PlayerHooks, 0);
	// 钩住瞬间且钩尖在队友位置 → 钩人 +1。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_PlayerHooks, 1);
	// 持续 GRABBED 不重复计钩人。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_PlayerHooks, 1);
}

TEST(ClientDrownBoard, JumpsFollowCumulativeCounterAndRespawnResetsBaseline)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 0));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 3));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Jumps, 3);
	// 重生后累计值回退：只重建基线，不把回退当跳。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 0));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Jumps, 3);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 5));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Jumps, 8);
}

TEST(ClientDrownBoard, SaveWaitsUntilRescuedTeeBecomesActive)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
	RunStats(Tracker, aTicks, 1, true, false, 99);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 1);
	EXPECT_EQ(Tracker.Stats(2).m_Saves, 0);
}

TEST(ClientDrownBoard, SaveDoesNotTransferToAnotherFrozenTeammate)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f));
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 500.0f, 100.0f));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 500.0f, 100.0f));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 1);
}

TEST(ClientDrownBoard, HookAtThawDoesNotImmediatelyConfirmSave)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
	RunStats(Tracker, aTicks, 1, true, false, 99);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 1);
}

TEST(ClientDrownBoard, NaturalThawInsideFreezeTileDoesNotCount)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 100));
	RunStats(Tracker, aTicks, 1, true, false, 99);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 100));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
}

TEST(ClientDrownBoard, DeepFreezeRescueOnFreezeTileCounts)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, -1));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, -1));
	RunStats(Tracker, aTicks, 1, true, false, 102);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 103);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 1);
}

TEST(ClientDrownBoard, HookerRecordedWhenVictimFreezesOnSameTick)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 1));
	RunStats(Tracker, aTicks, 1, true, false, 98);

	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 1));
	RunStats(Tracker, aTicks, 1, true, false, 99);

	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 1));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	EXPECT_EQ(Tracker.Stats(2).m_Saves, 1);
}

TEST(ClientDrownBoard, LastHookerDuringFreezeGetsCredit)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
	RunStats(Tracker, aTicks, 1, true, false, 99);
		FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 3));
		RunStats(Tracker, aTicks, 1, true, false, 100);
		// Rescuer 1 keeps holding; rescuer 2 grabs later and becomes the last hooker.
		FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 3));
		FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 150.0f, 50.0f, 100.0f, 100.0f, false, -1, 3));
		RunStats(Tracker, aTicks, 1, true, false, 101);
		FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 3));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick()); // rescuer 2 releases before thaw
	RunStats(Tracker, aTicks, 1, true, false, 102);
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 103);
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 104);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	EXPECT_EQ(Tracker.Stats(2).m_Saves, 1);
}

TEST(ClientDrownBoard, HookOnDifferentTeeDoesNotCreditNearbyFrozenBystander)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 110.0f, 100.0f, 0.0f, 0.0f, true, 200));
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 99);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 3));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 110.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 142.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 101);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
}

TEST(ClientDrownBoard, DeadRescuerCannotReattributeFromStaleGrab)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
	RunStats(Tracker, aTicks, 1, true, false, 99);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	Tracker.FeedDeath(1);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	RunStats(Tracker, aTicks, 1, true, false, 101);
	// A stale idle-to-grabbed snapshot arrives after the death message while the
	// victim is still frozen and in range.
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 102);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 102);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 103);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
}

TEST(ClientDrownBoard, RescuerEligibilityReturnsOnlyAfterCharacterRespawns)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
	RunStats(Tracker, aTicks, 1, true, false, 99);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 100);
	Tracker.FeedDeath(1);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	RunStats(Tracker, aTicks, 1, true, false, 101);

	CTClientDrownBoard::SDrownStatTick Missing = MakeStatTick();
	Missing.m_HasCharacter = false;
	Tracker.Observe(1, true, "player", "clan", 1, false, false, false, false, true);
	aTicks[1] = Missing;
	RunStats(Tracker, aTicks, 1, true, false, 102);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	RunStats(Tracker, aTicks, 1, true, false, 103);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
	RunStats(Tracker, aTicks, 1, true, false, 104);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
	RunStats(Tracker, aTicks, 1, true, false, 104);
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
	RunStats(Tracker, aTicks, 1, true, false, 105);
	EXPECT_EQ(Tracker.Stats(1).m_Saves, 1);
}

TEST(ClientDrownBoard, SlotDisconnectOrIdentityChangeClearsHookAttribution)
{
	for(bool ChangeIdentity : {false, true})
	{
		SCOPED_TRACE(ChangeIdentity);
		CTClientDrownBoard Tracker;
		CTickArray aTicks{};
		FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
		FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, true, 200));
		RunStats(Tracker, aTicks, 1, true, false, 99);
		FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_GRABBED, -1, 50.0f, 50.0f, 100.0f, 100.0f, false, -1, 2));
		RunStats(Tracker, aTicks, 1, true, false, 100);
		if(ChangeIdentity)
			Tracker.Observe(1, true, "replacement", "clan", 1, false, false, false, false, true);
		else
			Tracker.Observe(1, false, "player", "clan", 1, false, false, false, false, true);
		// Reuse the slot with its original identity before thaw, so later eligibility
		// checks cannot hide a stale frozen-hooker reference.
		FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
		FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 100.0f, 100.0f, 0.0f, 0.0f, false, 200));
		RunStats(Tracker, aTicks, 1, true, false, 101);
		FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 132.0f, 100.0f));
		RunStats(Tracker, aTicks, 1, true, false, 102);
		EXPECT_EQ(Tracker.Stats(1).m_Saves, 0);
	}
}

TEST(ClientDrownBoard, TeamZeroDeathResetsWhenLocalTeamIsDifferent)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, TEAM_FLOCK, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, TEAM_FLOCK, MakeStatTick(true));
	RunStats(Tracker, aTicks, 1, true);
	ASSERT_EQ(Tracker.Stats(2).m_Freezes, 1);
	Tracker.FeedDeath(2);
	FeedTick(Tracker, aTicks, 2, TEAM_FLOCK, MakeStatTick(false, true)); // stale pre-death snapshot
	RunStats(Tracker, aTicks, 1, true);
	EXPECT_EQ(Tracker.Stats(2).m_Freezes, 1);
	CTClientDrownBoard::SDrownStatTick Missing = MakeStatTick();
	Missing.m_HasCharacter = false;
	Tracker.Observe(2, true, "player", "clan", TEAM_FLOCK, false, false, false, false, true);
	aTicks[2] = Missing;
	RunStats(Tracker, aTicks, 1, true);
	FeedTick(Tracker, aTicks, 2, TEAM_FLOCK, MakeStatTick(false, true));
	RunStats(Tracker, aTicks, 1, true);
	EXPECT_EQ(Tracker.Stats(2).m_Freezes, 0);
}

TEST(ClientDrownBoard, LastsAndAledsPerTeamEpisode)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 10.0f * ClientId, 0.0f));
	RunStats(Tracker, aTicks);
	// 两人冻结：剩下的那名玩家记一次坚持。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 20.0f, 0.0f));
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 30.0f, 0.0f));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(3).m_Lasts, 1);
	// 持续未解冻不重复计数。
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(3).m_Lasts, 1);
	// 最后一人也冻结 → 全灭，三人各 +1。
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 30.0f, 0.0f));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(2).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(3).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(3).m_Lasts, 1);
	// 有人解冻后再次冻结仍属于同一轮，全队 K 掉前不重复计全灭。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 20.0f, 0.0f));
	FeedTick(Tracker, aTicks, 3, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 30.0f, 0.0f));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(2).m_Lasts, 0);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	Tracker.FeedDeath(1);
	Tracker.FeedDeath(2);
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	Tracker.FeedDeath(3);
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 2);
}

TEST(ClientDrownBoard, DisconnectedMemberDoesNotStallWipeResetAndWait)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 10.0f * ClientId, 0.0f));
	RunStats(Tracker, aTicks, 1, true, true);
	// 全队冻结 → 全灭一轮。
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f * ClientId, 0.0f));
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	// 成员 1、2 相继死亡，成员 3 不死直接离线（快照无角色）。
	Tracker.FeedDeath(1);
	Tracker.FeedDeath(2);
	CTClientDrownBoard::SDrownStatTick Offline;
	Offline.m_Active = false;
	FeedTick(Tracker, aTicks, 3, 1, Offline);
	RunStats(Tracker, aTicks, 1, true, true);
	// 离线成员不阻塞本轮重置：计数继续而不是永久暂停。
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 0);
	// 复活解冻等待也只看仍在服的成员：1、2 复活解冻即恢复计数。
	for(int ClientId = 1; ClientId <= 2; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 10.0f * ClientId, 0.0f));
	RunStats(Tracker, aTicks, 1, true, true);
	// 新一轮：剩下的 1、2 再次全队冻结 → 再次计全灭。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 20.0f, 0.0f));
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(2).m_Aleds, 1);
}

TEST(ClientDrownBoard, ResetOnWipeClearsAllCountersLikeNewRound)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick());
	RunStats(Tracker, aTicks, 1, true, true);
	Tracker.FeedDeath(1);
	ASSERT_EQ(Tracker.Stats(1).m_Deaths, 1);
	// 全员冻结时保留统计，单人 K 或重复 K 都不能提前重置。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(1).m_Deaths, 1);
	Tracker.FeedDeath(1);
	RunStats(Tracker, aTicks, 1, true, true);
	Tracker.FeedDeath(1);
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(2).m_Aleds, 1);
	Tracker.FeedDeath(2);
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Deaths, 0);
	EXPECT_EQ(Tracker.Stats(1).m_Freezes, 0);
	EXPECT_EQ(Tracker.Stats(2).m_Freezes, 0);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 0);
	EXPECT_EQ(Tracker.Stats(2).m_Aleds, 0);
}

TEST(ClientDrownBoard, TeamDeathWaitsForFreshUnfrozenSnapshot)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	for(int ClientId = 1; ClientId <= 2; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	for(int ClientId = 1; ClientId <= 2; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks);
	Tracker.FeedTeamDeath(2);
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	Tracker.FeedTeamDeath(1);
	RunStats(Tracker, aTicks);
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	for(int ClientId = 1; ClientId <= 2; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	for(int ClientId = 1; ClientId <= 2; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 2);
}

TEST(ClientDrownBoard, MissingMemberCannotBeCountedAsDead)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick());
	RunStats(Tracker, aTicks, 1, true, true);
	for(int ClientId = 1; ClientId <= 3; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks, 1, true, true);
	Tracker.FeedDeath(1);
	Tracker.FeedDeath(2);
	aTicks[3].m_HasCharacter = false;
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 1);
	EXPECT_EQ(Tracker.Stats(2).m_Aleds, 1);
	Tracker.FeedDeath(3);
	RunStats(Tracker, aTicks, 1, true, true);
	EXPECT_EQ(Tracker.Stats(1).m_Aleds, 0);
}

TEST(ClientDrownBoard, TickTimeAccumulatesWhileCharacterPresent)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_TicksAlive, 1);
	EXPECT_EQ(Tracker.Stats(1).m_TicksFrozen, 2);
}

TEST(ClientDrownBoard, TimeUsesElapsedServerTicks)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick());
	Tracker.ObserveStats(aTicks.data(), 1, true, false, 2);
	Tracker.ObserveStats(aTicks.data(), 1, true, false, 2);
	EXPECT_EQ(Tracker.Stats(1).m_TicksAlive, 4);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true));
	Tracker.ObserveStats(aTicks.data(), 1, true, false, 3);
	EXPECT_EQ(Tracker.Stats(1).m_TicksFrozen, 3);
	Tracker.ObserveStats(aTicks.data(), 1, true, false, 0);
	EXPECT_EQ(Tracker.Stats(1).m_TicksFrozen, 3);
}

TEST(ClientDrownBoard, NewRoundClearsJumpBaseline)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 5));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, true, HOOK_IDLE, 5));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 8));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Jumps, 0);
}

TEST(ClientDrownBoard, MissingCharacterClearsJumpBaseline)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 2));
	RunStats(Tracker, aTicks);
	auto Missing = MakeStatTick();
	Missing.m_HasCharacter = false;
	FeedTick(Tracker, aTicks, 1, 1, Missing);
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 8));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Jumps, 0);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(false, false, HOOK_IDLE, 9));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(1).m_Jumps, 1);
}

TEST(ClientDrownBoard, LocalTeamChangeRestartsEpisodesOnly)
{
	CTClientDrownBoard Tracker;
	CTickArray aTicks{};
	for(int ClientId = 1; ClientId <= 2; ++ClientId)
		FeedTick(Tracker, aTicks, ClientId, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 10.0f * ClientId, 0.0f));
	RunStats(Tracker, aTicks);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	RunStats(Tracker, aTicks);
	EXPECT_EQ(Tracker.Stats(2).m_Lasts, 1);
	// 本地换队：轮次状态重来，但已计入的数字不变（队伍 1 的玩家不再产生新轮次）。
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	FeedTick(Tracker, aTicks, 2, 1, MakeStatTick(false, false, HOOK_IDLE, -1, 20.0f, 0.0f));
	RunStats(Tracker, aTicks, 2);
	EXPECT_EQ(Tracker.Stats(2).m_Lasts, 1);
	FeedTick(Tracker, aTicks, 1, 1, MakeStatTick(true, false, HOOK_IDLE, -1, 10.0f, 0.0f));
	RunStats(Tracker, aTicks, 2);
	EXPECT_EQ(Tracker.Stats(1).m_Lasts, 0);
	EXPECT_EQ(Tracker.Stats(2).m_Lasts, 1);
}
