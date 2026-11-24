#include "test.h"

#include <engine/shared/datafile.h>
#include <engine/shared/map_settings.h>
#include <engine/storage.h>

#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

TEST(MapSettings, RejectsInvalidBlocksBeforeVisitingAnyCommands)
{
	const char aValidPrefixWithUnterminatedLastString[] = "tune gun_speed 1400\0tune gun_curvature 0";
	int NumVisited = 0;
	auto Visit = [&](const char *, size_t) { NumVisited++; };

	EXPECT_FALSE(ForEachMapSetting(nullptr, sizeof(aValidPrefixWithUnterminatedLastString), Visit));
	EXPECT_FALSE(ForEachMapSetting(aValidPrefixWithUnterminatedLastString, 0, Visit));
	EXPECT_FALSE(ForEachMapSetting(aValidPrefixWithUnterminatedLastString, -1, Visit));
	EXPECT_FALSE(ForEachMapSetting(aValidPrefixWithUnterminatedLastString, sizeof(aValidPrefixWithUnterminatedLastString) - 1, Visit));
	EXPECT_EQ(NumVisited, 0);
}

TEST(MapSettings, VisitsValidStringsUnchanged)
{
	const char aSettings[] = "tune gun_speed 1400\0tune gun_curvature 0\0tune shotgun_speed 500\0";
	std::vector<std::string> vCommands;
	std::vector<size_t> vLengths;

	ASSERT_TRUE(ForEachMapSetting(aSettings, sizeof(aSettings) - 1, [&](const char *pCommand, size_t Length) {
		vCommands.emplace_back(pCommand, Length);
		vLengths.push_back(Length);
	}));

	EXPECT_EQ(vCommands, (std::vector<std::string>{"tune gun_speed 1400", "tune gun_curvature 0", "tune shotgun_speed 500"}));
	ASSERT_EQ(vLengths.size(), vCommands.size());
	for(size_t i = 0; i < vCommands.size(); i++)
		EXPECT_EQ(vLengths[i], vCommands[i].size());
}

TEST(MapSettings, RejectsInvalidDataFileIndexAndUnavailableData)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr);
	CTestInfo Info;
	const char aData[] = "valid\0";

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));
		Writer.AddData(sizeof(aData), aData);
		Writer.Finish();
	}

	{
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));
		const int DataIndex = 0;
		Reader.AddDataProcessor(DataIndex, [](void *pData, size_t Size) {
			std::free(pData);
			return std::pair<void *, size_t>{nullptr, Size};
		});
		EXPECT_EQ(Reader.GetDataSize(DataIndex), (int)sizeof(aData));
		EXPECT_EQ(Reader.GetData(DataIndex), nullptr);
		EXPECT_EQ(Reader.GetDataSize(DataIndex), (int)sizeof(aData));
		int NumVisited = 0;
		EXPECT_FALSE(ForEachMapSetting(static_cast<const char *>(Reader.GetData(DataIndex)), Reader.GetDataSize(DataIndex), [&](const char *, size_t) { NumVisited++; }));
		EXPECT_EQ(NumVisited, 0);

		const int InvalidIndex = Reader.NumData();
		EXPECT_EQ(Reader.GetDataSize(InvalidIndex), 0);
		EXPECT_EQ(Reader.GetData(InvalidIndex), nullptr);
		EXPECT_FALSE(ForEachMapSetting(static_cast<const char *>(Reader.GetData(InvalidIndex)), Reader.GetDataSize(InvalidIndex), [&](const char *, size_t) { NumVisited++; }));
		EXPECT_EQ(NumVisited, 0);
		Reader.Close();
	}

	if(!HasFailure())
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
}

TEST(MapSettings, DataIndexCanBeSharedWithRenderItems)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr);
	CTestInfo Info;
	const char aSettings[] = "tune gun_speed 1400\0";
	int SettingsIndex = -1;

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));
		SettingsIndex = Writer.AddData(sizeof(aSettings) - 1, aSettings);
		CMapItemInfoSettings MapInfo{};
		MapInfo.m_Settings = SettingsIndex;
		Writer.AddItem(MAPITEMTYPE_INFO, 0, sizeof(MapInfo), &MapInfo);
		CMapItemImage_v1 Image{};
		Image.m_ImageName = SettingsIndex;
		Image.m_ImageData = -1;
		Image.m_External = 1;
		Writer.AddItem(MAPITEMTYPE_IMAGE, 0, sizeof(Image), &Image);
		Writer.Finish();
	}

	{
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));
		const auto *pMapInfo = static_cast<const CMapItemInfoSettings *>(Reader.FindItem(MAPITEMTYPE_INFO, 0));
		const auto *pImage = static_cast<const CMapItemImage_v1 *>(Reader.FindItem(MAPITEMTYPE_IMAGE, 0));
		ASSERT_NE(pMapInfo, nullptr);
		ASSERT_NE(pImage, nullptr);
		EXPECT_EQ(pMapInfo->m_Settings, pImage->m_ImageName);

		const char *pSettings = static_cast<const char *>(Reader.GetData(pMapInfo->m_Settings));
		ASSERT_NE(pSettings, nullptr);
		ASSERT_TRUE(ForEachMapSetting(pSettings, Reader.GetDataSize(pMapInfo->m_Settings), [](const char *, size_t) {}));
		EXPECT_EQ(Reader.GetData(pImage->m_ImageName), pSettings);
		EXPECT_STREQ(pSettings, "tune gun_speed 1400");
		Reader.Close();
	}

	if(!HasFailure())
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
}
