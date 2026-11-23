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

TEST(ClientFrozenEntryHighlight, FlashFadesOutAfterHalfASecondWhileStillFrozen)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, true, [] { return -1; }, 10.0f);
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 37; }, 10.0f), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 38; }, 10.25f), 0.5f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 38; }, 10.5f), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [&] { ++Lookups; return 38; }, 10.6f), 0.0f);
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, LongFrameCannotExtendFlashBeyondHalfASecond)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, true, [] { return -1; }, 3.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [] { return 12; }, 3.0f), 1.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, [] { return 13; }, 3.6f), 0.0f);
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

TEST(ClientFrozenEntryHighlight, OnlyPrimaryLocalTeeIsSelected)
{
	EXPECT_TRUE(tclient_frozen_entry::IsPrimaryLocalClient(7, 7));
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(8, 7)); // Dummy
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(9, 7)); // Remote tee
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(-1, 7));
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(7, -1));
}
