#include <game/client/components/tclient/frozen_entry_highlight.h>

#include <gtest/gtest.h>

TEST(ClientFrozenEntryHighlight, FirstFrozenSnapshotHighlightsCurrentTile)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 5; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 5);
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, TileTurnsOnWithFreezeAndOffWithThaw)
{
	tclient_frozen_entry::CHighlight Highlight;
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, true, [] { return -1; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [] { return 37; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, FreezeHighlightsTheEntryTileUntilAlive)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, true, [] { return -1; });
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 37; }), 1.0f);
	for(int Frame = 1; Frame <= 60; ++Frame)
		EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 38; }), 1.0f);
	EXPECT_EQ(Lookups, 1);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, PermanentFreezeHighlightsItsEntryTile)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [] { return 37; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, FreezeWithoutTileDoesNotHighlight)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, true, [] { return -1; }), 0.0f);
}

TEST(ClientFrozenEntryHighlight, EnablingDuringFreezeHighlightsCurrentTileImmediately)
{
	tclient_frozen_entry::CHighlight Highlight;
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, false, [] { return -1; }), 0.0f);
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, false, [&] { ++Lookups; return 37; }), 0.0f);
	EXPECT_EQ(Lookups, 0);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 37; }), 1.0f);
	EXPECT_EQ(Lookups, 1);
	EXPECT_EQ(Highlight.TileIndex(), 37);
}

TEST(ClientFrozenEntryHighlight, DisabledFreezeNeverHighlights)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, false, [] { return 10; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, InactiveViewClearsHighlightImmediately)
{
	tclient_frozen_entry::CHighlight Highlight;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [] { return 10; }), 1.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(false, true, true, [] { return 10; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, MainAndDummyTrackTilesIndependently)
{
	tclient_frozen_entry::CHighlight Main;
	tclient_frozen_entry::CHighlight Dummy;
	EXPECT_FLOAT_EQ(Main.Update(true, true, true, [] { return 10; }), 1.0f);
	EXPECT_FLOAT_EQ(Dummy.Update(true, true, true, [] { return 20; }), 1.0f);
	EXPECT_EQ(Main.TileIndex(), 10);
	EXPECT_EQ(Dummy.TileIndex(), 20);
	EXPECT_FLOAT_EQ(Main.Update(true, false, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Main.TileIndex(), -1);
	EXPECT_EQ(Dummy.TileIndex(), 20);
}
