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
	EXPECT_FLOAT_EQ(MAP_PROGRESS_HUD_MAX_WIDTH, 25.0f);
	EXPECT_FLOAT_EQ(MAP_PROGRESS_HUD_FONT_SIZE, 4.5f);
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
	const float IconWidth = 12.0f;
	const float IconGap = 4.0f;
	const float Padding = 19.0f;
	const float ContentWidth = TClientHudMapProgress::IslandValueContentWidth(MaximumTextWidth, IconWidth, IconGap);
	const float CellWidth = TClientHudMapProgress::IslandValueCellWidth(ContentWidth, Padding);
	const auto ValidLayout = TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth, 72.0f, ContentWidth, 16.0f, Padding);
	const auto PlaceholderLayout = TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth, 72.0f, ContentWidth, 16.0f, Padding);
	ASSERT_TRUE(ValidLayout.m_Visible);
	ASSERT_TRUE(PlaceholderLayout.m_Visible);
	EXPECT_FLOAT_EQ(ValidLayout.m_ValueX, PlaceholderLayout.m_ValueX);
	EXPECT_FLOAT_EQ(ValidLayout.m_ValueY, PlaceholderLayout.m_ValueY);
	EXPECT_FLOAT_EQ(ContentWidth, MaximumTextWidth + IconWidth + IconGap);
	EXPECT_FLOAT_EQ(ValidLayout.m_ValueX, 50.0f + Padding);
	EXPECT_FALSE(TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth - 1.0f, 72.0f, ContentWidth, 16.0f, Padding).m_Visible);
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
