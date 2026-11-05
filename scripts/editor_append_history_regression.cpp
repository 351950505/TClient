// Client-linked regression harness. Compile this translation unit with the client
// include paths and link it with the normal client objects. In an isolated test
// copy of src/engine/client/client.cpp, call this immediately after
// m_pEditor->Init():
//   extern bool RunEditorAppendHistoryRegression(CEditor *);
//   std::_Exit(RunEditorAppendHistoryRegression(static_cast<CEditor *>(m_pEditor)) ? 0 : 1);
// Use private storage and skip network startup, external integrations and shell
// registration in the test copy. Exiting here avoids user configuration saves.
// No production entry point or CMake change is required. Assertions work in release.
#include <base/dbg.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/storage.h>

#include <game/editor/editor.h>
#include <game/editor/mapitems/envelope.h>
#include <game/editor/mapitems/image.h>
#include <game/editor/mapitems/layer_group.h>
#include <game/editor/mapitems/layer_quads.h>
#include <game/editor/mapitems/layer_sounds.h>
#include <game/editor/mapitems/map.h>
#include <game/editor/mapitems/sound.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
	void Require(bool Condition, const char *pMessage)
	{
		dbg_assert(Condition, "append history regression: %s", pMessage);
	}

	std::shared_ptr<CEditorImage> AddImage(CEditorMap &Map, const char *pName, unsigned char Value)
	{
		auto pImage = std::make_shared<CEditorImage>(&Map);
		str_copy(pImage->m_aName, pName);
		pImage->m_Width = pImage->m_Height = 16;
		pImage->m_Format = CImageInfo::FORMAT_RGBA;
		pImage->Allocate();
		memset(pImage->m_pData, Value, pImage->DataSize());
		Map.m_vpImages.push_back(pImage);
		return pImage;
	}

	// Three Ogg pages containing Opus headers and one valid 20 ms silence packet.
	// Keep the fixture self-contained while exercising real sample allocation.
	std::vector<unsigned char> SilentOpus()
	{
		std::vector<unsigned char> vResult;
		const auto Page = [&](const std::vector<unsigned char> &vPacket, unsigned char Flags, uint64_t Granule, unsigned char Sequence) {
			std::vector<unsigned char> vPage(28, 0);
			mem_copy(vPage.data(), "OggS", 4);
			vPage[5] = Flags;
			for(int i = 0; i < 8; ++i)
				vPage[6 + i] = (Granule >> (8 * i)) & 0xff;
			vPage[14] = 1; // stream serial
			vPage[18] = Sequence;
			vPage[26] = 1;
			vPage[27] = vPacket.size();
			vPage.insert(vPage.end(), vPacket.begin(), vPacket.end());
			uint32_t Crc = 0;
			for(unsigned char Byte : vPage)
			{
				Crc ^= static_cast<uint32_t>(Byte) << 24;
				for(int Bit = 0; Bit < 8; ++Bit)
					Crc = (Crc << 1) ^ ((Crc & 0x80000000U) ? 0x04c11db7U : 0);
			}
			for(int i = 0; i < 4; ++i)
				vPage[22 + i] = (Crc >> (8 * i)) & 0xff;
			vResult.insert(vResult.end(), vPage.begin(), vPage.end());
		};
		Page({'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 1, 0, 0, 0x80, 0xbb, 0, 0, 0, 0, 0}, 2, 0, 0);
		Page({'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0}, 0, 0, 1);
		Page({0xf8, 0xff, 0xfe}, 4, 960, 2);
		return vResult;
	}

	void AddResources(CEditorMap &Map)
	{
		for(auto Type : {CEnvelope::EType::POSITION, CEnvelope::EType::COLOR, CEnvelope::EType::SOUND})
		{
			auto pEnvelope = std::make_shared<CEnvelope>(Type);
			pEnvelope->AddPoint(CFixedTime(0), {1024, 1024, 1024, 1024});
			Map.m_vpEnvelopes.push_back(pEnvelope);
		}
		auto pSound = std::make_shared<CEditorSound>(&Map);
		str_copy(pSound->m_aName, "append-regression-silence");
		const auto vOpus = SilentOpus();
		pSound->m_DataSize = vOpus.size();
		pSound->m_pData = malloc(vOpus.size());
		mem_copy(pSound->m_pData, vOpus.data(), vOpus.size());
		Map.m_vpSounds.push_back(pSound);
	}

	std::shared_ptr<CLayerQuads> AddQuads(CEditorMap &Map, const char *pName, int Image)
	{
		auto pGroup = Map.NewGroup();
		str_copy(pGroup->m_aName, pName);
		auto pLayer = std::make_shared<CLayerQuads>(&Map);
		pLayer->m_Image = Image;
		for(int i = 0; i < 2; ++i)
		{
			auto *pQuad = pLayer->NewQuad(i * 64, 0, 32, 32);
			pQuad->m_PosEnv = 0;
			pQuad->m_ColorEnv = 1;
		}
		pGroup->AddLayer(pLayer);
		return pLayer;
	}

	std::shared_ptr<CLayerSounds> AddSounds(CEditorMap &Map)
	{
		auto pGroup = Map.NewGroup();
		str_copy(pGroup->m_aName, "reg-sound");
		auto pLayer = std::make_shared<CLayerSounds>(&Map);
		pLayer->m_Sound = 0;
		auto *pSource = pLayer->NewSource(0, 0);
		pSource->m_PosEnv = 0;
		pSource->m_SoundEnv = 2;
		pGroup->AddLayer(pLayer);
		return pLayer;
	}

	std::vector<std::string> Settings(const CEditorMap &Map)
	{
		std::vector<std::string> vResult;
		for(const auto &Setting : Map.m_vSettings)
			vResult.emplace_back(Setting.m_aCommand);
		return vResult;
	}

	int FindGroup(const CEditorMap &Map, const char *pName, int Start)
	{
		for(int i = Start; i < static_cast<int>(Map.m_vpGroups.size()); ++i)
			if(str_comp(Map.m_vpGroups[i]->m_aName, pName) == 0)
				return i;
		Require(false, "imported fixture group missing");
		return -1;
	}
} // namespace

bool RunEditorAppendHistoryRegression(CEditor *pEditor)
{
	Require(pEditor != nullptr, "initialized editor required");
	const FErrorHandler Error = [](const char *pMessage) { Require(false, pMessage); };
	char aFixture[IO_MAX_PATH_LENGTH];
	str_format(aFixture, sizeof(aFixture), "maps/append-history-%lld.map", static_cast<long long>(time_get()));
	pEditor->Storage()->CreateFolder("maps", IStorage::TYPE_SAVE);

	CEditorMap Source(pEditor);
	Source.CreateDefault();
	AddImage(Source, "alpha", 17);
	AddImage(Source, "middle", 33);
	AddImage(Source, "middle", 49); // Same name, different data: rename on import.
	AddResources(Source);
	AddQuads(Source, "reg-alpha", 0);
	AddQuads(Source, "reg-dedup", 1);
	AddQuads(Source, "reg-clash", 2);
	AddSounds(Source);
	Source.m_vSettings = {"sv_test_cmds 1", "sv_motd imported", "sv_motd imported", "sv_test_cmds 1"};

	Require(Source.Save(aFixture, Error), "save source fixture");
	auto pJob = pEditor->m_WriterFinishJobs.back();
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while(pJob->State() != IJob::STATE_DONE && std::chrono::steady_clock::now() < Deadline)
		std::this_thread::yield();
	Require(pJob->State() == IJob::STATE_DONE, "fixture save timed out");
	Require(pJob->ErrorMessage()[0] == '\0', pJob->ErrorMessage());
	pEditor->m_WriterFinishJobs.erase(std::find(pEditor->m_WriterFinishJobs.begin(), pEditor->m_WriterFinishJobs.end(), pJob));

	CEditorMap Map(pEditor);
	Map.CreateDefault();
	auto pMiddle = AddImage(Map, "middle", 33);
	AddImage(Map, "zulu", 65);
	AddResources(Map);
	auto pExisting = AddQuads(Map, "reg-existing", 1);
	Map.m_vSettings = {"sv_motd original", "sv_test_cmds 1", "sv_test_cmds 1"};
	Map.SelectLayer(0, 2);
	Map.m_vSelectedQuads = {1};
	Map.m_SelectedImage = 1;
	Map.m_SelectedEnvelope = 0;
	Map.m_SelectedSound = 0;
	const auto vBeforeSettings = Settings(Map);
	const auto vpBeforeImages = Map.m_vpImages;
	const auto BeforeGroups = Map.m_vpGroups.size();

	Require(Map.Append(aFixture, IStorage::TYPE_SAVE, false, Error), "real append");
	Require(pEditor->Storage()->RemoveFile(aFixture, IStorage::TYPE_SAVE), "remove source before redo");
	const std::vector<std::string> vAfterSettings = {"sv_motd original", "sv_test_cmds 1", "sv_test_cmds 1", "sv_motd imported"};
	Require(Settings(Map) == vAfterSettings, "append settings order and deduplication");
	Require(Map.m_vpImages.size() == 4 && Map.m_vpImages[1] == pMiddle, "image deduplication");
	Require(str_comp(Map.m_vpImages[2]->m_aName, "middle (1)") == 0, "conflicting image rename");
	auto vpAfterImages = Map.m_vpImages;
	const int AlphaGroup = FindGroup(Map, "reg-alpha", BeforeGroups);
	const int DedupGroup = FindGroup(Map, "reg-dedup", BeforeGroups);
	const int ConflictGroup = FindGroup(Map, "reg-clash", BeforeGroups);
	const int SoundGroup = FindGroup(Map, "reg-sound", BeforeGroups);
	auto *pImported = static_cast<CLayerQuads *>(Map.m_vpGroups[AlphaGroup]->m_vpLayers[0].get());
	auto *pDedup = static_cast<CLayerQuads *>(Map.m_vpGroups[DedupGroup]->m_vpLayers[0].get());
	auto *pConflict = static_cast<CLayerQuads *>(Map.m_vpGroups[ConflictGroup]->m_vpLayers[0].get());
	auto *pImportedSound = static_cast<CLayerSounds *>(Map.m_vpGroups[SoundGroup]->m_vpLayers[0].get());
	std::weak_ptr<CLayerGroup> pWeakGroup = Map.m_vpGroups[AlphaGroup];
	std::weak_ptr<CLayer> pWeakLayer = Map.m_vpGroups[AlphaGroup]->m_vpLayers[0];
	std::weak_ptr<CEditorImage> pWeakImage = Map.m_vpImages[0];
	std::weak_ptr<CEditorSound> pWeakSound = Map.m_vpSounds[1];
	std::weak_ptr<CEnvelope> pWeakEnvelope = Map.m_vpEnvelopes[3];
	Require(Map.m_vpSounds[1]->m_SoundId >= 0, "real imported sound sample");
	const int SampleId = Map.m_vpSounds[1]->m_SoundId;
	const auto Texture = Map.m_vpImages[0]->m_Texture;
	const auto AfterGroups = Map.m_vpGroups.size();

	const auto CheckImported = [&]() {
		Require(Map.m_vpGroups.size() == AfterGroups && Map.m_vpGroups[AlphaGroup]->m_vpLayers[0].get() == pImported, "original imported layer reattached");
		Require(Map.m_vpImages == vpAfterImages && pExisting->m_Image == 3, "existing image forward permutation");
		Require(pImported->m_Image == 0 && pDedup->m_Image == 1 && pConflict->m_Image == 2, "imported image indices remain final");
		Require(Map.m_vpEnvelopes.size() == 6 && Map.m_vpSounds.size() == 2, "resource counts");
		Require(pImported->m_vQuads[0].m_PosEnv == 3 && pImported->m_vQuads[0].m_ColorEnv == 4, "imported quad envelope offsets");
		Require(pImportedSound->m_Sound == 1 && pImportedSound->m_vSources[0].m_PosEnv == 3 && pImportedSound->m_vSources[0].m_SoundEnv == 5, "sound and source envelope offsets");
		Require(Map.m_vpSounds[1]->m_SoundId == SampleId && Map.m_vpImages[0]->m_Texture.Id() == Texture.Id(), "original resource handles retained");
		Require(Settings(Map) == vAfterSettings && Map.m_Modified, "redo settings and modified state");
		Map.CheckIntegrity();
	};
	CheckImported();

	Map.SelectLayer(0, AlphaGroup);
	Map.m_vSelectedQuads = {0};
	Map.DeleteSelectedQuads(); // Real later action retains the imported layer.
	Require(pImported->m_vQuads.size() == 1, "later quad deletion executed");
	for(int Cycle = 0; Cycle < 4; ++Cycle)
	{
		Require(Map.m_EditorHistory.Undo() && pImported->m_vQuads.size() == 2, "undo later deletion");
		Map.ResetModifiedState();
		Require(Map.m_EditorHistory.Undo(), "undo append");
		Require(Map.m_vpGroups.size() == BeforeGroups && Map.m_vpImages == vpBeforeImages, "undo counts and image ordering");
		Require(pExisting->m_Image == 1 && pExisting->m_vQuads[0].m_PosEnv == 0, "existing indices restored");
		Require(pImported->m_Image == 0 && pImported->m_vQuads[0].m_PosEnv == 3, "detached imported indices unchanged");
		Require(Map.m_vpSounds.size() == 1 && Map.m_vpEnvelopes.size() == 3 && Settings(Map) == vBeforeSettings, "undo resources and exact settings");
		Require(Map.m_SelectedGroup == 2 && Map.m_vSelectedLayers == std::vector<int>{0} && Map.m_vSelectedQuads == std::vector<int>{1} && Map.m_SelectedImage == 1, "undo selection restored");
		Require(Map.m_Modified && !pWeakLayer.expired() && !pWeakSound.expired(), "detached objects live in history");
		Require(Map.m_EditorHistory.m_vpRedoActions.size() == 2, "later redo entry retained");
		Map.ResetModifiedState();
		Require(Map.m_EditorHistory.Redo(), "redo append without source file");
		CheckImported();
		Require(Map.m_EditorHistory.CanRedo(), "later action still redoable");
		Require(Map.m_EditorHistory.Redo(), "redo later deletion");
		Require(Map.m_vpGroups[AlphaGroup]->m_vpLayers[0].get() == pImported && pImported->m_vQuads.size() == 1, "redo deletion changes attached original layer");
	}

	Require(Map.m_EditorHistory.Undo() && Map.m_EditorHistory.Undo(), "final detach");
	// Drop the snapshot's extra image references before checking history ownership.
	vpAfterImages.clear();
	Map.m_EditorHistory.Clear();
	Require(pWeakGroup.expired() && pWeakLayer.expired() && pWeakImage.expired() && pWeakSound.expired() && pWeakEnvelope.expired(), "history clear releases detached resources");
	Require(!Map.m_EditorHistory.CanUndo() && !Map.m_EditorHistory.CanRedo(), "history clear");
	dbg_msg("editor/append-regression", "PASS: real append, exact settings, four undo/redo cycles, indices, identity, resource ownership");
	return true;
}
