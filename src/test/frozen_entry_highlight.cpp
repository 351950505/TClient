#include <game/client/components/tclient/frozen_entry_highlight.h>

#include <gtest/gtest.h>

TEST(ClientFrozenEntryHighlight, FirstSnapshotSetsBaselineWithoutLookingUpTiles)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	const auto FindTile = [&] { ++Lookups; return 5; };
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 0.0f, true, FindTile), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 1.0f, true, FindTile), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_EQ(Lookups, 0);
}

TEST(ClientFrozenEntryHighlight, AliveToFrozenFreezeTileFlashesForHalfSecond)
{
	tclient_frozen_entry::CHighlight Highlight;
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 1.0f, true, [] { return -1; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 1.0f, true, [] { return 37; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Alpha(1.25f), 0.5f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 1.5f, true, [] { return 37; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, FrozenFramesDoNotRepeatTileLookup)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0.0f, true, [] { return -1; });
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 1.0f, true, [&] { ++Lookups; return 37; }), 1.0f);
	for(int Frame = 1; Frame <= 60; ++Frame)
		Highlight.Update(true, true, 1.0f + Frame / 120.0f, true, [&] { ++Lookups; return 37; });
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, FreezeWithoutTileDoesNotFlash)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 0.1f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, FiveSecondCooldownRequiresAnotherFreezeEdge)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 0.0f, true, [] { return 10; }), 1.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0.6f, true, [] { return -1; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 4.9f, true, [] { return 11; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	Highlight.Update(true, false, 5.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 5.0f, true, [] { return 12; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 12);
}

TEST(ClientFrozenEntryHighlight, CooldownAvoidsFreezeTileLookup)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0.0f, true, [] { return -1; });
	Highlight.Update(true, true, 0.0f, true, [] { return 10; });
	Highlight.Update(true, false, 0.6f, true, [] { return -1; });
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 1.0f, true, [&] { ++Lookups; return 11; }), 0.0f);
	EXPECT_EQ(Lookups, 0);
}

TEST(ClientFrozenEntryHighlight, DisabledEdgeDoesNotTriggerWhenReenabledWhileFrozen)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 0.1f, false, [] { return 10; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 0.2f, true, [] { return 10; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, LocalTrackersHaveIndependentCooldowns)
{
	tclient_frozen_entry::CHighlight Main;
	tclient_frozen_entry::CHighlight Dummy;
	Main.Update(true, false, 0.0f, true, [] { return -1; });
	Dummy.Update(true, false, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Main.Update(true, true, 0.0f, true, [] { return 10; }), 1.0f);
	EXPECT_FLOAT_EQ(Dummy.Update(true, true, 1.0f, true, [] { return 20; }), 1.0f);
	EXPECT_EQ(Main.TileIndex(), 10);
	EXPECT_EQ(Dummy.TileIndex(), 20);
}
