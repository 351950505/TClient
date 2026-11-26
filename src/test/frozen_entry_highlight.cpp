#include <game/client/components/tclient/frozen_entry_highlight.h>

#include <gtest/gtest.h>

TEST(ClientFrozenEntryHighlight, FirstFrozenSnapshotDoesNotHighlightUntilAConfirmedThaw)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen, float Now = 0.0f) {
		return Highlight.Update(true, true, Frozen, true, true, [&] { ++Lookups; return 5; }, Now);
	};

	EXPECT_FLOAT_EQ(Update(true), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Update(true, 0.1f), 0.0f);
	EXPECT_EQ(Lookups, 0);
	EXPECT_FLOAT_EQ(Update(false, 0.2f), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Update(true, 0.3f), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 5);
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, AuthoritativeThawArmsOneHighlightPerFreezeEpisode)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen) {
		return Highlight.Update(true, true, Frozen, true, true, [&] { ++Lookups; return 37 + Lookups; });
	};

	EXPECT_FLOAT_EQ(Update(false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 38);
	EXPECT_FLOAT_EQ(Update(true), 1.0f);
	EXPECT_FLOAT_EQ(Update(true), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 38);
	EXPECT_EQ(Lookups, 1);
	EXPECT_FLOAT_EQ(Update(false), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Update(true), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 39);
	EXPECT_EQ(Lookups, 2);
}

TEST(ClientFrozenEntryHighlight, FlashFadesWithoutRestartingDuringTheSameFreeze)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen, float Now) {
		return Highlight.Update(true, true, Frozen, true, true, [&] { ++Lookups; return 37; }, Now);
	};

	Update(false, 10.0f);
	EXPECT_FLOAT_EQ(Update(true, 10.0f), 1.0f);
	EXPECT_FLOAT_EQ(Update(true, 10.25f), 0.5f);
	EXPECT_EQ(Highlight.TileIndex(), 37);
	EXPECT_FLOAT_EQ(Update(true, 10.5f), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Update(true, 10.6f), 0.0f);
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, MissingTileDoesNotRearmUntilAConfirmedThaw)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen, int Tile) {
		return Highlight.Update(true, true, Frozen, true, true, [&] { ++Lookups; return Tile; });
	};

	Update(false, -1);
	EXPECT_FLOAT_EQ(Update(true, -1), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, 37), 0.0f);
	EXPECT_EQ(Lookups, 1);
	EXPECT_FLOAT_EQ(Update(false, -1), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, 37), 1.0f);
	EXPECT_EQ(Lookups, 2);
	EXPECT_EQ(Highlight.TileIndex(), 37);
}

TEST(ClientFrozenEntryHighlight, ConfigChangesCannotHighlightAnExistingFreeze)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen, bool Enabled) {
		return Highlight.Update(true, true, Frozen, Enabled, true, [&] { ++Lookups; return 10; });
	};

	Update(false, true);
	EXPECT_FLOAT_EQ(Update(true, false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, true), 0.0f);
	EXPECT_EQ(Lookups, 0);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	Update(false, false);
	EXPECT_FLOAT_EQ(Update(true, true), 1.0f);
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, InactiveOrMissingSnapshotsCannotCountAsAThaw)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Active, bool HasServerSnapshot, bool Frozen) {
		return Highlight.Update(Active, HasServerSnapshot, Frozen, true, true, [&] { ++Lookups; return 10; });
	};

	Update(true, true, false);
	EXPECT_FLOAT_EQ(Update(true, true, true), 1.0f);
	EXPECT_FLOAT_EQ(Update(false, false, false), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
	EXPECT_FLOAT_EQ(Update(true, false, false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, true, true), 0.0f);
	EXPECT_EQ(Lookups, 1);
	EXPECT_FLOAT_EQ(Update(true, true, false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, true, true), 1.0f);
	EXPECT_EQ(Lookups, 2);
}

TEST(ClientFrozenEntryHighlight, DeathResetDoesNotCarryThePreviousFreezeEpisodeAcrossRespawn)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen) {
		return Highlight.Update(true, true, Frozen, true, true, [&] { ++Lookups; return 10; });
	};

	EXPECT_FLOAT_EQ(Update(false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true), 1.0f);
	EXPECT_EQ(Lookups, 1);

	Highlight.Reset(); // The local character died while frozen.
	EXPECT_FLOAT_EQ(Update(true), 0.0f); // A respawn observed frozen starts a fresh baseline.
	EXPECT_FLOAT_EQ(Update(true), 0.0f);
	EXPECT_EQ(Lookups, 1);
	EXPECT_FLOAT_EQ(Update(false), 0.0f); // Only this valid thaw arms the next episode.
	EXPECT_FLOAT_EQ(Update(true), 1.0f);
	EXPECT_EQ(Lookups, 2);
}

TEST(ClientFrozenEntryHighlight, HiddenViewTracksEpisodesWithoutHighlightingThem)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen, bool Visible) {
		return Highlight.Update(true, true, Frozen, true, Visible, [&] { ++Lookups; return 10; });
	};

	EXPECT_FLOAT_EQ(Update(false, false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, true), 0.0f);
	EXPECT_EQ(Lookups, 0);
	EXPECT_FLOAT_EQ(Update(false, false), 0.0f);
	EXPECT_FLOAT_EQ(Update(true, true), 1.0f);
	EXPECT_EQ(Lookups, 1);
}

TEST(ClientFrozenEntryHighlight, LongFrameCannotExtendFlashBeyondHalfASecond)
{
	tclient_frozen_entry::CHighlight Highlight;
	Highlight.Update(true, true, false, true, true, [] { return -1; }, 3.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, true, true, [] { return 12; }, 3.0f), 1.0f);
	EXPECT_FLOAT_EQ(Highlight.Update(true, true, true, true, true, [] { return 13; }, 3.6f), 0.0f);
	EXPECT_EQ(Highlight.TileIndex(), -1);
}

TEST(ClientFrozenEntryHighlight, RepeatedFreezeTileOccupancyChangesDoNotRearm)
{
	tclient_frozen_entry::CHighlight Highlight;
	int Lookups = 0;
	auto Update = [&](bool Frozen, int Tile) {
		return Highlight.Update(true, true, Frozen, true, true, [&] { ++Lookups; return Tile; });
	};

	Update(false, -1);
	EXPECT_FLOAT_EQ(Update(true, 12), 1.0f);
	EXPECT_FLOAT_EQ(Update(true, -1), 1.0f);
	EXPECT_FLOAT_EQ(Update(true, 27), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 12);
	EXPECT_EQ(Lookups, 1);

	EXPECT_FLOAT_EQ(Update(false, -1), 0.0f); // Only a confirmed thaw rearms.
	EXPECT_FLOAT_EQ(Update(true, 27), 1.0f);
	EXPECT_EQ(Highlight.TileIndex(), 27);
	EXPECT_EQ(Lookups, 2);
}

TEST(ClientFrozenEntryHighlight, OnlyPrimaryLocalTeeIsSelected)
{
	EXPECT_TRUE(tclient_frozen_entry::IsPrimaryLocalClient(7, 7));
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(8, 7)); // Dummy
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(9, 7)); // Remote tee
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(-1, 7));
	EXPECT_FALSE(tclient_frozen_entry::IsPrimaryLocalClient(7, -1));
}
