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

TEST(ClientHookTile, PredictionCacheKeyChangesForNextFrameAndTarget)
{
	using tclient_hook_tile::PredictionCacheKeyChanged;
	EXPECT_FALSE(PredictionCacheKeyChanged(12, 4, 12, 4));
	EXPECT_TRUE(PredictionCacheKeyChanged(12, 4, 13, 4));
	EXPECT_TRUE(PredictionCacheKeyChanged(12, 4, 12, 5));
	EXPECT_TRUE(PredictionCacheKeyChanged(UINT64_MAX - 1, 4, UINT64_MAX, 4));
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
