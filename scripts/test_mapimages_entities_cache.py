#!/usr/bin/env python3
"""Compile exact CMapImages methods with in-memory graphics; no client/GPU/build tree.

Run with Python -I. Each run keeps its generated C++, binaries, compiler/run logs,
and JSON results under --output-root. Baseline is read with git show, never checked
out. The fake seam is CImageInfo/IGraphics/CGameInfo/component wiring; GetEntities,
GetTuneColors, ChangeEntitiesPath, constructor, tile predicates and color routines
come verbatim from the selected production source. This does not test rendering,
PNG decoding, real GPU ownership, client integration, or missing-image failures.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FILES = (
    "src/game/client/components/mapimages.cpp",
    "src/game/client/components/mapimages.h",
    "src/game/map/render_interfaces.h",
    "src/game/mapitems.h",
    "src/game/mapitems.cpp",
    "src/engine/gfx/image_manipulation.cpp",
    "src/engine/graphics.h",
)
SCENARIOS = (
    "hot_tune_first", "hot_entities_first", "mod_mask_rebuild",
    "repeated_changes", "legacy_no_arrays", "png_fallbacks",
)

PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <base/color.h>
static const char *g_pScenario = "setup";
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string(g_pScenario) + ": " + #x + " (harness line " + std::to_string(__LINE__) + ")"); } while(false)
#define dbg_assert(x, message) do { if(!(x)) throw std::runtime_error(message); } while(false)
constexpr int IO_MAX_PATH_LENGTH = 1024;
template<typename... T> void str_format(char *pOut, size_t Size, const char *pFormat, T... Args) { std::snprintf(pOut, Size, pFormat, Args...); }
template<size_t N> void str_copy(char (&aOut)[N], const char *pText) { std::snprintf(aOut, N, "%s", pText); }
int str_comp(const char *pA, const char *pB) { return std::strcmp(pA, pB); }
void mem_zero(void *pData, size_t Size) { std::memset(pData, 0, Size); }
struct CImageInfo {
    enum EImageFormat { FORMAT_RGB, FORMAT_RGBA, FORMAT_R, FORMAT_RA };
    size_t m_Width = 0, m_Height = 0;
    EImageFormat m_Format = FORMAT_RGBA;
    uint8_t *m_pData = nullptr;
    static size_t PixelSize(EImageFormat Format) { return Format == FORMAT_RGBA ? 4 : Format == FORMAT_RGB ? 3 : Format == FORMAT_RA ? 2 : 1; }
    size_t PixelSize() const { return PixelSize(m_Format); }
    size_t DataSize() const { return m_Width * m_Height * PixelSize(); }
    void Allocate() { CHECK(!m_pData); m_pData = static_cast<uint8_t *>(std::malloc(DataSize())); CHECK(m_pData); }
    void AllocateFillZero() { Allocate(); mem_zero(m_pData, DataSize()); }
    void Free() { std::free(m_pData); m_pData = nullptr; }
    void CopyRectFrom(const CImageInfo &Source, size_t SrcX, size_t SrcY, size_t Width, size_t Height, size_t DestX, size_t DestY) {
        CHECK(PixelSize() == Source.PixelSize());
        CHECK(SrcX + Width <= Source.m_Width && SrcY + Height <= Source.m_Height);
        CHECK(DestX + Width <= m_Width && DestY + Height <= m_Height);
        for(size_t Y = 0; Y < Height; ++Y)
            std::memcpy(m_pData + ((DestY + Y) * m_Width + DestX) * PixelSize(), Source.m_pData + ((SrcY + Y) * Source.m_Width + SrcX) * PixelSize(), Width * PixelSize());
    }
};
struct IStorage { enum { TYPE_ALL }; };
struct CGameInfo {
    bool m_EntitiesFDDrace = false, m_EntitiesDDNet = false, m_EntitiesDDRace = false;
    bool m_EntitiesRace = false, m_EntitiesBW = false, m_EntitiesFNG = false, m_EntitiesVanilla = false;
    bool m_DontMaskEntities = false;
};
struct CGameClient { CGameInfo m_GameInfo; };
'''

GRAPHICS = r'''
    struct STexture {
        bool m_Live = true, m_Tune = false;
        int m_Flags;
        std::string m_Path;
        std::vector<uint8_t> m_vPixels;
    };
    bool m_Arrays = true;
    int m_NextId = 1;
    std::map<int, STexture> m_Textures;
    std::map<std::string, uint8_t> m_Images;
    std::vector<std::string> m_vPngCalls;
    std::vector<int> m_vUnloads;
    bool HasTextureArraysSupport() const { return m_Arrays; }
    int TextureLoadFlags() const { return TEXLOAD_TO_2D_ARRAY_TEXTURE; }
    bool LoadPng(CImageInfo &Image, const char *pPath, int) {
        CHECK(!Image.m_pData);
        m_vPngCalls.emplace_back(pPath);
        const auto It = m_Images.find(pPath);
        if(It == m_Images.end()) return false;
        Image.m_Width = Image.m_Height = 16;
        Image.m_Format = CImageInfo::FORMAT_RGBA;
        Image.Allocate();
        for(size_t Tile = 0; Tile < 256; ++Tile) {
            Image.m_pData[Tile * 4] = static_cast<uint8_t>(30 + Tile % 97);
            Image.m_pData[Tile * 4 + 1] = static_cast<uint8_t>(50 + Tile % 83);
            Image.m_pData[Tile * 4 + 2] = static_cast<uint8_t>(70 + Tile % 61);
            Image.m_pData[Tile * 4 + 3] = It->second;
        }
        return true;
    }
    CTextureHandle Store(const CImageInfo &Image, int Flags, const char *pPath, bool Tune) {
        CHECK(Image.m_pData && Image.DataSize() == 1024);
        const int Id = m_NextId++;
        m_Textures.emplace(Id, STexture{true, Tune, Flags, pPath ? pPath : "", {Image.m_pData, Image.m_pData + Image.DataSize()}});
        CTextureHandle Handle;
        Handle.m_Id = Id;
        return Handle;
    }
    CTextureHandle LoadTextureRaw(const CImageInfo &Image, int Flags, const char *pPath = nullptr) { return Store(Image, Flags, pPath, false); }
    CTextureHandle LoadTextureRawMove(CImageInfo &Image, int Flags, const char *pPath = nullptr) {
        auto Handle = Store(Image, Flags, pPath, true);
        Image.Free();
        return Handle;
    }
    void UnloadTexture(CTextureHandle *pHandle) {
        if(pHandle->IsValid()) {
            CHECK(Alive(*pHandle));
            m_Textures.at(pHandle->Id()).m_Live = false;
            m_vUnloads.push_back(pHandle->Id());
        }
        pHandle->Invalidate();
    }
    bool Alive(CTextureHandle Handle) const {
        auto It = m_Textures.find(Handle.Id());
        return It != m_Textures.end() && It->second.m_Live;
    }
    size_t LiveCount() const {
        return static_cast<size_t>(std::count_if(m_Textures.begin(), m_Textures.end(), [](const auto &Pair) { return Pair.second.m_Live; }));
    }
};
class CMapImages {
public:
    int m_Count;
    char m_aEntitiesPath[IO_MAX_PATH_LENGTH];
    bool m_aEntitiesIsLoaded[MAP_IMAGE_MOD_TYPE_COUNT * 2];
    bool m_SpeedupArrowIsLoaded, m_TuneColorsIsLoaded;
    IGraphics::CTextureHandle m_aaEntitiesTextures[MAP_IMAGE_MOD_TYPE_COUNT * 2][MAP_IMAGE_ENTITY_LAYER_TYPE_COUNT];
    IGraphics::CTextureHandle m_TuneColorMapTexture;
    IGraphics m_Graphics;
    CGameClient m_GameClient;
    IGraphics *Graphics() { return &m_Graphics; }
    CGameClient *GameClient() { return &m_GameClient; }
    CMapImages();
    IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType);
    IGraphics::CTextureHandle GetTuneColors();
    void ChangeEntitiesPath(const char *pPath);
};
'''

TESTS = r'''
using THandle = IGraphics::CTextureHandle;
constexpr auto GAME = MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH;
constexpr auto SWITCH = MAP_IMAGE_ENTITY_LAYER_TYPE_SWITCH;
uint8_t Stamp(int Profile, int Mod) { return static_cast<uint8_t>(20 + Profile * 60 + Mod * 3); }
std::string PathFor(int Profile, int Mod) {
    return std::string(Profile == 0 ? "editor/entities_clear" : Profile == 1 ? "assets/entities/alpha" : "assets/entities/beta") + "/" + gs_apModEntitiesNames[Mod] + ".png";
}
const char *Selection(int Profile) { return Profile == 0 ? "default" : Profile == 1 ? "alpha" : "beta"; }
void Populate(CMapImages &Map) {
    for(int Profile = 0; Profile < 3; ++Profile)
        for(int Mod = 0; Mod < MAP_IMAGE_MOD_TYPE_COUNT; ++Mod)
            Map.m_Graphics.m_Images.emplace(PathFor(Profile, Mod), Stamp(Profile, Mod));
}
void SelectMod(CMapImages &Map, int Mod, bool Masked) {
    auto &Info = Map.m_GameClient.m_GameInfo;
    Info = {};
    Info.m_DontMaskEntities = !Masked;
    switch(Mod) {
    case MAP_IMAGE_MOD_TYPE_DDNET: Info.m_EntitiesDDNet = true; break;
    case MAP_IMAGE_MOD_TYPE_DDRACE: Info.m_EntitiesDDRace = true; break;
    case MAP_IMAGE_MOD_TYPE_RACE: Info.m_EntitiesRace = true; break;
    case MAP_IMAGE_MOD_TYPE_BLOCKWORLDS: Info.m_EntitiesBW = true; break;
    case MAP_IMAGE_MOD_TYPE_FNG: Info.m_EntitiesFNG = true; break;
    case MAP_IMAGE_MOD_TYPE_VANILLA: Info.m_EntitiesVanilla = true; break;
    case MAP_IMAGE_MOD_TYPE_FDDRACE: Info.m_EntitiesFDDrace = true; break;
    default: CHECK(false);
    }
}
void CheckTexture(CMapImages &Map, THandle Handle, uint8_t Alpha, bool Tune, const std::string &Path = "") {
    CHECK(Map.m_Graphics.Alive(Handle));
    const auto &Texture = Map.m_Graphics.m_Textures.at(Handle.Id());
    CHECK(Texture.m_Tune == Tune);
    CHECK(Texture.m_Flags == (Map.m_Graphics.m_Arrays ? IGraphics::TEXLOAD_TO_2D_ARRAY_TEXTURE | IGraphics::TEXLOAD_NO_2D_TEXTURE : 0));
    CHECK(Texture.m_vPixels[3] == 0);
    if(Tune) {
        for(size_t Tile = 1; Tile < 256; ++Tile)
            CHECK(Texture.m_vPixels[Tile * 4 + 3] == Alpha);
    } else {
        CHECK(Texture.m_Path == Path);
        CHECK(Texture.m_vPixels[TILE_FREEZE * 4 + 3] == Alpha);
        CHECK(Texture.m_vPixels[TILE_FREEZE * 4] == 30 + TILE_FREEZE % 97);
    }
}
void CheckEmpty(CMapImages &Map) {
    CHECK(!Map.m_TuneColorsIsLoaded);
    CHECK(!Map.m_TuneColorMapTexture.IsValid());
    CHECK(Map.m_Graphics.LiveCount() == 0);
    CHECK(Map.m_Graphics.m_vUnloads.size() == Map.m_Graphics.m_Textures.size());
    for(int Slot = 0; Slot < MAP_IMAGE_MOD_TYPE_COUNT * 2; ++Slot) {
        CHECK(!Map.m_aEntitiesIsLoaded[Slot]);
        for(const auto &Texture : Map.m_aaEntitiesTextures[Slot]) CHECK(!Texture.IsValid());
    }
}
void CheckOrdinary(CMapImages &Map, int Profile, int Mod) {
    for(auto Layer : {GAME, SWITCH})
        CheckTexture(Map, Map.GetEntities(Layer), Stamp(Profile, Mod), false, PathFor(Profile, Mod));
}
void HotChange(bool EntitiesFirst) {
    CMapImages Map; Populate(Map); SelectMod(Map, MAP_IMAGE_MOD_TYPE_DDNET, true);
    auto OldTune = Map.GetTuneColors();
    auto OldGame = Map.GetEntities(GAME), OldSwitch = Map.GetEntities(SWITCH);
    CheckTexture(Map, OldTune, Stamp(0, 0), true);
    CheckOrdinary(Map, 0, 0);
    const int Allocated = Map.m_Graphics.m_NextId;
    const size_t Reads = Map.m_Graphics.m_vPngCalls.size();
    CHECK(Map.GetTuneColors().Id() == OldTune.Id());
    CHECK(Map.m_Graphics.m_NextId == Allocated);
    CHECK(Map.m_Graphics.m_vPngCalls.size() == Reads);
    Map.ChangeEntitiesPath("beta");
    CHECK(!Map.m_Graphics.Alive(OldTune));
    CHECK(!Map.m_Graphics.Alive(OldGame) && !Map.m_Graphics.Alive(OldSwitch));
    CheckEmpty(Map);
    if(EntitiesFirst) CheckOrdinary(Map, 2, 0);
    auto NewTune = Map.GetTuneColors();
    CHECK(NewTune.Id() != OldTune.Id());
    CheckTexture(Map, NewTune, Stamp(2, 0), true);
    CheckOrdinary(Map, 2, 0);
    CHECK(Map.m_Graphics.LiveCount() == 3);
    CHECK(Map.m_Graphics.m_vPngCalls.size() == Reads + 1);
    CHECK(Map.m_Graphics.m_NextId == Allocated + 3);
    Map.ChangeEntitiesPath("default"); CheckEmpty(Map);
}
void ModMaskRebuild() {
    CMapImages Map; Populate(Map); Map.ChangeEntitiesPath("alpha");
    THandle Previous;
    size_t Slots = 0;
    for(int Mod = 0; Mod < MAP_IMAGE_MOD_TYPE_COUNT; ++Mod) {
        for(bool Masked : {false, true}) {
            SelectMod(Map, Mod, Masked);
            Map.GetEntities(GAME);
            auto Tune = Map.GetTuneColors();
            if(Previous.IsValid()) CHECK(!Map.m_Graphics.Alive(Previous));
            CHECK(Tune.Id() != Previous.Id());
            CheckTexture(Map, Tune, Stamp(1, Mod), true);
            CheckOrdinary(Map, 1, Mod);
            CHECK(Map.m_Graphics.LiveCount() == ++Slots * 2 + 1);
            const int Allocated = Map.m_Graphics.m_NextId;
            const size_t Reads = Map.m_Graphics.m_vPngCalls.size();
            CHECK(Map.GetTuneColors().Id() == Tune.Id());
            Map.GetEntities(GAME); Map.GetEntities(SWITCH);
            CHECK(Map.m_Graphics.m_NextId == Allocated && Map.m_Graphics.m_vPngCalls.size() == Reads);
            Previous = Tune;
        }
    }
    CHECK(Slots == 14);
    Map.ChangeEntitiesPath("default"); CheckEmpty(Map);
}
void RepeatedChanges() {
    CMapImages Map; Populate(Map);
    for(int Cycle = 0; Cycle < 32; ++Cycle) {
        const int Profile = Cycle % 3, Mod = Cycle % MAP_IMAGE_MOD_TYPE_COUNT;
        Map.ChangeEntitiesPath(Selection(Profile)); CheckEmpty(Map);
        SelectMod(Map, Mod, Cycle % 2 != 0);
        auto Tune = Map.GetTuneColors();
        CheckTexture(Map, Tune, Stamp(Profile, Mod), true); CheckOrdinary(Map, Profile, Mod);
        CHECK(Map.m_Graphics.LiveCount() == 3);
        const int Allocated = Map.m_Graphics.m_NextId;
        CHECK(Map.GetTuneColors().Id() == Tune.Id());
        CHECK(Map.m_Graphics.m_NextId == Allocated);
        Map.ChangeEntitiesPath(Selection(Profile)); CheckEmpty(Map);
        Map.ChangeEntitiesPath(Selection(Profile)); CheckEmpty(Map);
    }
}
void Legacy() {
    CMapImages Map; Populate(Map); Map.m_Graphics.m_Arrays = false;
    for(int Profile = 0; Profile < 3; ++Profile) {
        Map.ChangeEntitiesPath(Selection(Profile)); CheckEmpty(Map);
        size_t Slots = 0;
        for(int Mod = 0; Mod < MAP_IMAGE_MOD_TYPE_COUNT; ++Mod) {
            for(bool Masked : {false, true}) {
                SelectMod(Map, Mod, Masked);
                auto Tune = Map.GetTuneColors();
                CHECK(Tune.Id() == Map.GetEntities(GAME).Id());
                CheckOrdinary(Map, Profile, Mod);
                CHECK(!Map.m_TuneColorsIsLoaded && !Map.m_TuneColorMapTexture.IsValid());
                CHECK(Map.m_Graphics.LiveCount() == ++Slots * 2);
                const int Allocated = Map.m_Graphics.m_NextId;
                CHECK(Map.GetTuneColors().Id() == Tune.Id());
                CHECK(Map.m_Graphics.m_NextId == Allocated);
            }
        }
    }
    Map.ChangeEntitiesPath("default"); CheckEmpty(Map);
}
void Fallbacks() {
    {
        CMapImages Map; Map.m_Graphics.m_Images.emplace("assets/entities/single.png", 201);
        SelectMod(Map, MAP_IMAGE_MOD_TYPE_DDNET, true); Map.ChangeEntitiesPath("single");
        CheckTexture(Map, Map.GetTuneColors(), 201, true);
        CheckTexture(Map, Map.GetEntities(GAME), 201, false, "assets/entities/single.png");
        CHECK((Map.m_Graphics.m_vPngCalls == std::vector<std::string>{"assets/entities/single/ddnet.png", "assets/entities/single.png"}));
        Map.ChangeEntitiesPath("default"); CheckEmpty(Map);
    }
    {
        CMapImages Map; Map.m_Graphics.m_Images.emplace("editor/entities_clear/fng.png", 202);
        SelectMod(Map, MAP_IMAGE_MOD_TYPE_FNG, true); Map.ChangeEntitiesPath("missing");
        CheckTexture(Map, Map.GetTuneColors(), 202, true);
        CheckTexture(Map, Map.GetEntities(GAME), 202, false, "editor/entities_clear/fng.png");
        CHECK((Map.m_Graphics.m_vPngCalls == std::vector<std::string>{"assets/entities/missing/fng.png", "editor/entities_clear/fng.png"}));
        Map.ChangeEntitiesPath("default"); CheckEmpty(Map);
    }
}
int main(int Argc, char **ppArgv) {
    try {
        CHECK(Argc == 2); g_pScenario = ppArgv[1];
        const std::string Name = g_pScenario;
        if(Name == "hot_tune_first") HotChange(false);
        else if(Name == "hot_entities_first") HotChange(true);
        else if(Name == "mod_mask_rebuild") ModMaskRebuild();
        else if(Name == "repeated_changes") RepeatedChanges();
        else if(Name == "legacy_no_arrays") Legacy();
        else if(Name == "png_fallbacks") Fallbacks();
        else CHECK(false);
        std::printf("PASS %s\n", g_pScenario); return 0;
    } catch(const std::exception &Error) {
        std::fprintf(stderr, "FAIL %s\n", Error.what()); return 1;
    }
}
'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def extract(text, pattern, description):
    matches = list(re.finditer(pattern, text, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise RuntimeError(f"expected one {description}, found {len(matches)}")
    return matches[0].group(0)


def enum_with(text, token):
    matches = [match.group(0) for match in re.finditer(r"^enum[^\n]*\n\{.*?^\};", text, re.MULTILINE | re.DOTALL)
               if re.search(r"\b" + re.escape(token) + r"\b", match.group(0))]
    if len(matches) != 1:
        raise RuntimeError(f"expected one enum containing {token}")
    return matches[0]


def generate(sources):
    cpp, header, interface, tiles, predicates, colors, graphics = (sources[name] for name in FILES)
    exact = []

    def method(source, name, signature):
        raw = extract(source, r"^" + signature + r"\s*\n\{.*?^\}", name)
        exact.append({"symbol": name, "sha256": digest(raw.encode())})
        return raw

    declarations = "\n".join((
        enum_with(header, "MAP_IMAGE_MOD_TYPE_DDNET"),
        extract(header, r"^constexpr const char \*const gs_apModEntitiesNames\[\] = \{.*?^\};", "mod names"),
        enum_with(interface, "MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH"),
        enum_with(tiles, "ENTITY_NULL"), enum_with(tiles, "TILE_AIR"),
    ))
    handle = extract(graphics, r"\tclass CTextureHandle\s*\n\t\{.*?^\t\};", "texture handle")
    flags = extract(graphics, r"\tenum\s*\n\t\{\s*\n\t\tTEXLOAD_TO_3D_TEXTURE.*?^\t\t?\};", "texture flags")
    helpers = []
    # Forward declaration is needed by IsValidGameTile and IsValidSwitchTile.
    helpers.append("bool IsValidEntity(int Index);")
    for name in ("IsValidGameTile", "IsValidFrontTile", "IsValidSpeedupTile", "IsValidTeleTile", "IsValidSwitchTile", "IsValidTuneTile", "IsValidEntity", "IsCreditsTile"):
        helpers.append(method(predicates, name, r"bool " + name + r"\(int [^\n]*\)"))
    for name, signature in (
        ("ConvertToGrayscalePixel", r"static inline void ConvertToGrayscalePixel\([^\n]*\)"),
        ("ConvertToGrayscaleRect", r"void ConvertToGrayscaleRect\([^\n]*\)"),
        ("ColorizeWithHueRect", r"void ColorizeWithHueRect\([^\n]*\)"),
    ):
        helpers.append(method(colors, name, signature))
    for name, signature in (
        ("CMapImages", r"CMapImages::CMapImages\(\)"),
        ("GetEntitiesModType", r"static EMapImageModType GetEntitiesModType\([^\n]*\)"),
        ("IsValidTile", r"static bool IsValidTile\([^\n]*\)"),
        ("GetEntities", r"IGraphics::CTextureHandle CMapImages::GetEntities\([^\n]*\)"),
        ("GetTuneColors", r"IGraphics::CTextureHandle CMapImages::GetTuneColors\(\)"),
        ("ChangeEntitiesPath", r"void CMapImages::ChangeEntitiesPath\([^\n]*\)"),
    ):
        helpers.append(method(cpp, name, signature))
    return PREAMBLE + declarations + "\nclass IGraphics {\npublic:\n" + flags + "\n" + handle + GRAPHICS + "\n" + "\n".join(helpers) + TESTS, exact


def find_compiler():
    compiler = shutil.which("cl") if os.name == "nt" else next((p for n in ("c++", "g++", "clang++") if (p := shutil.which(n))), None)
    if compiler:
        return compiler, None
    if os.name != "nt":
        raise RuntimeError("requires a C++20 compiler")
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], capture_output=True, text=True, timeout=15, check=True).stdout.strip()
    vcvars = Path(found) / "VC/Auxiliary/Build/vcvars64.bat"
    if not found or not vcvars.is_file():
        raise RuntimeError("MSVC x64 setup not found")
    return "cl", vcvars


def run_variant(root, directory, ref, compiler, vcvars):
    directory.mkdir()
    sources = {}
    for name in FILES:
        sources[name] = (subprocess.run(["git", "-C", str(root), "show", f"{ref}:{name}"], capture_output=True, text=True, encoding="utf-8", timeout=15, check=True).stdout
                         if ref else (root / name).read_text(encoding="utf-8"))
    harness, exact = generate(sources)
    cpp = directory / "mapimages_harness.cpp"
    cpp.write_text(harness, encoding="utf-8")
    exe = directory / ("mapimages_harness.exe" if os.name == "nt" else "mapimages_harness")
    if os.name == "nt":
        args = [compiler, "/nologo", "/std:c++20", "/EHsc", "/utf-8", "/W4", "/Od", "/I" + str(root / "src"), str(cpp), "/Fo:" + str(directory / "mapimages_harness.obj"), "/Fe:" + str(exe)]
    else:
        args = [compiler, "-std=c++20", "-Wall", "-Wextra", "-I" + str(root / "src"), str(cpp), "-o", str(exe)]
    if vcvars:
        batch = directory / "compile.cmd"
        batch.write_text('@call "' + str(vcvars) + '" >nul\n@if errorlevel 1 exit /b %errorlevel%\n@' + subprocess.list2cmdline(args) + "\n", encoding="utf-8")
        command = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(batch)]
    else:
        command = args
    compiled = subprocess.run(command, cwd=directory, capture_output=True, text=True, errors="replace", timeout=90)
    (directory / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    result = {"ref": ref or "working_tree", "compiled": compiled.returncode == 0,
              "compiler_command": args, "exact_extracted_methods": exact,
              "source_sha256": {name: digest(text.encode()) for name, text in sources.items()}, "scenarios": []}
    if compiled.returncode == 0:
        for scenario in SCENARIOS:
            completed = subprocess.run([str(exe), scenario], cwd=directory, capture_output=True, text=True, errors="replace", timeout=15)
            log = completed.stdout + completed.stderr
            (directory / (scenario + ".log")).write_text(log, encoding="utf-8")
            result["scenarios"].append({"name": scenario, "passed": completed.returncode == 0, "exit_code": completed.returncode, "output": log.strip()})
    (directory / "results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--output-root", type=Path, default=Path("D:/kehuduan/audit-fixes-20261105/mapimages"))
    parser.add_argument("--baseline-ref", default="e713802")
    parser.add_argument("--skip-baseline", action="store_true")
    args = parser.parse_args()
    root = args.source_root.resolve()
    output = args.output_root.resolve() / (datetime.datetime.now().strftime("run-%Y%m%d-%H%M%S-") + str(os.getpid()))
    output.mkdir(parents=True, exist_ok=False)
    watched = FILES + ("src/base/color.h", "src/base/math.h")
    before = {name: digest((root / name).read_bytes()) for name in watched}
    summary = {"output": str(output), "scope": "exact production methods with fake image/graphics/component wiring", "limitations": ["No real PNG decoder, graphics backend, GPU or client integration", "Synthetic 16x16 RGBA fixtures with alpha provenance stamps", "No missing-all-images failure-path assertion", "Mod/mask tests cover uncached slots and repeated same-slot access; not a renderer/UI test"], "variants": []}
    try:
        compiler, vcvars = find_compiler()
        summary["variants"].append(run_variant(root, output / "current", None, compiler, vcvars))
        if not args.skip_baseline:
            summary["variants"].append(run_variant(root, output / "baseline", args.baseline_ref, compiler, vcvars))
    except Exception as error:
        summary["error"] = str(error)
    finally:
        after = {name: digest((root / name).read_bytes()) for name in watched}
        summary["production_unchanged"] = before == after
        summary["production_before_sha256"] = before
        summary["production_after_sha256"] = after
        (output / "results.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print("RESULTS=" + str(output / "results.json"))
    for variant in summary["variants"]:
        passed = sum(case["passed"] for case in variant["scenarios"])
        print(f'{variant["ref"]}: compiled={variant["compiled"]}, passed={passed}/{len(SCENARIOS)}')
        for case in variant["scenarios"]:
            if not case["passed"]:
                print(case["output"])
    print("production_unchanged=" + str(summary["production_unchanged"]))
    if "error" in summary:
        print("ERROR=" + summary["error"])
    current_green = bool(summary["variants"]) and summary["variants"][0]["compiled"] and all(case["passed"] for case in summary["variants"][0]["scenarios"]) and len(summary["variants"][0]["scenarios"]) == len(SCENARIOS)
    baseline_valid = args.skip_baseline or (len(summary["variants"]) == 2 and summary["variants"][1]["compiled"] and any(not case["passed"] for case in summary["variants"][1]["scenarios"]))
    return 0 if current_green and baseline_valid and summary["production_unchanged"] and "error" not in summary else 1


if __name__ == "__main__":
    sys.exit(main())
