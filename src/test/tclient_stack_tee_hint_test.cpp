#include <engine/shared/config.h>

#include <game/client/components/tclient/stack_tee_hint.h>

#include <gtest/gtest.h>

#include <vector>

using tclient_stack_tee::CStackTeeHintCharacter;

TEST(ClientStackTeeHint, DisabledByDefault)
{
	EXPECT_EQ(DefaultConfig::MeStackTeeHint, 0);
}

TEST(ClientStackTeeHint, CustomBindTogglesHintBothDirections)
{
	EXPECT_STREQ(tclient_stack_tee::BIND_COMMAND, "toggle me_stack_tee_hint 0 1");
}

TEST(ClientStackTeeHint, MarksSameTeamFrozenTeeBelowAlignedLocalTee)
{
	const CStackTeeHintCharacter Local(true, false, 1, 100, 200.0f);
	const CStackTeeHintCharacter Target(true, true, 2, 100, 260.0f);
	EXPECT_TRUE(tclient_stack_tee::ShouldShow(true, Local, Target, [] { return true; }));
}

TEST(ClientStackTeeHint, DisabledFrozenOrInactiveTeeDoesNotMark)
{
	const CStackTeeHintCharacter Local(true, false, 1, 100, 200.0f);
	const CStackTeeHintCharacter Target(true, true, 2, 100, 260.0f);
	const auto ClearDrop = [] { return true; };
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(false, Local, Target, ClearDrop));
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, CStackTeeHintCharacter(true, true, 1, 100, 200.0f), Target, ClearDrop));
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, CStackTeeHintCharacter(false, false, 1, 100, 200.0f), Target, ClearDrop));
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, Local, CStackTeeHintCharacter(true, false, 2, 100, 260.0f), ClearDrop));
}

TEST(ClientStackTeeHint, RequiresDistinctTargetAndAlignedX)
{
	const CStackTeeHintCharacter Local(true, false, 1, 100, 200.0f);
	const CStackTeeHintCharacter Target(true, true, 2, 100, 260.0f);
	const auto ClearDrop = [] { return true; };
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, Local, CStackTeeHintCharacter(true, true, 1, 100, 260.0f), ClearDrop));
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, Local, CStackTeeHintCharacter(true, true, 2, 101, 260.0f), ClearDrop));
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, CStackTeeHintCharacter(true, false, 2, 100, 260.0f), Local, ClearDrop));
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, CStackTeeHintCharacter(true, false, 1, 100, 260.0f), Target, ClearDrop));
}

TEST(ClientStackTeeHint, RequiresClearDropPath)
{
	const CStackTeeHintCharacter Local(true, false, 1, 100, 200.0f);
	const CStackTeeHintCharacter Target(true, true, 2, 100, 260.0f);
	int PathChecks = 0;
	EXPECT_FALSE(tclient_stack_tee::ShouldShow(true, Local, Target, [&] { ++PathChecks; return false; }));
	EXPECT_EQ(PathChecks, 1);
	EXPECT_TRUE(tclient_stack_tee::ShouldShow(true, Local, Target, [&] { ++PathChecks; return true; }));
	EXPECT_EQ(PathChecks, 2);
}

TEST(ClientStackTeeHint, DropPathSamplesTheTeeCenterAndChecksItsFullBox)
{
	std::vector<vec2> Checked;
	// A tee center 60 units above another stops after falling to targetY - diameter.
	EXPECT_TRUE(tclient_stack_tee::ClearDropPath(vec2(100.0f, 100.0f), vec2(100.0f, 132.0f),
		[&](vec2 Point) { Checked.push_back(Point); return true; }));
	EXPECT_EQ(Checked.size(), 5);
	EXPECT_FLOAT_EQ(Checked.front().x, 100.0f);
	EXPECT_FLOAT_EQ(Checked[1].y, 108.0f);
	EXPECT_FLOAT_EQ(Checked.back().x, 100.0f);

	// TestBox uses the full body size around each center sample.
	EXPECT_FALSE(tclient_stack_tee::ClearDropPath(vec2(100.0f, 100.0f), vec2(100.0f, 132.0f),
		[](vec2 Point) { return !(Point.x + 14.0f > 112.0f && Point.y >= 108.0f); }));
	int AlreadyStackedSamples = 0;
	EXPECT_TRUE(tclient_stack_tee::ClearDropPath(vec2(100.0f, 100.0f), vec2(100.0f, 90.0f),
		[&](vec2) { ++AlreadyStackedSamples; return false; }));
	EXPECT_EQ(AlreadyStackedSamples, 0);
}

TEST(ClientStackTeeHint, FreezeTilesDoNotInterruptDropButDeathTilesDo)
{
	EXPECT_FALSE(tclient_stack_tee::TileStopsDrop(TILE_FREEZE));
	EXPECT_FALSE(tclient_stack_tee::TileStopsDrop(TILE_DFREEZE));
	EXPECT_FALSE(tclient_stack_tee::TileStopsDrop(TILE_LFREEZE));
	EXPECT_TRUE(tclient_stack_tee::TileStopsDrop(TILE_DEATH));
	EXPECT_FALSE(tclient_stack_tee::TileStopsDrop(TILE_NOHOOK));
}

TEST(ClientStackTeeHint, ThirdTeeInsidePlayerCollisionRadiusBlocksDropPath)
{
	const vec2 Start(100.0f, 100.0f);
	const vec2 End(100.0f, 200.0f);
	EXPECT_FALSE(tclient_stack_tee::DropPathClearOfTee(Start, End, vec2(128.0f, 150.0f), 35.0f));
	EXPECT_TRUE(tclient_stack_tee::DropPathClearOfTee(Start, End, vec2(135.0f, 150.0f), 35.0f));
	EXPECT_TRUE(tclient_stack_tee::DropPathClearOfTee(Start, End, vec2(100.0f, 236.0f), 35.0f));
}

TEST(ClientStackTeeHint, PlayerCollisionMatchesCharacterCoreGates)
{
	const tclient_stack_tee::CStackTeeCollisionState Normal{false, false, false};
	const tclient_stack_tee::CStackTeeCollisionState Solo{false, true, false};
	const tclient_stack_tee::CStackTeeCollisionState Disabled{false, false, true};
	const tclient_stack_tee::CStackTeeCollisionState Super{true, true, true};
	EXPECT_TRUE(tclient_stack_tee::CanPlayersCollide(true, true, Normal, Normal));
	EXPECT_FALSE(tclient_stack_tee::CanPlayersCollide(false, true, Normal, Normal));
	EXPECT_FALSE(tclient_stack_tee::CanPlayersCollide(true, true, Normal, Solo));
	EXPECT_FALSE(tclient_stack_tee::CanPlayersCollide(true, true, Disabled, Normal));
	EXPECT_FALSE(tclient_stack_tee::CanPlayersCollide(true, false, Normal, Normal));
	EXPECT_TRUE(tclient_stack_tee::CanPlayersCollide(true, false, Super, Solo));
}
