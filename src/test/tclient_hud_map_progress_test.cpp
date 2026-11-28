/*
 * Based on QmClient's src/game/client/components/qmclient/hud_map_progress.h.
 * Adapted for TClient standalone/Dynamic Island surfaces; tests cover the adapted layouts.
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
#include <game/client/components/tclient/hud_map_progress.h>
#include <game/map/freeze_outline_geometry.h>

#include <gtest/gtest.h>

TEST(TClientHudMapProgress, IslandAndStandaloneHudDestinationsAreMutuallyExclusive)
{
	using TClientHudMapProgress::ShouldReserveIslandValue;
	using TClientHudMapProgress::ShouldShowIslandValue;
	using TClientHudMapProgress::ShouldShowStandaloneValue;
	EXPECT_FALSE(ShouldReserveIslandValue(false, 1, true, true));
	EXPECT_FALSE(ShouldReserveIslandValue(true, 0, true, true));
	EXPECT_FALSE(ShouldReserveIslandValue(true, 1, false, true));
	EXPECT_TRUE(ShouldReserveIslandValue(true, 1, true, true));
	EXPECT_FALSE(ShouldReserveIslandValue(true, 1, true, false));
	EXPECT_FALSE(ShouldShowIslandValue(true, 1, true, false, true));
	EXPECT_TRUE(ShouldShowIslandValue(true, 1, true, true, true));
	EXPECT_FALSE(ShouldShowIslandValue(true, 1, true, true, false));
	EXPECT_TRUE(ShouldShowStandaloneValue(true, 0, true, true));
	EXPECT_FALSE(ShouldShowStandaloneValue(true, 1, true, true));
	EXPECT_TRUE(ShouldShowStandaloneValue(true, 1, false, true));
	EXPECT_TRUE(ShouldShowStandaloneValue(true, 1, true, false));
	EXPECT_FALSE(ShouldShowStandaloneValue(false, 0, true, true));
}

TEST(TClientHudMapProgress, MiniDebugCoordinatesKeepTheirOriginalOffsets)
{
	using TClientHudMapProgress::MiniDebugCoordinateOffsetY;
	EXPECT_FLOAT_EQ(MiniDebugCoordinateOffsetY(false, false), 3.0f);
	EXPECT_FLOAT_EQ(MiniDebugCoordinateOffsetY(false, true), 30.0f);
	EXPECT_FLOAT_EQ(MiniDebugCoordinateOffsetY(true, false), 53.0f);
}

TEST(TClientHudMapProgress, StandaloneHudTextUsesOneAlignedRowAndReservesMatchingStackSpace)
{
	using namespace TClientHudMapProgress;
	const float PanelTop = 100.0f;
	const float ProgressTop = MapProgressHudTop(PanelTop);
	EXPECT_FLOAT_EQ(MOVEMENT_INFORMATION_BOX_WIDTH, 62.0f);
	EXPECT_FLOAT_EQ(MOVEMENT_INFORMATION_TEXT_INSET, 2.0f);
	EXPECT_FLOAT_EQ(MAP_PROGRESS_HUD_MAX_WIDTH, 25.0f);
	EXPECT_FLOAT_EQ(MAP_PROGRESS_HUD_FONT_SIZE, 4.5f);
	EXPECT_LE(MAP_PROGRESS_HUD_MAX_WIDTH, MOVEMENT_INFORMATION_BOX_WIDTH - 2.0f * MOVEMENT_INFORMATION_TEXT_INSET);
	EXPECT_FLOAT_EQ(StandaloneHudValueX(300.0f, 18.0f) + 18.0f, 298.0f);
	EXPECT_FLOAT_EQ(PanelTop - ProgressTop, MAP_PROGRESS_HUD_HEIGHT + MAP_PROGRESS_HUD_GAP);
	EXPECT_FLOAT_EQ(ProgressTop + MAP_PROGRESS_HUD_HEIGHT + MAP_PROGRESS_HUD_GAP, PanelTop);
	EXPECT_FLOAT_EQ(StandaloneHudStackOffset(true), 10.0f);
	EXPECT_FLOAT_EQ(StandaloneHudStackOffset(false), 0.0f);
	EXPECT_LT(PanelTop - StandaloneHudStackOffset(true), ProgressTop);
	EXPECT_FLOAT_EQ(ReadoutFit(18.0f), 1.0f);
	EXPECT_FLOAT_EQ(50.0f * ReadoutFit(50.0f) + LABEL_GAP, MAP_PROGRESS_HUD_MAX_WIDTH);
}

TEST(TClientHudMapProgress, IslandFinishMarkAndValueShareAStableCenteredCell)
{
	const float MaximumTextWidth = 56.0f;
	const float IconWidth = TClientHudMapProgress::ISLAND_MAP_PROGRESS_ICON_SIZE;
	const float IconGap = TClientHudMapProgress::ISLAND_MAP_PROGRESS_ICON_GAP;
	const float Padding = TClientHudMapProgress::ISLAND_MAP_PROGRESS_PADDING;
	const float FontSize = 16.0f * TClientHudMapProgress::ISLAND_MAP_PROGRESS_FONT_SCALE;
	EXPECT_FLOAT_EQ(TClientHudMapProgress::ISLAND_MAP_PROGRESS_FONT_SCALE, 0.875f);
	EXPECT_FLOAT_EQ(FontSize, 14.0f);
	EXPECT_FLOAT_EQ(IconWidth, 10.0f);
	EXPECT_FLOAT_EQ(IconGap, 3.0f);
	EXPECT_FLOAT_EQ(Padding, 14.0f);
	const float ContentWidth = TClientHudMapProgress::IslandValueContentWidth(MaximumTextWidth, IconWidth, IconGap);
	const float CellWidth = TClientHudMapProgress::IslandValueCellWidth(ContentWidth, Padding);
	const auto ValidLayout = TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth, 72.0f, ContentWidth, FontSize, Padding);
	const auto PlaceholderLayout = TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth, 72.0f, ContentWidth, FontSize, Padding);
	ASSERT_TRUE(ValidLayout.m_Visible);
	ASSERT_TRUE(PlaceholderLayout.m_Visible);
	EXPECT_FLOAT_EQ(ValidLayout.m_ValueX, PlaceholderLayout.m_ValueX);
	EXPECT_FLOAT_EQ(ValidLayout.m_ValueY, PlaceholderLayout.m_ValueY);
	EXPECT_FLOAT_EQ(ContentWidth, MaximumTextWidth + IconWidth + IconGap);
	EXPECT_FLOAT_EQ(ValidLayout.m_ValueX, 50.0f + Padding);
	EXPECT_FALSE(TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth - 1.0f, 72.0f, ContentWidth, FontSize, Padding).m_Visible);
	EXPECT_FLOAT_EQ(CellWidth, ContentWidth + 2.0f * Padding);
}

TEST(TClientHudMapProgress, ProgressSmoothsAndResetsWhenTargetChangesOrDisappears)
{
	TClientHudMapProgress::CProgressState State;
	EXPECT_FLOAT_EQ(State.Update(true, 0.25f, 0.016f, 4), 0.25f);
	EXPECT_FLOAT_EQ(State.Update(true, 0.75f, 0.125f, 4), 0.75f);
	EXPECT_FLOAT_EQ(State.Update(true, 0.20f, 0.016f, 5), 0.20f);
	EXPECT_FLOAT_EQ(State.Update(true, 2.0f, 0.0f, 5), 0.20f);
	EXPECT_FLOAT_EQ(State.Update(false, 0.5f, 1.0f, 5), 0.0f);
	EXPECT_FALSE(State.IsTracking(5));
}

TEST(TClientHudMapProgress, ReadoutFollowsActiveTargetEvenBeforeNextHudUpdate)
{
	TClientHudMapProgress::CProgressState State;
	constexpr int Self = 4, OtherA = 7, OtherB = 9, Freeview = -1;
	float Value = -1.0f;
	EXPECT_FALSE(State.TryGetForClient(Self, Value));
	State.Update(true, 0.20f, 0.0f, Self);
	ASSERT_TRUE(State.TryGetForClient(Self, Value));
	EXPECT_FLOAT_EQ(Value, 0.20f);

	EXPECT_FALSE(State.TryGetForClient(OtherA, Value));
	State.Update(true, 0.70f, 0.0f, OtherA);
	ASSERT_TRUE(State.TryGetForClient(OtherA, Value));
	EXPECT_FLOAT_EQ(Value, 0.70f);

	EXPECT_FALSE(State.TryGetForClient(OtherB, Value));
	State.Update(false, 0.0f, 0.0f, OtherB);
	EXPECT_FALSE(State.TryGetForClient(OtherB, Value));
	State.Update(true, 0.40f, 0.0f, OtherB);
	ASSERT_TRUE(State.TryGetForClient(OtherB, Value));
	EXPECT_FLOAT_EQ(Value, 0.40f);

	EXPECT_FALSE(State.TryGetForClient(Freeview, Value));
	EXPECT_FALSE(State.TryGetForClient(Self, Value)); // No HUD update during freeview.
	State.Update(true, 0.30f, 0.0f, Self);
	ASSERT_TRUE(State.TryGetForClient(Self, Value));
	EXPECT_FLOAT_EQ(Value, 0.30f);
}

TEST(FreezeOutlineGeometry, SolidFacingEdgeKeepsFreezeTileInteriorFilled)
{
	int aTypes[9]{};
	aTypes[4] = FREEZE_OUTLINE_FREEZE;
	aTypes[1] = FREEZE_OUTLINE_SOLID;
	const CFreezeOutlineMapView Map{aTypes, 3, 3};

	const auto Geometry = MakeFreezeOutlineGeometry(Map, 1, 1, FREEZE_OUTLINE_FREEZE, 13, false, true);
	bool aBand[32][32]{};
	bool aInterior[32][32]{};
	for(int i = 0; i < Geometry.m_NumBand; ++i)
	{
		const auto &Band = Geometry.m_aBand[i];
		for(int Y = Band.m_Y; Y < Band.m_Y + Band.m_Height; ++Y)
			for(int X = Band.m_X; X < Band.m_X + Band.m_Width; ++X)
				aBand[Y][X] = true;
	}
	for(int i = 0; i < Geometry.m_NumInterior; ++i)
	{
		const auto &Rect = Geometry.m_aInterior[i];
		for(int Y = Rect.m_Y; Y < Rect.m_Y + Rect.m_Height; ++Y)
			for(int X = Rect.m_X; X < Rect.m_X + Rect.m_Width; ++X)
				aInterior[Y][X] = true;
	}

	EXPECT_FALSE(aBand[6][16]); // Keep the freeze texture against the wall-facing side.
	EXPECT_TRUE(aInterior[6][16]);
	EXPECT_TRUE(aBand[25][16]); // Preserve cutout at the exposed outside edge.
	EXPECT_FALSE(aInterior[25][16]);
	EXPECT_EQ(Map.TypeAt(1, 0), FREEZE_OUTLINE_SOLID);
	EXPECT_EQ(Map.TypeAt(1, 1), FREEZE_OUTLINE_FREEZE);
}

TEST(FreezeOutlineGeometry, CutoutComplementsBandForWidthLimitsAndCorners)
{
	int aTypes[9]{};
	aTypes[4] = FREEZE_OUTLINE_FREEZE;
	aTypes[1] = FREEZE_OUTLINE_SOLID;
	const CFreezeOutlineMapView Map{aTypes, 3, 3};
	for(int Width : {1, 13, 16})
	{
		const auto Geometry = MakeFreezeOutlineGeometry(Map, 1, 1, FREEZE_OUTLINE_FREEZE, Width, true, true);
		bool aBand[32][32]{};
		bool aInterior[32][32]{};
		for(int i = 0; i < Geometry.m_NumBand; ++i)
		{
			const auto &Rect = Geometry.m_aBand[i];
			ASSERT_GE(Rect.m_X, 0);
			ASSERT_GE(Rect.m_Y, 0);
			ASSERT_LE(Rect.m_X + Rect.m_Width, 32);
			ASSERT_LE(Rect.m_Y + Rect.m_Height, 32);
			for(int y = Rect.m_Y; y < Rect.m_Y + Rect.m_Height; ++y)
				for(int x = Rect.m_X; x < Rect.m_X + Rect.m_Width; ++x)
				{
					ASSERT_FALSE(aBand[y][x]);
					aBand[y][x] = true;
				}
		}
		for(int i = 0; i < Geometry.m_NumInterior; ++i)
		{
			const auto &Rect = Geometry.m_aInterior[i];
			ASSERT_GE(Rect.m_X, 0);
			ASSERT_GE(Rect.m_Y, 0);
			ASSERT_LE(Rect.m_X + Rect.m_Width, 32);
			ASSERT_LE(Rect.m_Y + Rect.m_Height, 32);
			for(int y = Rect.m_Y; y < Rect.m_Y + Rect.m_Height; ++y)
				for(int x = Rect.m_X; x < Rect.m_X + Rect.m_Width; ++x)
				{
					ASSERT_FALSE(aInterior[y][x]);
					ASSERT_FALSE(aBand[y][x]);
					aInterior[y][x] = true;
				}
		}
		for(int y = 0; y < 32; ++y)
			for(int x = 0; x < 32; ++x)
				EXPECT_NE(aBand[y][x], aInterior[y][x]);
	}
}

TEST(FreezeOutlineGeometry, AdjacentFreezeTilesDoNotDuplicateTheirSharedSeam)
{
	int aTypes[9];
	std::fill(std::begin(aTypes), std::end(aTypes), FREEZE_OUTLINE_FREEZE);
	const CFreezeOutlineMapView Map{aTypes, 3, 3};
	const auto Geometry = MakeFreezeOutlineGeometry(Map, 1, 1, FREEZE_OUTLINE_FREEZE, 13, true, true);
	EXPECT_EQ(Geometry.m_NumBand, 0);
	ASSERT_EQ(Geometry.m_NumInterior, 1);
	EXPECT_EQ(Geometry.m_aInterior[0].m_Width, 32);
	EXPECT_EQ(Geometry.m_aInterior[0].m_Height, 32);
}
