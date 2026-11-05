#include "test.h"

#include <engine/shared/datafile.h>
#include <engine/shared/map.h>
#include <engine/storage.h>

#include <game/gamecore.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <memory>

class CMapQuadsTest : public ::testing::Test
{
protected:
	CTestInfo m_Info;
	std::unique_ptr<IStorage> m_pStorage;

	void SetUp() override
	{
		m_Info.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_Info.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
	}

	void WriteMap(int NumQuads, size_t DataSize = sizeof(CQuad), int Version = 2,
		size_t ItemSize = sizeof(CMapItemLayerQuads), int DataIndex = -2, const char *pFilename = "quads.map")
	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(m_pStorage.get(), pFilename));
		CMapItemVersion MapVersion = {1};
		Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(MapVersion), &MapVersion);

		CMapItemGroup Group = {};
		Group.m_Version = 3;
		Group.m_ParallaxX = Group.m_ParallaxY = 100;
		Group.m_NumLayers = 2;
		StrToInts(Group.m_aName, std::size(Group.m_aName), "Game");
		Writer.AddItem(MAPITEMTYPE_GROUP, 0, sizeof(Group), &Group);

		CTile aTiles[4] = {};
		CMapItemLayerTilemap Game = {};
		Game.m_Layer.m_Type = LAYERTYPE_TILES;
		Game.m_Version = 3;
		Game.m_Width = Game.m_Height = 2;
		Game.m_Flags = TILESLAYERFLAG_GAME;
		Game.m_Color = {255, 255, 255, 255};
		Game.m_ColorEnv = Game.m_Image = -1;
		Game.m_Tele = Game.m_Speedup = Game.m_Front = Game.m_Switch = Game.m_Tune = -1;
		Game.m_Data = Writer.AddData(sizeof(aTiles), aTiles);
		StrToInts(Game.m_aName, std::size(Game.m_aName), "Game");
		Writer.AddItem(MAPITEMTYPE_LAYER, 0, sizeof(Game), &Game);

		CQuad Quad = {};
		Quad.m_aPoints[0].x = 12345;
		CMapItemLayerQuads Layer = {};
		Layer.m_Layer.m_Type = LAYERTYPE_QUADS;
		Layer.m_Version = Version;
		Layer.m_NumQuads = NumQuads;
		Layer.m_Image = -1;
		Layer.m_Data = DataIndex == -2 ? Writer.AddDataSwapped(DataSize, &Quad) : DataIndex;
		StrToInts(Layer.m_aName, std::size(Layer.m_aName), "QFr");
		Writer.AddItem(MAPITEMTYPE_LAYER, 1, ItemSize, &Layer);
		Writer.Finish();
	}

	bool Load(CMap &Map)
	{
		return Map.Load(m_pStorage.get(), "quads.map", IStorage::TYPE_ALL);
	}
};

TEST_F(CMapQuadsTest, ValidQuadData)
{
	WriteMap(1);
	CMap Map;
	ASSERT_TRUE(Load(Map));
	const auto *pLayer = static_cast<const CMapItemLayerQuads *>(Map.FindItem(MAPITEMTYPE_LAYER, 1));
	ASSERT_NE(pLayer, nullptr);
	const auto *pQuad = static_cast<const CQuad *>(Map.GetDataSwapped(pLayer->m_Data));
	ASSERT_NE(pQuad, nullptr);
	EXPECT_EQ(pQuad->m_aPoints[0].x, 12345);
}

TEST_F(CMapQuadsTest, LegacyQuadHeader)
{
	WriteMap(1, sizeof(CQuad), 1, offsetof(CMapItemLayerQuads, m_aName));
	CMap Map;
	ASSERT_TRUE(Load(Map));
	const int Index = Map.FindItemIndex(MAPITEMTYPE_LAYER, 1);
	ASSERT_GE(Index, 0);
	EXPECT_EQ(Map.GetItemSize(Index), sizeof(CMapItemLayerQuads));
	const auto *pLayer = static_cast<const CMapItemLayerQuads *>(Map.GetItem(Index));
	char aName[sizeof(pLayer->m_aName)];
	ASSERT_TRUE(IntsToStr(pLayer->m_aName, std::size(pLayer->m_aName), aName, sizeof(aName)));
	EXPECT_STREQ(aName, "");
}

TEST_F(CMapQuadsTest, EmptyQuadLayerWithoutData)
{
	WriteMap(0, sizeof(CQuad), 2, sizeof(CMapItemLayerQuads), -1);
	CMap Map;
	EXPECT_TRUE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsTruncatedQuadHeader)
{
	WriteMap(1, sizeof(CQuad), 2, sizeof(CMapItemLayer));
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsTruncatedQuadName)
{
	WriteMap(1, sizeof(CQuad), 2, offsetof(CMapItemLayerQuads, m_aName));
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsNegativeQuadCount)
{
	WriteMap(-1);
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsQuadCountBeyondData)
{
	WriteMap(2);
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsOverflowingQuadCount)
{
	WriteMap(std::numeric_limits<int>::max());
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsTruncatedQuadData)
{
	WriteMap(1, sizeof(CQuad) - sizeof(int));
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsMissingQuadData)
{
	WriteMap(1, sizeof(CQuad), 2, sizeof(CMapItemLayerQuads), -1);
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, RejectsQuadDataIndexBeyondEnd)
{
	WriteMap(1, sizeof(CQuad), 2, sizeof(CMapItemLayerQuads), 100);
	CMap Map;
	EXPECT_FALSE(Load(Map));
}

TEST_F(CMapQuadsTest, FailedLoadPreservesCurrentMap)
{
	WriteMap(1);
	CMap Map;
	ASSERT_TRUE(Load(Map));
	WriteMap(2, sizeof(CQuad), 2, sizeof(CMapItemLayerQuads), -2, "invalid.map");
	EXPECT_FALSE(Map.Load(m_pStorage.get(), "invalid.map", IStorage::TYPE_ALL));
	EXPECT_TRUE(Map.IsLoaded());
	const auto *pLayer = static_cast<const CMapItemLayerQuads *>(Map.FindItem(MAPITEMTYPE_LAYER, 1));
	ASSERT_NE(pLayer, nullptr);
	EXPECT_EQ(pLayer->m_NumQuads, 1);
}
