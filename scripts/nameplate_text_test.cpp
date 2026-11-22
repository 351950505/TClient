// Production functions are extracted at runtime by run_nameplate_text_test.js.
// FreeType and font files are real; GPU calls are recorded, never executed.
#include <base/color.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/vmath.h>
#include <cstdarg>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
#define dbg_assert(condition, ...) do { if(!(condition)) throw std::runtime_error(#__VA_ARGS__); } while(0)
#define log_debug(...) ((void)0)
#define log_info(...) ((void)0)
#define log_error(...) ((void)0)
#define log_warn(...) ((void)0)
void mem_copy(void *pDst, const void *pSrc, size_t Size) { std::memcpy(pDst, pSrc, Size); }
int str_length(const char *pText) { return (int)std::strlen(pText); }
int str_comp(const char *pA, const char *pB) { return std::strcmp(pA, pB); }
int str_copy(char *pDst, const char *pSrc, int Size) { if(Size > 0) return std::snprintf(pDst, Size, "%s", pSrc ? pSrc : ""); return 0; }
void str_append(char *pDst, const char *pSrc, int Size) { const int Used = str_length(pDst); if(Used < Size) str_copy(pDst + Used, pSrc, Size - Used); }
#undef str_format
int str_format_int(char *pDst, size_t Size, int Value) { return std::snprintf(pDst, Size, "%d", Value); }
int str_utf8_isstart(char C) { return (C & 0xC0) != 0x80; }
int str_format(char *pDst, int Size, const char *pFormat, ...) { va_list Args; va_start(Args, pFormat); int Ret = std::vsnprintf(pDst, Size, pFormat, Args); va_end(Args); return Ret; }
// Unicode decoding/rewind implementations are extracted from production too.
#include "nameplate_text_strings.h"
#include "nameplate_text_types.h"
#include "nameplate_text_screen.h"
struct SBufferContainerInfo { int m_VertBufferBindingIndex = -1; };
class SRecordedVertex { public: float x, y, u, v; };
class SRecordedDraw { public: std::vector<SRecordedVertex> vertices; };
using VkBuffer = size_t;
using VkDeviceMemory = size_t;
using VkDeviceSize = size_t;
using VkBufferUsageFlagBits = int;
constexpr size_t VK_NULL_HANDLE = 0, VK_WHOLE_SIZE = SIZE_MAX;
constexpr int VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE = 0, VK_SUCCESS = 0, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT = 0, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 0;
constexpr size_t MAIN_THREAD_INDEX = 0, STAGING_BUFFER_CACHE_ID = 0, VERTEX_BUFFER_CACHE_ID = 1;
struct VkMappedMemoryRange { int sType; size_t memory, offset, size; };
struct SDeviceMemoryBlock { size_t m_Mem = 0, m_Size = 0; };
template<size_t Id> struct SMemoryBlock { VkBuffer m_Buffer = 0; struct { size_t m_OffsetToAlign = 0; } m_HeapData; SDeviceMemoryBlock m_BufferMem; };
static std::map<size_t, std::vector<uint8_t>> s_Memory;
static int vkMapMemory(int, size_t Mem, int, size_t, int, void **ppData) { *ppData = s_Memory.at(Mem).data(); return VK_SUCCESS; }
static void vkFlushMappedMemoryRanges(int, size_t, const VkMappedMemoryRange *) {}
struct GL_SVertexTex3DStream { unsigned char m_Data[24]; };
struct CCommandBuffer { static constexpr size_t MAX_VERTICES = 32768; struct SState { int m_Texture = 0; }; };
struct SRenderCommandExecuteBuffer { VkBuffer m_Buffer = 0; size_t m_BufferOff = 0; VkBuffer m_IndexBuffer = 0; size_t m_EstimatedRenderCallCount = 0; std::array<int, 1> m_aDescriptors{}; };
class CVulkanMemoryTest;
class IGraphics
{
public:
	struct CTextureHandle { int m_Id = -1; int Id() const { return m_Id; } };
	struct CQuadItem { float m_X = 0, m_Y = 0, m_Width = 0, m_Height = 0; CQuadItem() = default; CQuadItem(float X, float Y, float Width, float Height) : m_X(X), m_Y(Y), m_Width(Width), m_Height(Height) {} };
	enum EBufferObjectCreateFlags { BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT = 1 };
	CVulkanMemoryTest *m_pVulkan = nullptr;
	bool m_Buffered = true;
	CScreenRect m_Screen = CScreenRect(vec2(0, 0), vec2(1920, 1080));
	vec2 m_Pixels = vec2(1920, 1080);
	std::vector<std::vector<unsigned char>> m_Buffers;
	std::vector<int> m_Bindings;
	std::vector<int> m_BufferFlags;
	std::vector<SRecordedDraw> m_Draws;
	std::vector<SRecordedVertex> m_Immediate;
	std::array<float, 4> m_UV{};
	CScreenRect GetScreen() const { return m_Screen; }
	vec2 ScreenSize() const { return m_Pixels; }
	int ScreenWidth() const { return (int)m_Pixels.x; }
	void MapScreen(CScreenRect Screen) { m_Screen = Screen; }
	void MapScreenToInterface(float X, float Y) { const float H = 1000, W = H * m_Pixels.x / m_Pixels.y; m_Screen = CScreenRect(vec2(X - W / 2, Y - H / 2), vec2(X + W / 2, Y + H / 2)); }
	bool IsTextBufferingEnabled() const { return m_Buffered; }
	void TextureClear() {}
	void TextureSet(CTextureHandle) {}
	void FlushVertices() {}
	void SetColor(float, float, float, float) {}
	void SetColor(ColorRGBA) {}
	void QuadsBegin() { m_Immediate.clear(); }
	void QuadsSetSubset(float U0, float V0, float U1, float V1) { m_UV = {U0, V0, U1, V1}; }
	void QuadsDrawTL(const CQuadItem *pItems, size_t Count)
	{
		for(size_t i = 0; i < Count; ++i)
		{
			const auto &q = pItems[i];
			m_Immediate.push_back(Project(q.m_X, q.m_Y, m_UV[0], m_UV[1]));
			m_Immediate.push_back(Project(q.m_X + q.m_Width, q.m_Y, m_UV[2], m_UV[1]));
			m_Immediate.push_back(Project(q.m_X + q.m_Width, q.m_Y + q.m_Height, m_UV[2], m_UV[3]));
			m_Immediate.push_back(Project(q.m_X, q.m_Y + q.m_Height, m_UV[0], m_UV[3]));
		}
	}
	void QuadsEndKeepVertices() { m_Draws.push_back({m_Immediate}); }
	void QuadsEnd() { m_Draws.push_back({m_Immediate}); }
	void QuadsDrawCurrentVertices(bool) {}
	void ChangeColorOfQuadVertices(int, unsigned char, unsigned char, unsigned char, unsigned char) {}
	void QuadContainerReset(int) {}
	int CreateQuadContainer(bool) { return 1; }
	void QuadContainerAddQuads(int, const CQuadItem *, size_t) {}
	void QuadContainerUpload(int) {}
	void RenderQuadContainerEx(int, int, int, float, float) {}
	void DeleteQuadContainer(int &) {}
	void IndicesNumRequiredNotify(size_t) {}
	bool LoadTextTextures(size_t, size_t, CTextureHandle &Fill, CTextureHandle &Outline, uint8_t *pFill, uint8_t *pOutline) { Fill.m_Id = 0; Outline.m_Id = 1; free(pFill); free(pOutline); return true; }
	bool UnloadTextTextures(CTextureHandle &, CTextureHandle &) { return true; }
	bool UpdateTextTexture(CTextureHandle, int, int, size_t, size_t, uint8_t *pData, bool Moved) { if(Moved) free(pData); return true; }
	int CreateBufferObject(size_t Size, void *pData, int Flags)
	{
		int Index = (int)m_Buffers.size(); m_Buffers.emplace_back(); m_BufferFlags.push_back(Flags); RecreateBufferObject(Index, Size, pData, Flags); return Index;
	}
	void RecreateBufferObject(int Index, size_t Size, void *pData, int Flags);
	int CreateBufferContainer(SBufferContainerInfo *pInfo) { m_Bindings.push_back(pInfo->m_VertBufferBindingIndex); return (int)m_Bindings.size() - 1; }
	void DeleteBufferContainer(int &Index, bool) { Index = -1; }
	SRecordedVertex Project(float X, float Y, float U, float V) const { const vec2 P = (vec2(X, Y) - m_Screen.m_TopLeft) / m_Screen.Size() * m_Pixels; return {P.x, P.y, U, V}; }
	void RenderText(int Container, int Count, int TexSize, int, int, const ColorRGBA &, const ColorRGBA &);
};
class CVulkanMemoryTest
{
public:
	enum class EMemoryBlockUsage { STREAM };
	enum class EGfxErrorType { GFX_ERROR_TYPE_OUT_OF_MEMORY_BUFFER };
	int m_VKDevice = 0;
	size_t m_CurImageIndex = 0, m_NonCoherentMemAlignment = 1;
	VkBuffer m_RenderIndexBuffer = 0;
	struct STexture { std::array<int, 1> m_aVKStandardTexturedDescrSets{}; };
	std::array<STexture, 1> m_vTextures{};
	void SetError(EGfxErrorType, const char *) { throw std::runtime_error("stream allocation failure"); }
	bool GetIsTextured(const CCommandBuffer::SState &) const { return false; }
	size_t GetAddressModeIndex(const CCommandBuffer::SState &) const { return 0; }
	void ExecBufferFillDynamicStates(const CCommandBuffer::SState &, SRenderCommandExecuteBuffer &) {}
	bool CreateBuffer(size_t Size, EMemoryBlockUsage, int, int, VkBuffer &Buffer, SDeviceMemoryBlock &Mem)
	{
		Buffer = s_Memory.size() + 1; s_Memory[Buffer].resize(Size); Mem.m_Mem = Buffer; Mem.m_Size = Size; return true;
	}
	template<size_t Id> bool GetStagingBuffer(SMemoryBlock<Id> &Mem, const void *pData, size_t Size)
	{
		CreateBuffer(Size, EMemoryBlockUsage::STREAM, 0, 0, Mem.m_Buffer, Mem.m_BufferMem); mem_copy(s_Memory.at(Mem.m_Buffer).data(), pData, Size); return true;
	}
	template<size_t Id> bool GetVertexBuffer(SMemoryBlock<Id> &Mem, size_t Size) { return CreateBuffer(Size, EMemoryBlockUsage::STREAM, 0, 0, Mem.m_Buffer, Mem.m_BufferMem); }
	template<size_t Id> void FreeVertexMemBlock(SMemoryBlock<Id> &) {}
	template<size_t Id> void UploadAndFreeStagingMemBlock(SMemoryBlock<Id> &) {}
	bool MemoryBarrier(VkBuffer, size_t, size_t, int, bool) { return true; }
	bool CopyBuffer(VkBuffer Src, VkBuffer Dst, size_t SrcOffset, size_t DstOffset, size_t Size) { mem_copy(s_Memory.at(Dst).data() + DstOffset, s_Memory.at(Src).data() + SrcOffset, Size); return true; }
#include "nameplate_text_vulkan.h"
	std::vector<SBufferObjectFrame> m_vBufferObjects;
	std::vector<SBufferContainer> m_vBufferContainers;
	std::vector<SStreamMemory<SFrameBuffers>> m_vStreamedVertexBuffers;
	CVulkanMemoryTest() { m_vStreamedVertexBuffers.resize(1); m_vStreamedVertexBuffers[0].Init(2); }
	void EndFrame() { UploadStreamedBuffer<true>(m_vStreamedVertexBuffers[0]); m_CurImageIndex = (m_CurImageIndex + 1) % 2; }
};
void IGraphics::RecreateBufferObject(int Index, size_t Size, void *pData, int Flags)
{
	const auto *p = static_cast<unsigned char *>(pData); m_Buffers.at(Index).assign(p, p + Size); m_BufferFlags.at(Index) = Flags;
	if(m_pVulkan) { if((size_t)Index < m_pVulkan->m_vBufferObjects.size()) m_pVulkan->DeleteBufferObject(Index); dbg_assert(m_pVulkan->CreateBufferObject(Index, pData, Size, (Flags & BUFFER_OBJECT_CREATE_FLAGS_ONE_TIME_USE_BIT) != 0), "Vulkan create failed"); }
}
#include "nameplate_text_glyphs.h"
struct STestConfig
{
	int m_TcFontSize = 100;
	int m_TcWarList = 0;
	int m_TcGradientName = 1;
	int m_TcGradientClan = 1;
} g_Config;
std::chrono::nanoseconds time_get_nanoseconds() { return 1000ms; }
#include "nameplate_text_impl.h"
void IGraphics::RenderText(int Container, int Count, int TexSize, int, int, const ColorRGBA &, const ColorRGBA &)
{
	std::vector<unsigned char> VulkanBytes;
	if(m_pVulkan)
	{
		m_pVulkan->m_vBufferContainers.resize(m_Bindings.size());
		m_pVulkan->m_vBufferContainers.at(Container).m_BufferObjectIndex = m_Bindings.at(Container);
		SRenderCommandExecuteBuffer Exec;
		m_pVulkan->BufferContainer_FillExecuteBuffer(Exec, {}, Container, 1);
		const auto &Memory = s_Memory.at(Exec.m_Buffer);
		VulkanBytes.assign(Memory.begin() + Exec.m_BufferOff, Memory.begin() + Exec.m_BufferOff + sizeof(STextCharQuad) * Count);
	}
	const auto &bytes = m_pVulkan ? VulkanBytes : m_Buffers.at(m_Bindings.at(Container));
	dbg_assert(bytes.size() >= sizeof(STextCharQuad) * Count, "Buffer size mismatch");
	SRecordedDraw Draw;
	for(int i = 0; i < Count; ++i)
	{
		STextCharQuad Quad; std::memcpy(&Quad, bytes.data() + i * sizeof(Quad), sizeof(Quad));
		for(const auto &V : Quad.m_aVertices) Draw.vertices.push_back(Project(V.m_X, V.m_Y, V.m_U / TexSize, V.m_V / TexSize));
	}
	m_Draws.push_back(std::move(Draw));
}
class CTeeRenderInfo;
#include "nameplate_text_gradient.h"
class CGameClient;
class CTClientGradient
{
public:
	static bool AppliesTo(int, CGameClient *) { return true; }
	static float AnimatePhase(double Time) { return TClientGradient::AnimatePhase(Time, 100); }
	static int PhaseBucket(float Phase) { return TClientGradient::PhaseBucket(Phase); }
	static std::vector<STextColorSplit> BuildAnimatedTextSplits(const char *pText, int, CGameClient *, float Phase) { return TClientGradient::BuildRainbowColorSplits(pText, Phase); }
};
constexpr int MAX_NAME_LENGTH = 64, MAX_CLAN_LENGTH = 64, MAX_CLIENTS = 128;
namespace protocol7 { constexpr int MAX_NAME_ARRAY_SIZE = 64, MAX_CLAN_ARRAY_SIZE = 64; }
namespace FontIcon { constexpr const char *HEART = "♥"; }
class CGameClient
{
public:
	CTextRender *m_pTextRender;
	IGraphics *m_pGraphics;
	struct SClock { double m_Time = 0; double GlobalTime() const { return m_Time; } } m_Clock;
	struct SCamera { vec2 m_Center = vec2(0, 0); } m_Camera;
	struct SWarList
	{
		struct SWarData { bool m_WarName = false, m_WarClan = false; } m_Data;
		const SWarData &GetWarData(int) const { return m_Data; }
		ColorRGBA GetNameplateColor(int) const { return ColorRGBA(1, 0, 0, 1); }
		ColorRGBA GetClanColor(int) const { return ColorRGBA(1, 0, 0, 1); }
	} m_WarList;
	CTextRender *TextRender() { return m_pTextRender; }
	IGraphics *Graphics() { return m_pGraphics; }
	SClock *Client() { return &m_Clock; }
};
#include "nameplate_text_nameplates.h"
static void Check(bool Value, const std::string &What) { if(!Value) throw std::runtime_error(What); }
static bool Near(float A, float B) { return std::abs(A - B) <= 0.002f; }
static void Compare(const SRecordedDraw &A, const SRecordedDraw &B, const std::string &Case)
{
	Check(A.vertices.size() == B.vertices.size(), Case + ": vertex count");
	for(size_t i = 0; i < A.vertices.size(); ++i)
	{
		const auto &a = A.vertices[i], &b = B.vertices[i];
		if(!Near(a.x, b.x) || !Near(a.y, b.y) || !Near(a.u, b.u) || !Near(a.v, b.v))
		{
			std::cerr << Case << " vertex " << i << " actual " << a.x << "," << a.y << " expected " << b.x << "," << b.y << "\n";
			throw std::runtime_error(Case + ": glyph displaced or UV changed");
		}
	}
}
static void RenderDifferential(CTextRender &TR, IGraphics &G, const char *pText, unsigned Flags, int Iterations)
{
	STextContainerIndex Soft;
	TR.SetRenderFlags(Flags);
	CTextCursor Initial; Initial.m_FontSize = 28.0f; Initial.m_vColorSplits = TClientGradient::BuildRainbowColorSplits(pText, 0);
	TR.CreateTextContainer(Soft, &Initial, pText);
	for(int i = 0; i < Iterations; ++i)
	{
		G.m_Pixels = vec2(1280 + (i % 3) * 640, 720 + (i % 3) * 360);
		G.MapScreenToInterface(1400.25f + i * 43, -500.125f - i * 9);
		g_Config.m_TcFontSize = 80 + (i % 6) * 20;
		CTextCursor Cursor; Cursor.m_FontSize = 18 + (i % 5) * 4.5f; Cursor.m_vColorSplits = TClientGradient::BuildRainbowColorSplits(pText, (i % 64) / 64.0f);
		CTextCursor HardCursor = Cursor;
		TR.RecreateTextContainerSoft(Soft, &Cursor, pText);
		STextContainerIndex Hard; TR.CreateTextContainer(Hard, &HardCursor, pText);
		const auto SoftBox = TR.GetBoundingBoxTextContainer(Soft), HardBox = TR.GetBoundingBoxTextContainer(Hard);
		Check(Near(SoftBox.m_W, HardBox.m_W) && Near(SoftBox.m_H, HardBox.m_H), "soft/hard bbox changed");
		for(int pass = 0; pass < 3; ++pass)
		{
			const float X = i * 2.625f - pass * 554.875f, Y = -250.125f + i * 19 + pass * 471.625f;
			const auto Before = G.GetScreen();
			TR.RenderTextContainer(Soft, ColorRGBA(1, 1, 1, 1), ColorRGBA(0, 0, 0, 0.5f), X, Y);
			const auto A = G.m_Draws.back();
			Check(G.GetScreen().m_TopLeft == Before.m_TopLeft && G.GetScreen().m_BottomRight == Before.m_BottomRight, "MapScreen not restored");
			TR.RenderTextContainer(Hard, ColorRGBA(1, 1, 1, 1), ColorRGBA(0, 0, 0, 0.5f), X, Y);
			Compare(A, G.m_Draws.back(), "soft/hard render");
		}
		TR.DeleteTextContainer(Hard);
		G.m_Draws.clear();
	}
	TR.DeleteTextContainer(Soft);
}
static void NameplateDifferential(CTextRender &TR, IGraphics &G)
{
	CGameClient GC; GC.m_pTextRender = &TR; GC.m_pGraphics = &G;
	CNamePlateData Data{}; Data.m_InGame = true; Data.m_ShowName = true; Data.m_ShowClan = true; Data.m_ShowClientId = true; Data.m_ShowFriendMark = true; Data.m_ClientId = 12; Data.m_FontSizeClientId = Data.m_FontSize = 28; Data.m_FontSizeClan = 24; Data.m_Color = ColorRGBA(1, 1, 1, 1);
	str_copy(Data.m_aName, "AV昵称🙂j"); str_copy(Data.m_aClan, "Clan氏族");
	CNamePlate Cached;
	for(int i = 0; i < 128; ++i)
	{
		GC.m_Clock.m_Time = i / 64.0;
		GC.m_Camera.m_Center = vec2(142.125f + i * 24, -874.5f + i * 13);
		G.MapScreenToInterface(GC.m_Camera.m_Center.x, GC.m_Camera.m_Center.y);
		const auto Screen = G.GetScreen();
		Data.m_FontSize = Data.m_FontSizeClientId = 18 + (i % 5) * 5;
		g_Config.m_TcFontSize = 100;
		g_Config.m_TcGradientName = g_Config.m_TcGradientClan = 1;
		Cached.Update(GC, Data);
		const vec2 Pos(i * 73.75f - 855.25f, i * -42.5f - 964.125f);
		G.m_Draws.clear(); Cached.Render(GC, Pos); const auto Actual = G.m_Draws;
		Check(G.GetScreen().m_TopLeft == Screen.m_TopLeft, "cached plate screen changed");
		g_Config.m_TcGradientName = g_Config.m_TcGradientClan = 0;
		CNamePlate Hard(GC, Data); G.m_Draws.clear(); Hard.Render(GC, Pos);
		Check(Actual.size() == G.m_Draws.size(), "nameplate part count changed");
		for(size_t j = 0; j < Actual.size(); ++j) Compare(Actual[j], G.m_Draws[j], "gradient vs ordinary nameplate");
		Hard.Reset(GC);
		// Same cached player rendered at a second position without recreation.
		g_Config.m_TcGradientName = g_Config.m_TcGradientClan = 1;
		G.m_Draws.clear(); Cached.Update(GC, Data); Cached.Render(GC, Pos + vec2(520.75f, 991.125f));
	}
	Cached.Reset(GC); G.m_Draws.clear();
}
static void VulkanLifetime(CTextRender &TR, IGraphics &G)
{
	CVulkanMemoryTest Vulkan; G.m_pVulkan = &Vulkan; G.m_Buffered = true;
	CGameClient GC; GC.m_pTextRender = &TR; GC.m_pGraphics = &G;
	CNamePlateData Data{}; Data.m_InGame = true; Data.m_ShowName = Data.m_ShowClan = true; Data.m_ClientId = 7; Data.m_FontSize = 28; Data.m_FontSizeClan = 24; Data.m_Color = ColorRGBA(1, 1, 1, 1); str_copy(Data.m_aName, "NickAV"); str_copy(Data.m_aClan, "ClanjT");
	g_Config.m_TcGradientName = g_Config.m_TcGradientClan = 1;
	g_Config.m_TcFontSize = 100; G.MapScreenToInterface(1000, -600);
	CNamePlate Cached(GC, Data); const vec2 Pos(1234.5f, -427.25f);
	G.m_Draws.clear(); Cached.Render(GC, Pos); const auto Baseline = G.m_Draws;
	Check(Baseline.size() == 2, "lifetime expected name and clan");
	for(int i = 0; i < 7; ++i)
	{
		Vulkan.EndFrame();
		// Unrelated transient geometry in the same main-thread stream cache.
		// Production allocator/reset determines whether this reuses nickname bytes.
		std::vector<STextCharQuad> Other(256);
		for(auto &Q : Other) for(auto &V : Q.m_aVertices) { V.m_X = -4200.0f + i * 17; V.m_Y = 2600.0f - i * 9; V.m_U = V.m_V = 0; }
		Check(Vulkan.CreateBufferObject(10000 + i, Other.data(), Other.size() * sizeof(STextCharQuad), true), "transient create failed");
		// Stay within the same phase bucket: no nickname rebuild is requested.
		GC.m_Clock.m_Time = i * 0.0001; Cached.Update(GC, Data);
		G.m_Draws.clear(); Cached.Render(GC, Pos);
		Check(Baseline.size() == G.m_Draws.size(), "lifetime draw count");
		for(size_t Part = 0; Part < Baseline.size(); ++Part) Compare(G.m_Draws[Part], Baseline[Part], std::string("Vulkan cached ") + (Part == 0 ? "nickname" : "clan") + " frame " + std::to_string(i + 1));
	}
	Cached.Reset(GC); G.m_pVulkan = nullptr;
	std::cout << "PASS: Vulkan real stream allocation/reset/buffer binding preserves cached nickname + clan across 7 frames\n";
}
int main(int argc, char **argv)
{
	try
	{
		Check(argc == 2, "repository font path required");
		FT_Library Library = nullptr; Check(FT_Init_FreeType(&Library) == 0, "FreeType init failed");
		std::vector<FT_Face> Faces;
		for(const char *pFile : {"DejaVuSans.ttf", "GlowSansJ-Compressed-Book.otf", "SourceHanSans.ttc"})
		{
			FT_Face Face = nullptr;
			Check(FT_New_Face(Library, (std::string(argv[1]) + "/data/fonts/" + pFile).c_str(), 0, &Face) == 0, "font load failed");
			Faces.push_back(Face);
		}
		{
			IGraphics G; CGlyphMap Glyphs(&G);
			for(auto Face : Faces) Glyphs.AddFace(Face);
			Glyphs.SetDefaultFaceByName(Faces[0]->family_name); Glyphs.SetIconFaceByName(Faces[0]->family_name);
			Glyphs.AddFallbackFaceByName(Faces[2]->family_name);
			CTextRender TR(&G, &Glyphs);
			VulkanLifetime(TR, G);
			int Cases = 0;
			for(bool Buffered : {false, true})
			{
				G.m_Buffered = Buffered;
				for(auto Face : Faces)
				{
					Glyphs.SetDefaultFaceByName(Face->family_name);
					for(const char *pText : {"AVjT", "名字昵称氏族", "AV🙂 é", "space name"})
						for(unsigned Extra : {0u, (unsigned)TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT, (unsigned)TEXT_RENDER_FLAG_KERNING, (unsigned)(TEXT_RENDER_FLAG_KERNING | TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT)})
						{
							RenderDifferential(TR, G, pText, TEXT_RENDER_FLAG_NO_FIRST_CHARACTER_X_BEARING | TEXT_RENDER_FLAG_NO_LAST_CHARACTER_ADVANCE | Extra, 32); ++Cases;
						}
					NameplateDifferential(TR, G); ++Cases;
				}
			}
			std::cout << "PASS: " << Cases << " real-font soft/hard-create and gradient/ordinary-nameplate cases; repeated positions, scale, bearing, kerning, UTF-8, splits, buffered/unbuffered\n";
		}
		for(auto Face : Faces) FT_Done_Face(Face);
		FT_Done_FreeType(Library);
		return 0;
	}
	catch(const std::exception &Error) { std::cerr << "FAIL: " << Error.what() << "\n"; return 1; }
}
