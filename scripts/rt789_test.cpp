// Offline regression fixture: production text/gradient/envelope functions, no real GPU.
#include "rt789_support.h"
#include <engine/map.h>
#include "rt789_envelope_types.h"

static void Check(bool Value, const char *pWhat)
{
	if(!Value) throw std::runtime_error(pWhat);
}
static bool Near(float A, float B) { return std::abs(A - B) < 0.002f; }
static bool SameSplits(const std::vector<STextColorSplit> &A, const std::vector<STextColorSplit> &B)
{
	if(A.size() != B.size()) return false;
	for(size_t i = 0; i < A.size(); ++i)
		if(A[i].m_CharIndex != B[i].m_CharIndex || A[i].m_Length != B[i].m_Length || A[i].m_Color != B[i].m_Color) return false;
	return true;
}
static CTextCursor Cursor()
{
	CTextCursor C;
	C.m_FontSize = 24;
	C.m_Flags = TEXTFLAG_RENDER;
	return C;
}
static void QuadColor(CTextRender &TR, STextContainerIndex Index, size_t Quad, ColorRGBA Color)
{
	const auto &Quads = TR.GetTextContainer(Index).m_StringInfo.m_vCharacterQuads;
	Check(Quad < Quads.size(), "missing text quad");
	for(const auto &V : Quads[Quad].m_aVertices)
		Check(V.m_Color.r == (unsigned char)(Color.r * 255.f) && V.m_Color.g == (unsigned char)(Color.g * 255.f) && V.m_Color.b == (unsigned char)(Color.b * 255.f) && V.m_Color.a == (unsigned char)(Color.a * 255.f), "text quad RGBA mismatch");
}

class IClient { public: enum { STATE_DEMOPLAYBACK = 5 }; };
class CGameClient
{
public:
	struct { int m_LocalClientId = 1; struct { bool m_Active = false; int m_SpectatorId = -1; } m_SpecInfo; } m_Snap;
};
class CTClientGradient
{
public:
	CTextRender *m_pText;
	CGameClient m_Game;
	struct SClient { int State() const { return 0; } double GlobalTime() const { return 0.0; } } m_Client;
	ColorRGBA m_Stop1 = ColorRGBA(1, 0, 0, 1), m_Stop2 = ColorRGBA(0, 0, 1, 1);
	CGameClient *GameClient() { return &m_Game; }
	SClient *Client() { return &m_Client; }
	CTextRender *TextRender() { return m_pText; }
	static float AnimatePhase(double Time) { return TClientGradient::AnimatePhase(Time, 100); }
	std::vector<STextColorSplit> BuildAnimatedTextSplits(const char *pText, int, CGameClient *, float Phase, int Length)
	{
		const ColorRGBA Stops[] = {m_Stop1, m_Stop2};
		return TClientGradient::BuildCustomColorSplits(pText, Stops, 2, Phase, Length);
	}
	void ApplyEverythingGradient(CTextCursor *, const char *, int);
	static void Callback(CTextCursor *pCursor, const char *pText, int Length, void *pUser)
	{
		static_cast<CTClientGradient *>(pUser)->ApplyEverythingGradient(pCursor, pText, Length);
	}
};
#include "rt789_gradient.h"
#include "rt789_envelope_impl.h"

class CRenderLayerQuads
{
public:
	struct CQuadCluster { bool m_Grouped = false; int m_StartIndex = 0; int m_NumQuads = 1; int m_PosEnv = 0; };
	struct SManager { CEnvelopeExtrema *m_pExt; CEnvelopeExtrema *EnvelopeExtrema() { return m_pExt; } };
	SManager *m_pEnvelopeManager;
	CQuad *m_pQuads;
	bool CalculateQuadClipping(const CQuadCluster &, float[2], float[2]) const;
};
#include "rt789_clipping.h"

class CFixtureMap : public IMap
{
public:
	std::vector<CMapItemEnvelope> m_Envelopes;
	std::vector<CEnvPoint> m_Points;
	std::vector<CEnvPointBezier> m_Bezier;
	bool m_HasBezier = true;
	CFixtureMap()
	{
		CMapItemEnvelope E{};
		E.m_Version = 2; E.m_Channels = 3; E.m_StartPoint = 0; E.m_NumPoints = 2;
		m_Envelopes.push_back(E);
		m_Points.resize(2); m_Bezier.resize(2);
		m_Points[0].m_Time = CFixedTime(0); m_Points[1].m_Time = CFixedTime(1000);
		m_Points[0].m_Curvetype = CURVETYPE_BEZIER;
		m_Points[1].m_Curvetype = CURVETYPE_LINEAR;
		for(int c = 0; c < 4; ++c)
		{
			m_Bezier[0].m_aOutTangentDeltaX[c] = CFixedTime(333);
			m_Bezier[1].m_aInTangentDeltaX[c] = CFixedTime(-333);
		}
	}
	int GetDataSize(int) const override { return 0; }
	void *GetData(int) override { return nullptr; }
	void *GetDataSwapped(int) override { return nullptr; }
	const char *GetDataString(int) override { return nullptr; }
	void UnloadData(int) override {}
	int NumData() const override { return 0; }
	int GetItemSize(int Index) override
	{
		if(Index == 100) return (int)(m_Points.size() * sizeof(CEnvPoint));
		if(Index == 101) return m_HasBezier ? (int)(m_Bezier.size() * sizeof(CEnvPointBezier)) : 0;
		return sizeof(CMapItemEnvelope);
	}
	void *GetItem(int Index, int * = nullptr, int * = nullptr, CUuid * = nullptr) override
	{
		if(Index == 100) return m_Points.data();
		if(Index == 101) return m_HasBezier ? m_Bezier.data() : nullptr;
		return &m_Envelopes.at(Index);
	}
	void GetType(int Type, int *pStart, int *pNum) override
	{
		*pStart = 0; *pNum = 0;
		if(Type == MAPITEMTYPE_ENVELOPE) *pNum = (int)m_Envelopes.size();
		if(Type == MAPITEMTYPE_ENVPOINTS) { *pStart = 100; *pNum = 1; }
		if(Type == MAPITEMTYPE_ENVPOINTS_BEZIER) { *pStart = 101; *pNum = m_HasBezier ? 1 : 0; }
	}
	int FindItemIndex(int, int) override { return -1; }
	void *FindItem(int, int) override { return nullptr; }
	int NumItems() const override { return (int)m_Envelopes.size() + 2; }
	bool Load(const char *, IStorage *, const char *, int) override { return false; }
	bool Load(IStorage *, const char *, int) override { return false; }
	void Unload() override {}
	bool IsLoaded() const override { return true; }
	IOHANDLE File() const override { return nullptr; }
	const char *FullName() const override { return "fixture"; }
	const char *BaseName() const override { return "fixture"; }
	const char *Path() const override { return "fixture"; }
	SHA256_DIGEST Sha256() const override { return {}; }
	unsigned Crc() const override { return 0; }
	int Size() const override { return 0; }
};

int main(int argc, char **argv)
{
	int Failed = 0, Passed = 0;
	auto Run = [&](const char *pName, auto Test) {
		try { Test(); ++Passed; std::cout << "PASS " << pName << '\n'; }
		catch(const std::exception &E) { ++Failed; std::cout << "FAIL " << pName << ": " << E.what() << '\n'; }
	};
	Check(argc == 2, "repository font path required");
	FT_Library Library = nullptr; Check(FT_Init_FreeType(&Library) == 0, "FreeType init");
	FT_Face Face = nullptr;
	Check(FT_New_Face(Library, (std::string(argv[1]) + "/data/fonts/DejaVuSans.ttf").c_str(), 0, &Face) == 0, "font load");
	{
		IGraphics G; CGlyphMap Glyphs(&G); Glyphs.AddFace(Face);
		Glyphs.SetDefaultFaceByName(Face->family_name); Glyphs.SetIconFaceByName(Face->family_name);
		CTextRender TR(&G, &Glyphs);
		Run("RT7 split table immutable across UTF8 Append", [&] {
			auto C = Cursor(); STextContainerIndex Index;
			C.m_vColorSplits = {{0, 3, ColorRGBA(1, 0, 0, 1)}, {3, 1, ColorRGBA(0, 1, 0, 1)}, {4, 1, ColorRGBA(0, 0, 1, 1)}};
			const auto Original = C.m_vColorSplits;
			TR.CreateTextContainer(Index, &C, "中AB");
			Check(C.m_CharCount == 5 && C.m_GlyphCount == 3, "m_CharCount must accumulate UTF8 bytes");
			QuadColor(TR, Index, 0, ColorRGBA(1, 0, 0, 1)); QuadColor(TR, Index, 1, ColorRGBA(0, 1, 0, 1)); QuadColor(TR, Index, 2, ColorRGBA(0, 0, 1, 1));
			Check(SameSplits(C.m_vColorSplits, Original), "split reference overwrote caller table");
			TR.AppendTextContainer(Index, &C, "DE");
			QuadColor(TR, Index, 3, TR.m_Color); QuadColor(TR, Index, 4, TR.m_Color);
			Check(SameSplits(C.m_vColorSplits, Original) && C.m_CharCount == 7, "Append mutated split table/count");
		});
		Run("RT7 skip several expired splits, gaps and unbounded split", [&] {
			auto C = Cursor(); C.m_CharCount = 9; STextContainerIndex Index;
			C.m_vColorSplits = {{0, 1, ColorRGBA(1, 0, 0, 1)}, {2, 1, ColorRGBA(0, 1, 0, 1)}, {4, 1, ColorRGBA(0, 0, 1, 1)}, {10, 3, ColorRGBA(1, 0, 1, 1)}, {14, -1, ColorRGBA(0, 1, 1, 1)}};
			const auto Original = C.m_vColorSplits;
			TR.CreateTextContainer(Index, &C, "A中");
			QuadColor(TR, Index, 0, TR.m_Color); QuadColor(TR, Index, 1, ColorRGBA(1, 0, 1, 1));
			TR.AppendTextContainer(Index, &C, "BC"); TR.AppendTextContainer(Index, &C, "D");
			QuadColor(TR, Index, 2, TR.m_Color); QuadColor(TR, Index, 3, ColorRGBA(0, 1, 1, 1)); QuadColor(TR, Index, 4, ColorRGBA(0, 1, 1, 1));
			Check(SameSplits(C.m_vColorSplits, Original) && C.m_CharCount == 16, "multi-Append bytes/table");
		});
		Run("RT8 base alpha 0/.3/1 times custom alpha", [&] {
			CTClientGradient Gradient; Gradient.m_pText = &TR;
			g_Config.m_TcGradientEverything = 1;
			TR.m_pfnTextColorSplitCallback = CTClientGradient::Callback; TR.m_pTextColorSplitCallbackUserData = &Gradient;
			for(float Base : {0.f, .3f, 1.f}) for(float Custom : {0.f, .4f, 1.f})
			{
				TR.m_Color = ColorRGBA(.2f, .5f, .8f, Base);
				Gradient.m_Stop1.a = Custom; Gradient.m_Stop2.a = Custom;
				auto C = Cursor(); C.m_CharCount = 5; STextContainerIndex Index;
				Gradient.ApplyEverythingGradient(&C, "中AB", 5);
				Check(C.m_vColorSplits.size() == 3 && C.m_vColorSplits[0].m_CharIndex == 5 && C.m_vColorSplits[1].m_CharIndex == 8, "gradient cumulative byte offsets");
				for(const auto &S : C.m_vColorSplits) Check(Near(S.m_Color.a, Base * Custom), "generated split lost base/custom alpha");
				const bool Created = TR.CreateTextContainer(Index, &C, "中AB");
				Check(C.m_CharCount == 10, "gradient accumulated UTF8 count");
				const auto Count = Created ? TR.GetTextContainer(Index).m_StringInfo.m_vCharacterQuads.size() : 0u;
				Check(Count == (Base * Custom == 0 ? 0u : 3u), "hidden gradient generated visible quads");
				for(size_t i = 0; i < Count; ++i) QuadColor(TR, Index, i, C.m_vColorSplits[i].m_Color);
				// Also enter through AppendTextContainer's actual empty-table callback seam.
				auto Automatic = Cursor(); STextContainerIndex AutomaticIndex;
				const bool AutomaticCreated = TR.CreateTextContainer(AutomaticIndex, &Automatic, "中AB");
				Check(AutomaticCreated == (Base * Custom != 0), "callback path ignored visibility alpha");
				for(const auto &S : Automatic.m_vColorSplits) Check(Near(S.m_Color.a, Base * Custom), "automatic callback alpha");
				if(AutomaticCreated) for(size_t i = 0; i < 3; ++i) QuadColor(TR, AutomaticIndex, i, Automatic.m_vColorSplits[i].m_Color);
			}
			TR.m_pfnTextColorSplitCallback = nullptr; TR.m_Color = ColorRGBA(1, 1, 1, 1);
		});
		Run("RT8 disabled and explicit alpha unchanged; cleared cursor reapplies", [&] {
			CTClientGradient Gradient; Gradient.m_pText = &TR;
			TR.m_Color = ColorRGBA(.2f, .5f, .8f, .3f);
			g_Config.m_TcGradientEverything = 0;
			TR.m_pfnTextColorSplitCallback = CTClientGradient::Callback; TR.m_pTextColorSplitCallbackUserData = &Gradient;
			auto C = Cursor(); STextContainerIndex Index;
			TR.CreateTextContainer(Index, &C, "AB"); Check(C.m_vColorSplits.empty(), "disabled generated splits");
			QuadColor(TR, Index, 0, TR.m_Color);
			g_Config.m_TcGradientEverything = 1;
			auto E = Cursor(); STextContainerIndex Explicit;
			E.m_vColorSplits = {{0, -1, ColorRGBA(1, 0, 0, .7f)}}; const auto Original = E.m_vColorSplits;
			TR.CreateTextContainer(Explicit, &E, "A"); Check(SameSplits(E.m_vColorSplits, Original), "explicit split changed"); QuadColor(TR, Explicit, 0, Original[0].m_Color);
			C.m_vColorSplits.clear(); TR.AppendTextContainer(Index, &C, "中"); Check(C.m_vColorSplits[0].m_CharIndex == 2, "cleared chat-like Append lost offset");
			TR.m_pfnTextColorSplitCallback = nullptr; TR.m_Color = ColorRGBA(1, 1, 1, 1);
		});
	}
	FT_Done_Face(Face); FT_Done_FreeType(Library);
	Run("RT9 real Bezier evaluation 67.5 degrees disables static clipping", [&] {
		CFixtureMap Map; Map.m_Bezier[0].m_aOutTangentDeltaY[2] = f2fx(180.f);
		CMapBasedEnvelopePointAccess Points(&Map); ColorRGBA Value(0, 0, 0, 0);
		CRenderMap::RenderEvalEnvelope(&Points, 500ms, Value, 3);
		Check(Near(Value[2], 67.5f), "production angle not 67.5 degrees");
		CEnvelopeExtrema Ext(&Map); const auto &Bounds = Ext.GetExtrema(0);
		Check(Bounds.m_Available && Bounds.m_Rotating, "nonzero Bezier rotation classified static");
		Check(Bounds.m_Minima == ivec2(0, 0) && Bounds.m_Maxima == ivec2(0, 0), "angle changed position extrema");
		// Long thin quad centered at (0,-50): initially above viewport y>=0,
		// but its tip enters the viewport at the production-evaluated 67.5 degrees.
		CQuad Quad{}; Quad.m_PosEnv = 0;
		const vec2 Corners[] = {vec2(-100, -51), vec2(100, -51), vec2(-100, -49), vec2(100, -49)};
		for(int i = 0; i < 4; ++i) { Quad.m_aPoints[i].x = f2fx(Corners[i].x); Quad.m_aPoints[i].y = f2fx(Corners[i].y); }
		Quad.m_aPoints[4].x = 0; Quad.m_aPoints[4].y = f2fx(-50.f);
		const float Angle = Value[2] / 180.f * pi;
		Check(100.f * std::sin(Angle) + std::cos(Angle) - 50.f > 0, "rotation fixture did not enter viewport");
		CRenderLayerQuads::SManager Manager{&Ext}; CRenderLayerQuads Layer; Layer.m_pEnvelopeManager = &Manager; Layer.m_pQuads = &Quad;
		for(bool Grouped : {false, true})
		{
			CRenderLayerQuads::CQuadCluster Cluster; Cluster.m_Grouped = Grouped;
			float Min[2], Max[2]; Check(Layer.CalculateQuadClipping(Cluster, Min, Max), "production clip unavailable");
			Check(Max[1] > 0 && Min[1] < 0, "rotating quad statically clipped above viewport");
		}
		Quad.m_PosEnv = -1;
		CRenderLayerQuads::CQuadCluster Static; Static.m_PosEnv = -1; float Min[2], Max[2];
		Check(Layer.CalculateQuadClipping(Static, Min, Max) && Near(Max[1], -49.f), "static clipping behavior changed");
	});
	Run("RT9 incoming angle tangent and position extrema/cache", [&] {
		CFixtureMap Map; Map.m_Bezier[1].m_aInTangentDeltaY[2] = f2fx(-180.f);
		Map.m_Points[0].m_aValues[0] = 10; Map.m_Points[1].m_aValues[0] = 20;
		Map.m_Bezier[0].m_aOutTangentDeltaY[0] = -30; Map.m_Bezier[1].m_aInTangentDeltaY[0] = 50;
		CEnvelopeExtrema Ext(&Map); const auto &A = Ext.GetExtrema(0); const auto &B = Ext.GetExtrema(0);
		Check(A.m_Available && A.m_Rotating && &A == &B, "incoming rotation/cache");
		Check(A.m_Minima == ivec2(-20, 0) && A.m_Maxima == ivec2(70, 0), "position extrema changed");
		Check(Ext.GetExtrema(-1).m_Available && !Ext.GetExtrema(-1).m_Rotating && !Ext.GetExtrema(99).m_Available, "sentinel cache changed");
	});
	Run("RT9 ignore non-segment tangent channels and preceding envelope", [&] {
		CFixtureMap Map;
		Map.m_Points[0].m_Curvetype = CURVETYPE_LINEAR;
		Map.m_Points[1].m_Curvetype = CURVETYPE_BEZIER;
		Map.m_Bezier[0].m_aInTangentDeltaY[2] = f2fx(180.f);
		Map.m_Bezier[0].m_aOutTangentDeltaY[2] = f2fx(180.f);
		Map.m_Bezier[1].m_aInTangentDeltaY[2] = f2fx(180.f);
		Map.m_Bezier[1].m_aOutTangentDeltaY[2] = f2fx(180.f);
		Map.m_Bezier[0].m_aOutTangentDeltaY[3] = f2fx(180.f);
		CEnvelopeExtrema Ext(&Map); Check(Ext.GetExtrema(0).m_Available && !Ext.GetExtrema(0).m_Rotating, "inactive tangent marked rotating");
		Map.m_Points[0].m_Curvetype = CURVETYPE_BEZIER; Map.m_Envelopes[0].m_StartPoint = 1; Map.m_Envelopes[0].m_NumPoints = 1;
		CEnvelopeExtrema One(&Map); Check(One.GetExtrema(0).m_Available && !One.GetExtrema(0).m_Rotating, "foreign preceding envelope tangent marked rotating");
		Map.m_Envelopes[0].m_StartPoint = 0; Map.m_Envelopes[0].m_NumPoints = 2;
		Map.m_Points[1].m_Time = Map.m_Points[0].m_Time;
		CEnvelopeExtrema Degenerate(&Map); Check(Degenerate.GetExtrema(0).m_Available && !Degenerate.GetExtrema(0).m_Rotating, "zero-duration tangent marked rotating");
	});
	Run("RT9 zero angle, nonzero endpoint, invalid channel/missing data", [&] {
		CFixtureMap Map; Map.m_Bezier[0].m_aOutTangentDeltaY[0] = 900;
		CEnvelopeExtrema Zero(&Map); Check(Zero.GetExtrema(0).m_Available && !Zero.GetExtrema(0).m_Rotating, "position tangent interpreted as angle");
		Map.m_Points[1].m_aValues[2] = 1;
		CEnvelopeExtrema Nonzero(&Map); Check(Nonzero.GetExtrema(0).m_Rotating, "endpoint rotation lost");
		Map.m_Envelopes[0].m_Channels = 4;
		CEnvelopeExtrema Color(&Map); Check(!Color.GetExtrema(0).m_Available, "nonposition envelope accepted");
		Map.m_Envelopes[0].m_Channels = 3; Map.m_HasBezier = false;
		CEnvelopeExtrema Missing(&Map); Check(!Missing.GetExtrema(0).m_Available, "missing bezier extrema accepted");
	});
	std::cout << "SUMMARY " << Passed << " passed, " << Failed << " failed\n";
	return Failed ? 1 : 0;
}
