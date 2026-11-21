#include <game/client/components/tclient/frozen_entry_highlight.h>

#include <gtest/gtest.h>

TEST(ClientFrozenEntryHighlight, FirstSnapshotSetsBaselineWithoutFlash)
{
	tclient_frozen_entry::CHighlight Highlight;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 5, 0.0f, true), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 5, 1.0f, true), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, AliveToFrozenFreezeTileFlashesForHalfSecond)
{
	tclient_frozen_entry::CHighlight Highlight;
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, -1, 1.0f, true), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 37, 1.0f, true), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 37, 1.25f, true), 0.5f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 37, 1.5f, true), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, FreezeWithoutTileDoesNotFlash)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, -1, 0.0f, true);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, -1, 0.1f, true), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, FiveSecondCooldownRequiresAnotherFreezeEdge)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, -1, 0.0f, true);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 10, 0.0f, true), 1.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, -1, 0.6f, true), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 11, 4.9f, true), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	Highlight.Update(true, false, -1, 5.0f, true);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 12, 5.0f, true), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 12);
}

TEST(ClientFrozenEntryHighlight, DisabledEdgeDoesNotTriggerWhenReenabledWhileFrozen)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, -1, 0.0f, true);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 10, 0.1f, false), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 10, 0.2f, true), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, LocalTrackersHaveIndependentCooldowns)
{
	tclient_frozen_entry::CHighlight Main;
	tclient_frozen_entry::CHighlight Dummy;
	Main.Update(true, false, -1, 0.0f, true);
	Dummy.Update(true, false, -1, 0.0f, true);
	EXPECT_FLOAT_EQ(Main.Update(true, true, 10, 0.0f, true), 1.0f);
	EXPECT_FLOAT_EQ(Dummy.Update(true, true, 20, 1.0f, true), 1.0f);
	EXPECT_EQ(Main.TileIndex(), 10);
	EXPECT_EQ(Dummy.TileIndex(), 20);
}
