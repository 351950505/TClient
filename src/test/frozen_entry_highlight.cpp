#include <game/client/components/tclient/frozen_entry_highlight.h>

#include <gtest/gtest.h>

TEST(ClientFrozenEntryHighlight, FirstSnapshotSetsBaselineWithoutLookingUpTiles)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	const auto FindTile = [&] { ++Lookups; return 5; };
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 80, 10, 0.0f, true, FindTile), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 80, 11, 1.0f, true, FindTile), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_EQ(Lookups, 0);
}

TEST(ClientFrozenEntryHighlight, TileFlashesOnlyWhenFreezeTimerCompletes)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 60, 10, 1.0f, true, [] { return 37; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 60, 59, 1.49f, true, [] { return -1; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 60, 1.5f, true, [] { return -1; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Highlight.Alpha(1.75f), 0.5f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 61, 2.0f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, EarlyRescueCancelsPendingTile)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	Highlight.Update(true, true, 60, 10, 0.1f, true, [] { return 37; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 30, 0.2f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	Highlight.Update(true, true, 90, 40, 0.3f, true, [] { return 38; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 89, 0.4f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, DeepFreezeHasNoTimedCompletion)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, -1, 10, 0.1f, true, [] { return 37; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 10000, 100.0f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, ExtendedFreezeUsesLatestEndTick)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	Highlight.Update(true, true, 60, 10, 0.1f, true, [] { return 37; });
	Highlight.Update(true, true, 90, 40, 0.2f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 60, 0.3f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	Highlight.Update(true, true, 120, 70, 0.4f, true, [] { return 38; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 120, 1.0f, true, [] { return -1; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 38);
}

TEST(ClientFrozenEntryHighlight, FrozenFramesDoNotRepeatTileLookup)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 70, 10, 1.0f, true, [&] { ++Lookups; return 37; }), 0.0f);
	for(int Frame = 1; Frame <= 60; ++Frame)
		Highlight.Update(true, true, 70, 10 + Frame, 1.0f + Frame / 120.0f, true, [&] { ++Lookups; return 37; });
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, FreezeWithoutTileDoesNotFlash)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 60, 10, 0.1f, true, [] { return -1; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 60, 0.2f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, CooldownStartsAfterCompletedFreeze)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	Highlight.Update(true, true, 10, 1, 0.1f, true, [] { return 10; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 10, 1.0f, true, [] { return -1; }), 1.0f);
	Highlight.Update(true, true, 20, 12, 1.1f, true, [] { return 11; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 20, 5.9f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	Highlight.Update(true, true, 30, 21, 6.0f, true, [] { return 12; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 30, 6.1f, true, [] { return -1; }), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 12);
}

TEST(ClientFrozenEntryHighlight, CooldownAvoidsFreezeTileLookup)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	Highlight.Update(true, true, 10, 1, 0.1f, true, [] { return 10; });
	Highlight.Update(true, false, 0, 10, 0.2f, true, [] { return -1; });
	Highlight.Update(true, true, 20, 11, 1.0f, true, [] { return 11; });
	int Lookups = 0;
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 20, 1.1f, true, [&] { ++Lookups; return -1; }), 0.0f);
	EXPECT_EQ(Lookups, 0);
}

TEST(ClientFrozenEntryHighlight, DisabledFreezeDoesNotFlashWhenReenabled)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, 10, 1, 0.1f, false, [] { return 10; }), 0.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, false, 0, 10, 0.2f, true, [] { return -1; }), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, LocalTrackersHaveIndependentPendingTiles)
{
	tclient_frozen_entry::CHighlight Main;
	tclient_frozen_entry::CHighlight Dummy;
	Main.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	Dummy.Update(true, false, 0, 0, 0.0f, true, [] { return -1; });
	Main.Update(true, true, 10, 1, 0.1f, true, [] { return 10; });
	Dummy.Update(true, true, 20, 1, 0.1f, true, [] { return 20; });
	EXPECT_FLOAT_EQ(Main.Update(true, false, 0, 10, 0.2f, true, [] { return -1; }), 1.0f);
	EXPECT_FLOAT_EQ(Dummy.Update(true, false, 0, 20, 0.3f, true, [] { return -1; }), 1.0f);
	EXPECT_EQ(Main.TileIndex(), 10);
	EXPECT_EQ(Dummy.TileIndex(), 20);
}
