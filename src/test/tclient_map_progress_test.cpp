/*
 * Based on QmClient's src/test/unit/qmclient/client/qm_map_progress_test.cpp.
 * Adapted for TClient namespace/test names and extended for invalid inputs and repeated markers.
 * Source license: QmClient root license.txt (permissive license).
 *
 * Teeworlds Copyright (C) 2007-2014 Magnus Auvinen
 * DDRace    Copyright (C) 2010-2011 Shereef Marzouk
 * DDNet     Copyright (C)           Dennis Felsing
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */
#include <game/client/components/tclient/map_progress.h>

#include <gtest/gtest.h>
#include <test/test.h>

#include <array>
#include <vector>

namespace
{
	TClientMapProgress::CMap MakeProgressLine(int Width)
	{
		TClientMapProgress::CMap Map(Width, 1);
		for(int Index = 0; Index < Width; ++Index)
			Map.SetTile(Index, TClientMapProgress::MakeTile(Index == 0 ? TILE_START : (Index == Width - 1 ? TILE_FINISH : TILE_AIR)));
		return Map;
	}

	TClientMapProgress::SEstimate MoveProgressPlayer(TClientMapProgress::CPlayer &Player, const TClientMapProgress::CMap &Map, int Index, int TeleCheckpoint = 0, bool UseTimeCheckpoints = true)
	{
		Player.Observe(Map.Tile(Index), Index);
		// 小地图也分多次推进，以覆盖换段和回传后重建距离场的行为。
		for(int Step = 0; Step < 128; ++Step)
			Player.Update(Map, Index, TeleCheckpoint, 128, UseTimeCheckpoints);
		return Player.Estimate();
	}
}

TEST(TClientMapProgressTarget, ExplicitSpectatorTargetTakesPriorityOverLocalPlayer)
{
	EXPECT_EQ(TClientMapProgress::ResolveClientId(3, true, 17), 17);
	EXPECT_EQ(TClientMapProgress::ResolveClientId(-1, true, 17), 17);
	EXPECT_EQ(TClientMapProgress::ResolveClientId(3, true, 18), 18);
}

TEST(TClientMapProgressTarget, FreeviewAndInvalidTargetsKeepTheLocalPlayer)
{
	for(const int SpectatorId : std::array<int, 3>{-1, MAX_CLIENTS, MAX_CLIENTS + 1})
	{
		SCOPED_TRACE(SpectatorId);
		EXPECT_EQ(TClientMapProgress::ResolveClientId(3, true, SpectatorId), 3);
	}
	EXPECT_EQ(TClientMapProgress::ResolveClientId(3, false, 17), 3);
}

TEST(TClientMapProgressPosition, FullCharacterTakesPriorityOverSpectatorCoordinates)
{
	const auto Position = TClientMapProgress::ResolvePosition(vec2(16, 32), vec2(48, 64), vec2(80, 96));
	ASSERT_TRUE(Position);
	EXPECT_EQ(*Position, vec2(16, 32));
}

TEST(TClientMapProgressPosition, MissingFullCharacterUsesSpecCharacter)
{
	const auto Position = TClientMapProgress::ResolvePosition(std::nullopt, vec2(48, 64), vec2(80, 96));
	ASSERT_TRUE(Position);
	EXPECT_EQ(*Position, vec2(48, 64));
}

TEST(TClientMapProgressPosition, SpectatorCoordinatesRemainUsableWithoutCharacterSnapshots)
{
	const auto Position = TClientMapProgress::ResolvePosition(std::nullopt, std::nullopt, vec2(0, 0));
	ASSERT_TRUE(Position);
	EXPECT_EQ(*Position, vec2(0, 0));
}

TEST(TClientMapProgressPosition, MissingAllSourcesStaysUnknown)
{
	EXPECT_FALSE(TClientMapProgress::ResolvePosition(std::nullopt, std::nullopt, std::nullopt));
}

TEST(TClientMapProgressPosition, SpectatorCoordinatesDriveProgressAndRouteFromTheSameTile)
{
	auto Map = MakeProgressLine(11);
	Map.Finalize();
	const auto Position = TClientMapProgress::ResolvePosition(std::nullopt, std::nullopt, vec2(6 * 32 + 16, 16));
	ASSERT_TRUE(Position);
	TClientMapProgress::CPlayer Player;
	const auto Estimate = MoveProgressPlayer(Player, Map, (int)(Position->x / 32), 0, false);
	ASSERT_TRUE(Estimate.m_Valid);
	EXPECT_FLOAT_EQ(Estimate.m_Progress, 0.6f);
	std::vector<int> vRoute;
	ASSERT_TRUE(Player.BuildRoute(vRoute));
	ASSERT_FALSE(vRoute.empty());
	EXPECT_EQ(vRoute.front(), 6);
	EXPECT_EQ(vRoute.back(), 10);
}

TEST(TClientMapProgressDistance, MidRunObservationUsesRemainingDistanceAndCanMoveBackwards)
{
	const auto MidRun = TClientMapProgress::EstimateDistance(40, 100);
	ASSERT_TRUE(MidRun.m_Valid);
	EXPECT_FLOAT_EQ(MidRun.m_Progress, 0.6f);
	EXPECT_FLOAT_EQ(TClientMapProgress::EstimateDistance(70, 100).m_Progress, 0.3f);
	EXPECT_FLOAT_EQ(TClientMapProgress::EstimateDistance(0, 100).m_Progress, 1.0f);
	EXPECT_FLOAT_EQ(TClientMapProgress::EstimateDistance(100, 100).m_Progress, 0.0f);
	EXPECT_FLOAT_EQ(TClientMapProgress::EstimateDistance(150, 100).m_Progress, 0.0f);
}

TEST(TClientMapProgressDistance, MissingRouteOrStartDistanceStaysUnknown)
{
	EXPECT_FALSE(TClientMapProgress::EstimateDistance(TClientMapProgress::INF, 100).m_Valid);
	EXPECT_FALSE(TClientMapProgress::EstimateDistance(-1, 100).m_Valid);
	EXPECT_FALSE(TClientMapProgress::EstimateDistance(40, -1).m_Valid);
	EXPECT_FALSE(TClientMapProgress::EstimateDistance(40, 0).m_Valid);
	EXPECT_FALSE(TClientMapProgress::EstimateDistance(40, TClientMapProgress::INF).m_Valid);
}

TEST(TClientMapProgress, MapWithoutFinishIsNotReadyAndKeepsEstimateUnknown)
{
	TClientMapProgress::CMap Map(5, 1);
	for(int Index = 0; Index < Map.Size(); ++Index)
		Map.SetTile(Index, TClientMapProgress::MakeTile(Index == 0 ? TILE_START : TILE_AIR));
	Map.Finalize();
	EXPECT_FALSE(Map.Ready());

	TClientMapProgress::CPlayer Player;
	Player.Observe(Map.Tile(2), 2);
	Player.Update(Map, 2, 0, 128, false);
	EXPECT_FALSE(Player.Estimate().m_Valid);
}

TEST(TClientMapProgress, StartFinishAndBacktrackingClampProgressToTheMap)
{
	auto Map = MakeProgressLine(11);
	for(int Index = 4; Index <= 6; ++Index)
		Map.SetTile(Index, TClientMapProgress::MakeTile(TILE_FREEZE));
	Map.Finalize();
	TClientMapProgress::CPlayer Player;
	EXPECT_FLOAT_EQ(MoveProgressPlayer(Player, Map, 0).m_Progress, 0.0f);
	const auto InWater = MoveProgressPlayer(Player, Map, 5);
	ASSERT_TRUE(InWater.m_Valid);
	EXPECT_FLOAT_EQ(InWater.m_Progress, 0.5f);
	EXPECT_FLOAT_EQ(MoveProgressPlayer(Player, Map, 7).m_Progress, 0.7f);
	EXPECT_FLOAT_EQ(MoveProgressPlayer(Player, Map, 3).m_Progress, 0.3f);
	EXPECT_FLOAT_EQ(MoveProgressPlayer(Player, Map, 10).m_Progress, 1.0f);
	EXPECT_FLOAT_EQ(MoveProgressPlayer(Player, Map, 0).m_Progress, 0.0f);
}

TEST(TClientMapProgress, RepeatedStartAndFinishMarkersAreCollectedOncePerTile)
{
	TClientMapProgress::CMap Map(7, 1);
	for(int Index = 0; Index < Map.Size(); ++Index)
		Map.SetTile(Index, TClientMapProgress::MakeTile(TILE_AIR));
	Map.SetTile(0, TClientMapProgress::MakeTile(TILE_START, TILE_START));
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_START, TILE_START));
	Map.SetTile(4, TClientMapProgress::MakeTile(TILE_FINISH, TILE_FINISH));
	Map.SetTile(6, TClientMapProgress::MakeTile(TILE_FINISH, TILE_FINISH));
	Map.Finalize();

	ASSERT_TRUE(Map.Ready());
	EXPECT_EQ(Map.Sources(0).size(), 2U);
	EXPECT_EQ(Map.Targets(-1).size(), 2U);

	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Source(Map.Sources(-1), -1), 2);
	std::vector<int> vRoute;
	ASSERT_TRUE(Field.BuildRoute(2, vRoute));
	ASSERT_FALSE(vRoute.empty());
	EXPECT_EQ(vRoute.front(), 2);
	EXPECT_EQ(vRoute.back(), 4);
}

TEST(TClientMapProgress, UnknownTileIdsRemainUnmarkedAndTraversable)
{
	auto Map = MakeProgressLine(5);
	const auto Unknown = TClientMapProgress::MakeTile(-1, -1);
	EXPECT_FALSE(Unknown.m_Start);
	EXPECT_FALSE(Unknown.m_Finish);
	EXPECT_FALSE(Unknown.Blocked());
	Map.SetTile(2, Unknown);
	Map.Finalize();

	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Length(0), 4);
}

TEST(TClientMapProgress, InvalidMapDimensionsAndPlayerIndexesStayUnknown)
{
	for(const auto [Width, Height] : {std::pair{0, 2}, std::pair{-1, 2}, std::pair{2, -1}})
	{
		TClientMapProgress::CMap InvalidMap(Width, Height);
		InvalidMap.Finalize();
		EXPECT_EQ(InvalidMap.Size(), 0);
		EXPECT_FALSE(InvalidMap.Ready());
	}

	auto Map = MakeProgressLine(5);
	Map.Finalize();
	TClientMapProgress::CPlayer Player;
	Player.Observe(Map.Tile(0), 0);
	Player.Update(Map, 0, 0, 128, false);
	ASSERT_TRUE(Player.Estimate().m_Valid);
	Player.Update(Map, -1, 0, 128, false);
	EXPECT_FALSE(Player.Estimate().m_Valid);
	Player.Update(Map, Map.Size(), 0, 128, false);
	EXPECT_FALSE(Player.Estimate().m_Valid);
}

TEST(TClientMapProgress, ResourceBudgetKeepsTrackedMapsBoundedAndLargeMapsUnknown)
{
	EXPECT_TRUE(TClientMapProgress::IsMapSizeWithinProgressBudget(512, 512));
	EXPECT_TRUE(TClientMapProgress::IsMapSizeWithinProgressBudget(243100, 1));
	EXPECT_FALSE(TClientMapProgress::IsMapSizeWithinProgressBudget(513, 512));
	EXPECT_FALSE(TClientMapProgress::IsMapSizeWithinProgressBudget(5933, 2160));
	EXPECT_FALSE(TClientMapProgress::IsMapSizeWithinProgressBudget(0, 1));
	EXPECT_FALSE(TClientMapProgress::IsMapSizeWithinProgressBudget(-1, 512));

	TClientMapProgress::CMap RejectedMap(513, 512);
	EXPECT_EQ(RejectedMap.Size(), 0);
	RejectedMap.Finalize();
	EXPECT_FALSE(RejectedMap.Ready());
}

TEST(TClientMapProgress, VisualMetadataBudgetRejectsExtremeItemCounts)
{
	using namespace TClientMapProgress;
	EXPECT_TRUE(IsMapProgressVisualMetadataWithinBudget(32768, 4096, 4096, 16384));
	EXPECT_FALSE(IsMapProgressVisualMetadataWithinBudget(32769, 4096, 4096, 16384));
	EXPECT_FALSE(IsMapProgressVisualMetadataWithinBudget(32768, 4097, 4096, 16384));
	EXPECT_FALSE(IsMapProgressVisualMetadataWithinBudget(32768, 4096, 4097, 16384));
	EXPECT_FALSE(IsMapProgressVisualMetadataWithinBudget(32768, 4096, 4096, 16385));
	EXPECT_FALSE(IsMapProgressVisualMetadataWithinBudget(-1, 0, 0, 0));
}

TEST(TClientMapProgress, QueueBoundStopsGrowthInsteadOfReportingACompleteRoute)
{
	EXPECT_TRUE(TClientMapProgress::HasMapProgressQueueRoom(3, 4));
	EXPECT_FALSE(TClientMapProgress::HasMapProgressQueueRoom(4, 4));
	EXPECT_FALSE(TClientMapProgress::HasMapProgressQueueRoom(0, 0));

	auto Map = MakeProgressLine(5);
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	EXPECT_FALSE(Field.Failed());
	EXPECT_FALSE(Field.Complete());
}

TEST(TClientMapProgress, TeleportWorkBudgetFailsClosedAndPlayerStaysUnknown)
{
	TClientMapProgress::CMap Map(100, 1);
	Map.SetTile(0, TClientMapProgress::MakeTile(TILE_START));
	for(int Index = 1; Index <= 48; ++Index)
		Map.SetTile(Index, TClientMapProgress::MakeTile(ENTITY_OFFSET + ENTITY_SPAWN));
	for(int Index = 49; Index <= 96; ++Index)
		Map.SetTile(Index, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELECHECKIN, 0));
	Map.SetTile(97, TClientMapProgress::MakeTile(ENTITY_OFFSET + ENTITY_SPAWN));
	Map.SetTile(98, TClientMapProgress::MakeTile(TILE_AIR));
	Map.SetTile(99, TClientMapProgress::MakeTile(TILE_FINISH));
	Map.Finalize();
	ASSERT_TRUE(Map.Ready());

	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	Field.Step(Map, 10000);
	EXPECT_TRUE(Field.Failed());
	EXPECT_FALSE(Field.Complete());
	Field.Step(Map, 10000);
	EXPECT_TRUE(Field.Failed());
	EXPECT_FALSE(Field.Complete());

	TClientMapProgress::CPlayer Player;
	Player.Update(Map, 99, 0, 10000, false);
	EXPECT_TRUE(Player.Failed());
	EXPECT_FALSE(Player.Estimate().m_Valid);
	Player.Update(Map, 99, 0, 10000, false);
	EXPECT_FALSE(Player.Estimate().m_Valid);
}

TEST(TClientMapProgress, InvalidFieldStartLatchesFailureAndNeverReportsComplete)
{
	TClientMapProgress::CMap RejectedMap(513, 512);
	TClientMapProgress::CField Field;
	Field.Start(RejectedMap, 0);
	EXPECT_TRUE(Field.Failed());
	EXPECT_FALSE(Field.Complete());
	Field.Step(RejectedMap, 4096);
	EXPECT_TRUE(Field.Failed());
	EXPECT_FALSE(Field.Complete());
}

TEST(TClientMapProgress, DeepFreezeIsPassableButDeathAndSolidAreNot)
{
	for(const int Tile : {TILE_DFREEZE, TILE_LFREEZE, TILE_DEATH, TILE_SOLID, TILE_NOHOOK})
	{
		SCOPED_TRACE(Tile);
		auto Map = MakeProgressLine(5);
		Map.SetTile(2, TClientMapProgress::MakeTile(Tile));
		Map.Finalize();
		TClientMapProgress::CField Field;
		Field.Start(Map, 0);
		while(!Field.Complete())
			Field.Step(Map, 1);
		EXPECT_EQ(Field.Length(0), Tile == TILE_DFREEZE || Tile == TILE_LFREEZE ? 4 : -1);
	}
}

TEST(TClientMapProgress, SafeDetourWinsWithoutCountingItsPenaltyAsDistance)
{
	TClientMapProgress::CMap Map(5, 2);
	for(int Index = 0; Index < 10; ++Index)
		Map.SetTile(Index, TClientMapProgress::MakeTile(TILE_AIR));
	Map.SetTile(0, TClientMapProgress::MakeTile(TILE_START));
	Map.SetTile(4, TClientMapProgress::MakeTile(TILE_FINISH));
	Map.SetTile(1, TClientMapProgress::MakeTile(TILE_FREEZE));
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_FREEZE));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Length(0), 6);
	EXPECT_EQ(Field.Next(0), 5);
}

TEST(TClientMapProgress, OneWetTileLosesToAnEntirelyDryDetour)
{
	TClientMapProgress::CMap Map(3, 7);
	Map.SetTile(0, TClientMapProgress::MakeTile(TILE_START));
	Map.SetTile(1, TClientMapProgress::MakeTile(TILE_FREEZE));
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_FINISH));
	for(int Row = 1; Row < 6; ++Row)
		Map.SetTile(3 * Row + 1, TClientMapProgress::MakeTile(TILE_SOLID));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Cost(0).m_Risk, 0);
	EXPECT_EQ(Field.Length(0), 14);
	EXPECT_EQ(Field.Next(0), 3);
}

TEST(TClientMapProgress, NearDeathDetectionRespectsMapEdgesAndGrounding)
{
	TClientMapProgress::CMap Map(3, 2);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_DEATH));
	Map.SetTile(3, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, 0, 0, CANTMOVE_DOWN));
	Map.Finalize();
	EXPECT_FALSE(Map.Tile(3).m_NearDeath);
	EXPECT_TRUE(Map.Tile(4).m_NearDeath);
	EXPECT_TRUE(Map.Tile(3).m_Grounded);
	EXPECT_FALSE(Map.Tile(4).m_Grounded);
}

TEST(TClientMapProgress, DirectTeleportLinksRoomsWithoutCountingWorldDistance)
{
	auto Map = MakeProgressLine(9);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELEINEVIL, 17));
	Map.SetTile(3, TClientMapProgress::MakeTile(TILE_SOLID));
	Map.SetTile(6, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELEOUT, 17));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Length(0), 4);
	EXPECT_EQ(Field.Next(2), 6);
	EXPECT_EQ(Field.Length(2), Field.Length(6));
}

TEST(TClientMapProgress, InvalidTeleportNumberDoesNotCreateATeleportRoute)
{
	auto Map = MakeProgressLine(5);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELEOUT, 256));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Length(0), 4);
	EXPECT_EQ(Field.Next(2), 3);
}

TEST(TClientMapProgress, TeleportLandingCountsRiskButNotDistance)
{
	auto Map = MakeProgressLine(9);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELEIN, 17));
	Map.SetTile(3, TClientMapProgress::MakeTile(TILE_SOLID));
	Map.SetTile(6, TClientMapProgress::MakeTile(TILE_AIR, TILE_FREEZE, TILE_TELEOUT, 17));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Cost(0).m_Risk, 1);
	EXPECT_EQ(Field.Cost(2).m_Water, 1);
	EXPECT_EQ(Field.Length(2), Field.Length(6));
	EXPECT_EQ(Field.Next(2), 6);
}

TEST(TClientMapProgress, CheckpointReturnUsesRecordedNumberAndFallsBackToEarlierExit)
{
	auto Map = MakeProgressLine(9);
	Map.SetTile(1, TClientMapProgress::MakeTile(ENTITY_OFFSET + ENTITY_SPAWN));
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELECHECKINEVIL, 0));
	Map.SetTile(3, TClientMapProgress::MakeTile(TILE_SOLID));
	Map.SetTile(6, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELECHECKOUT, 2));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 4);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Next(2), 6);
	EXPECT_EQ(Field.Length(0), 4);
	Field.Start(Map, 1);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Length(0), -1);
}

TEST(TClientMapProgress, CheckpointReturnWithoutExitGoesToSpawn)
{
	auto Map = MakeProgressLine(9);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELECHECKIN, 0));
	Map.SetTile(3, TClientMapProgress::MakeTile(TILE_SOLID));
	Map.SetTile(6, TClientMapProgress::MakeTile(ENTITY_OFFSET + ENTITY_SPAWN));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Next(2), 6);
	EXPECT_EQ(Field.Length(0), 4);
}

TEST(TClientMapProgress, TimeCheckpointsDivideStagesAndBacktrackingDecreasesProgress)
{
	auto Map = MakeProgressLine(11);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_TIME_CHECKPOINT_FIRST));
	Map.SetTile(8, TClientMapProgress::MakeTile(TILE_TIME_CHECKPOINT_FIRST + 1));
	Map.Finalize();
	ASSERT_EQ(Map.CheckpointCount(), 2);
	TClientMapProgress::CPlayer Player;
	MoveProgressPlayer(Player, Map, 0);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 2).m_Progress, 1.0f / 3.0f, 0.001f);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 5).m_Progress, 0.5f, 0.001f);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 8).m_Progress, 2.0f / 3.0f, 0.001f);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 9).m_Progress, 5.0f / 6.0f, 0.001f);
	MoveProgressPlayer(Player, Map, 8);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 7).m_Progress, 11.0f / 18.0f, 0.001f);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 3).m_Progress, 7.0f / 18.0f, 0.001f);
	MoveProgressPlayer(Player, Map, 2);
	EXPECT_NEAR(MoveProgressPlayer(Player, Map, 1).m_Progress, 1.0f / 6.0f, 0.001f);
}

TEST(TClientMapProgress, MissingAndReversedCheckpointOrderFallsBackToWholeMap)
{
	for(const bool Reversed : {false, true})
	{
		SCOPED_TRACE(Reversed);
		auto Map = MakeProgressLine(11);
		Map.SetTile(2, TClientMapProgress::MakeTile(TILE_TIME_CHECKPOINT_FIRST + 1));
		if(Reversed)
			Map.SetTile(8, TClientMapProgress::MakeTile(TILE_TIME_CHECKPOINT_FIRST));
		Map.Finalize();
		TClientMapProgress::CPlayer Player;
		MoveProgressPlayer(Player, Map, 0);
		const auto Estimate = MoveProgressPlayer(Player, Map, 5);
		ASSERT_TRUE(Estimate.m_Valid);
		EXPECT_FLOAT_EQ(Estimate.m_Progress, 0.5f);
	}
}

TEST(TClientMapProgress, MidRunCanEstimateWithoutHavingObservedStart)
{
	auto Map = MakeProgressLine(11);
	Map.Finalize();
	TClientMapProgress::CPlayer Player;
	const auto Estimate = MoveProgressPlayer(Player, Map, 6);
	ASSERT_TRUE(Estimate.m_Valid);
	EXPECT_FLOAT_EQ(Estimate.m_Progress, 0.6f);
}

TEST(TClientMapProgress, MainAndDummyCheckpointStatesAreIndependent)
{
	auto Map = MakeProgressLine(9);
	Map.SetTile(1, TClientMapProgress::MakeTile(ENTITY_OFFSET + ENTITY_SPAWN));
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELECHECKIN, 0));
	Map.SetTile(3, TClientMapProgress::MakeTile(TILE_SOLID));
	Map.SetTile(6, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, TILE_TELECHECKOUT, 2));
	Map.Finalize();
	TClientMapProgress::CPlayer Main;
	TClientMapProgress::CPlayer Dummy;
	EXPECT_TRUE(MoveProgressPlayer(Main, Map, 1, 2).m_Valid);
	EXPECT_FALSE(MoveProgressPlayer(Dummy, Map, 1, 0).m_Valid);
	EXPECT_FALSE(MoveProgressPlayer(Main, Map, 1, 0).m_Valid);
	EXPECT_TRUE(MoveProgressPlayer(Dummy, Map, 1, 2).m_Valid);
}

TEST(TClientMapProgress, OneWayRestrictionsAreRespectedAndEmptyMapsStayUnknown)
{
	auto Map = MakeProgressLine(5);
	Map.SetTile(2, TClientMapProgress::MakeTile(TILE_AIR, TILE_AIR, 0, 0, CANTMOVE_RIGHT));
	Map.Finalize();
	TClientMapProgress::CField Field;
	Field.Start(Map, 0);
	while(!Field.Complete())
		Field.Step(Map, 1);
	EXPECT_EQ(Field.Length(0), -1);

	TClientMapProgress::CMap Empty(1, 1);
	Empty.Finalize();
	TClientMapProgress::CPlayer Player;
	EXPECT_FALSE(MoveProgressPlayer(Player, Empty, 0).m_Valid);
}
