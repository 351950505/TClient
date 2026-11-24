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

TEST(TClientHudMapProgress, IslandToggleAddsOnlyAValidPercentageValue)
{
	using TClientHudMapProgress::ShouldShowIslandValue;
	EXPECT_FALSE(ShouldShowIslandValue(false, 1, true, true));
	EXPECT_FALSE(ShouldShowIslandValue(true, 0, true, true));
	EXPECT_FALSE(ShouldShowIslandValue(true, 1, false, true));
	EXPECT_FALSE(ShouldShowIslandValue(true, 1, true, false));
	EXPECT_TRUE(ShouldShowIslandValue(true, 1, true, true));
}

TEST(TClientHudMapProgress, FallbackPercentageStaysInsideTheHudAndCenters)
{
	const auto Layout = TClientHudMapProgress::FallbackTextLayout(300.0f, 300.0f, 28.0f, 10.0f, 50.0f, 97.0f);
	EXPECT_FLOAT_EQ(Layout.m_Text.w, 84.0f);
	EXPECT_FLOAT_EQ(Layout.m_Text.x + Layout.m_Text.w * 0.5f, 150.0f);
	EXPECT_LE(Layout.m_Text.x, 300.0f - Layout.m_Text.w);
	EXPECT_LE(Layout.m_Text.y + Layout.m_Text.h, 298.0f);
	EXPECT_GE(Layout.m_FontSize, 6.0f);
}

TEST(TClientHudMapProgress, FallbackPercentageBoundsClampOversizedInputs)
{
	const auto Layout = TClientHudMapProgress::FallbackTextLayout(300.0f, 300.0f, 1000.0f, 1000.0f, -100.0f, 1000.0f);
	EXPECT_GE(Layout.m_Text.x, 6.0f);
	EXPECT_GE(Layout.m_Text.y, 2.0f);
	EXPECT_LE(Layout.m_Text.x + Layout.m_Text.w, 294.0f);
	EXPECT_LE(Layout.m_Text.y + Layout.m_Text.h, 298.0f);
}

TEST(TClientHudMapProgress, BoardPercentageSitsBelowTheActualPanelBounds)
{
	const CUIRect Board{240.0f, 248.0f, 54.0f, 42.0f};
	const float SafeBottom = 300.0f;
	ASSERT_TRUE(TClientHudMapProgress::HasRoomBelowBoard(Board, SafeBottom));
	const CUIRect Value = TClientHudMapProgress::BoardValue(Board);
	EXPECT_FLOAT_EQ(Value.x + Value.w * 0.5f, Board.x + Board.w * 0.5f);
	EXPECT_FLOAT_EQ(Value.y, Board.y + Board.h + TClientHudMapProgress::BOARD_VALUE_GAP);
	EXPECT_LE(Value.y + Value.h + TClientHudMapProgress::BOARD_VALUE_BOTTOM_PADDING, SafeBottom);
}

TEST(TClientHudMapProgress, BoardPercentageFallsBackWhenThePanelWasClampedIntoTheSafeArea)
{
	const CUIRect Board{240.0f, 258.0f, 54.0f, 42.0f};
	EXPECT_FALSE(TClientHudMapProgress::HasRoomBelowBoard(Board, 300.0f));
}

TEST(TClientHudMapProgress, IslandValueUsesOneCenteredNumberCellWithoutATrack)
{
	const float CellWidth = TClientHudMapProgress::IslandValueCellWidth(28.0f, 19.0f);
	const auto Layout = TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth, 72.0f, 28.0f, 16.0f, 19.0f);
	ASSERT_TRUE(Layout.m_Visible);
	EXPECT_FLOAT_EQ(Layout.m_ValueX + 28.0f * 0.5f, 50.0f + CellWidth * 0.5f);
	EXPECT_FLOAT_EQ(Layout.m_ValueY + Layout.m_FontSize * 0.5f, 46.0f);
	EXPECT_FALSE(TClientHudMapProgress::IslandValueLayout(50.0f, 10.0f, CellWidth - 1.0f, 72.0f, 28.0f, 16.0f, 19.0f).m_Visible);
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
