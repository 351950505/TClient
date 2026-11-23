#include <engine/shared/config.h>

#include <game/client/components/tclient/hook_tile_highlight.h>

#include <gtest/gtest.h>

#include <limits>

namespace
{
	using tclient_hook_tile::SSelection;
	bool Contains(const SSelection &Selection, int X, int Y)
	{
		for(int Index = 0; Index < Selection.m_Count; ++Index)
			if(Selection.m_aTiles[Index].m_X == X && Selection.m_aTiles[Index].m_Y == Y)
				return true;
		return false;
	}
}

TEST(ClientHookTile, VisualHighlightsAreDisabledByDefault)
{
	EXPECT_EQ(DefaultConfig::TcFreezeEntryHighlight, 0);
	EXPECT_EQ(DefaultConfig::TcHookTileHighlight, 0);
	EXPECT_EQ(DefaultConfig::TcHookTileHighlightPredict, 0);
	EXPECT_EQ(DefaultConfig::TcHookTileHighlightFreeze, 0);
	EXPECT_EQ(DefaultConfig::TcHookTileHighlightGlow, 0);
	EXPECT_EQ(DefaultConfig::TcHookTileHighlightRgb, 0);
	EXPECT_EQ(DefaultConfig::TcHookTileHighlightGradient, 0);
	EXPECT_EQ(DefaultConfig::TcIslandEnable, 0);
	EXPECT_EQ(DefaultConfig::TcDrownBoard, 0);
}

TEST(ClientHookTile, PredictionCacheKeyChangesForNextFrameAndTarget)
{
	using tclient_hook_tile::PredictionCacheKeyChanged;
	EXPECT_FALSE(PredictionCacheKeyChanged(12, 4, 12, 4));
	EXPECT_TRUE(PredictionCacheKeyChanged(12, 4, 13, 4));
	EXPECT_TRUE(PredictionCacheKeyChanged(12, 4, 12, 5));
	EXPECT_TRUE(PredictionCacheKeyChanged(UINT64_MAX - 1, 4, UINT64_MAX, 4));
}

TEST(ClientHookTile, FreezeHighlightRequiresEnabledPredictedUnhookedFrozenTarget)
{
	using tclient_hook_tile::FreezeHighlightMarked;
	EXPECT_FALSE(FreezeHighlightMarked(false, true, false, true));
	EXPECT_FALSE(FreezeHighlightMarked(false, false, true, true));
	EXPECT_FALSE(FreezeHighlightMarked(true, false, false, true));
	EXPECT_FALSE(FreezeHighlightMarked(true, false, true, false));
	EXPECT_TRUE(FreezeHighlightMarked(true, true, false, true));
	EXPECT_FALSE(FreezeHighlightMarked(true, false, true, true));
}

TEST(ClientHookTile, SegmentBoundsRejectDistantPlayersAndKeepConservativeCandidates)
{
	EXPECT_TRUE(tclient_hook_tile::SegmentBoundsMayIntersect(vec2(0.0f, 0.0f), vec2(50.0f, 0.0f), vec2(25.0f, 29.0f), 30.0f));
	EXPECT_TRUE(tclient_hook_tile::SegmentBoundsMayIntersect(vec2(0.0f, 0.0f), vec2(50.0f, 0.0f), vec2(79.0f, 0.0f), 30.0f));
	EXPECT_FALSE(tclient_hook_tile::SegmentBoundsMayIntersect(vec2(0.0f, 0.0f), vec2(50.0f, 0.0f), vec2(80.01f, 0.0f), 30.0f));
	EXPECT_TRUE(tclient_hook_tile::SegmentBoundsMayIntersect(vec2(50.0f, 0.0f), vec2(0.0f, 0.0f), vec2(25.0f, -30.0f), 30.0f));
}

TEST(ClientHookTile, SegmentBoundsNeverRejectAnExactSegmentHit)
{
	constexpr float Radius = 30.0f;
	const auto CheckSegment = [](vec2 Start, vec2 End) {
		for(float X = -40.0f; X <= 120.0f; X += 4.0f)
			for(float Y = -40.0f; Y <= 120.0f; Y += 4.0f)
			{
				const vec2 Point(X, Y);
				vec2 Closest;
				if(closest_point_on_line(Start, End, Point, Closest) && distance(Point, Closest) < Radius)
					EXPECT_TRUE(tclient_hook_tile::SegmentBoundsMayIntersect(Start, End, Point, Radius));
			}
	};
	CheckSegment(vec2(0.0f, 0.0f), vec2(64.0f, 0.0f));
	CheckSegment(vec2(64.0f, 0.0f), vec2(0.0f, 0.0f));
	CheckSegment(vec2(0.0f, 0.0f), vec2(64.0f, 64.0f));
	CheckSegment(vec2(64.0f, -16.0f), vec2(-8.0f, 72.0f));
}

TEST(ClientHookTile, HookedFrozenPredictionTargetIsNotHighlighted)
{
	using tclient_hook_tile::FreezeHighlightMarked;
	EXPECT_TRUE(FreezeHighlightMarked(true, true, false, true));
	EXPECT_FALSE(FreezeHighlightMarked(true, true, true, true));
}

TEST(ClientHookTile, HookedAndAliveTargetsLoseHighlightImmediately)
{
	using tclient_hook_tile::FreezeHighlightAlpha;
	using tclient_hook_tile::FreezeHighlightMarked;
	const bool FrozenTarget = FreezeHighlightMarked(true, true, false, true);
	const bool HookedTarget = FreezeHighlightMarked(true, true, true, true);
	const bool AliveTarget = FreezeHighlightMarked(true, true, false, false);
	EXPECT_FLOAT_EQ(FreezeHighlightAlpha(FrozenTarget), 1.0f);
	EXPECT_FLOAT_EQ(FreezeHighlightAlpha(HookedTarget), 0.0f);
	EXPECT_FLOAT_EQ(FreezeHighlightAlpha(AliveTarget), 0.0f);
}

TEST(ClientHookTile, InteriorSelectsOnlyOne)
{
	const auto Result = tclient_hook_tile::Select(vec2(48, 48), 4, 4, [](int, int) { return true; });
	ASSERT_EQ(Result.m_Count, 1);
	EXPECT_TRUE(Contains(Result, 1, 1));
}

TEST(ClientHookTile, VerticalSeamIncludesBothSides)
{
	for(float X : {31.0f, 32.0f})
	{
		const auto Result = tclient_hook_tile::Select(vec2(X, 48), 4, 4, [](int, int) { return true; });
		ASSERT_EQ(Result.m_Count, 2);
		EXPECT_TRUE(Contains(Result, 0, 1));
		EXPECT_TRUE(Contains(Result, 1, 1));
	}
}

TEST(ClientHookTile, HorizontalSeamIncludesBothSides)
{
	for(float Y : {31.0f, 32.0f})
	{
		const auto Result = tclient_hook_tile::Select(vec2(48, Y), 4, 4, [](int, int) { return true; });
		ASSERT_EQ(Result.m_Count, 2);
		EXPECT_TRUE(Contains(Result, 1, 0));
		EXPECT_TRUE(Contains(Result, 1, 1));
	}
}

TEST(ClientHookTile, NoHookAndAirCannotBecomeSeamNeighbors)
{
	const auto Result = tclient_hook_tile::Select(vec2(32, 48), 4, 4, [](int X, int Y) { return X == 1 && Y == 1; });
	ASSERT_EQ(Result.m_Count, 1);
	EXPECT_TRUE(Contains(Result, 1, 1));
}

TEST(ClientHookTile, IntegerEndpointJustOutsideWallFindsNearbySolid)
{
	const auto Result = tclient_hook_tile::Select(vec2(31, 48), 4, 4, [](int X, int Y) { return X == 1 && Y == 1; });
	ASSERT_EQ(Result.m_Count, 1);
	EXPECT_TRUE(Contains(Result, 1, 1));
}

TEST(ClientHookTile, AwayFromSeamDoesNotExpand)
{
	const auto Result = tclient_hook_tile::Select(vec2(34, 48), 4, 4, [](int, int) { return true; });
	EXPECT_EQ(Result.m_Count, 1);
}

TEST(ClientHookTile, FourTileCornerKeepsAtMostTwoSharingAnEdge)
{
	const auto Result = tclient_hook_tile::Select(vec2(32, 32), 4, 4, [](int, int) { return true; });
	ASSERT_EQ(Result.m_Count, 2);
	EXPECT_EQ(std::abs(Result.m_aTiles[0].m_X - Result.m_aTiles[1].m_X) +
			  std::abs(Result.m_aTiles[0].m_Y - Result.m_aTiles[1].m_Y),
		1);
}

TEST(ClientHookTile, DiagonalSolidsDoNotCountAsSharedSeam)
{
	const auto Result = tclient_hook_tile::Select(vec2(32, 32), 4, 4, [](int X, int Y) { return X == Y; });
	EXPECT_EQ(Result.m_Count, 1);
}

TEST(ClientHookTile, BoundariesNeverQueryOutsideMap)
{
	for(vec2 Pos : {vec2(0, 0), vec2(127, 127), vec2(0, 32), vec2(127, 32)})
	{
		const auto Result = tclient_hook_tile::Select(Pos, 4, 4, [](int X, int Y) {
			EXPECT_GE(X, 0);
			EXPECT_LT(X, 4);
			EXPECT_GE(Y, 0);
			EXPECT_LT(Y, 4);
			return true;
		});
		EXPECT_GE(Result.m_Count, 1);
		EXPECT_LE(Result.m_Count, 2);
	}
}

TEST(ClientHookTile, InvalidPointsAndEmptyMapReturnNothing)
{
	for(vec2 Pos : {vec2(-1, 32), vec2(128, 32), vec2(std::numeric_limits<float>::infinity(), 32), vec2(32, std::numeric_limits<float>::quiet_NaN())})
		EXPECT_EQ(tclient_hook_tile::Select(Pos, 4, 4, [](int, int) { ADD_FAILURE(); return true; }).m_Count, 0);
	EXPECT_EQ(tclient_hook_tile::Select(vec2(32, 32), 0, 0, [](int, int) { ADD_FAILURE(); return true; }).m_Count, 0);
	EXPECT_EQ(tclient_hook_tile::Select(vec2(32, 32), 4, 4, [](int, int) { return false; }).m_Count, 0);
}
