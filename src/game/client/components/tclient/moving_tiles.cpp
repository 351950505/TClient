#include "moving_tiles.h"

#include <base/color.h>
#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/str.h>
#include <base/vmath.h>

#include <engine/graphics.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>

#include <game/client/components/envelope_state.h>
#include <game/client/gameclient.h>
#include <game/map/render_map.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <iterator>

inline static void RotatePoint(const vec2 &Center, vec2 &Point, float Rotation)
{
	const vec2 RelativePos = Point - Center;
	Point.x = RelativePos.x * std::cos(Rotation) - RelativePos.y * std::sin(Rotation) + Center.x;
	Point.y = RelativePos.x * std::sin(Rotation) + RelativePos.y * std::cos(Rotation) + Center.y;
}

inline static bool QuadName(const int *pInts, size_t NumInts, char *pStr, size_t StrSize)
{
	dbg_assert(NumInts > 0, "QuadName: NumInts invalid");
	dbg_assert(StrSize >= NumInts * sizeof(int), "QuadName: StrSize invalid");

	// Unpack string without validation
	size_t StrIndex = 0;
	for(size_t IntIndex = 0; IntIndex < NumInts; IntIndex++)
	{
		const int CurrentInt = pInts[IntIndex];
		pStr[StrIndex] = ((CurrentInt >> 24) & 0xff) - 128;
		StrIndex++;
		pStr[StrIndex] = ((CurrentInt >> 16) & 0xff) - 128;
		StrIndex++;
		pStr[StrIndex] = ((CurrentInt >> 8) & 0xff) - 128;
		StrIndex++;
		pStr[StrIndex] = (CurrentInt & 0xff) - 128;
		StrIndex++;
	}
	// Ensure null-termination
	pStr[StrIndex - 1] = '\0';

	// Ensure valid UTF-8
	if(str_utf8_check(pStr))
	{
		return true;
	}
	pStr[0] = '\0';
	return false;
}

// Adapted from QmClient's moving-water layer recognition. Map artwork is water
// when its name contains "water" but not "sun"; the alpha source is unavailable.
static bool IsMovingWaterName(const char *pName)
{
	return pName && pName[0] != '\0' && str_find_nocase(pName, "sun") == nullptr && str_find_nocase(pName, "water") != nullptr;
}

static bool QuadLayerUsesMovingWaterImage(IMap *pMap, const CMapItemLayerQuads *pLayer)
{
	if(!pMap || !pLayer)
		return false;

	int ImagesStart = 0;
	int ImagesNum = 0;
	pMap->GetType(MAPITEMTYPE_IMAGE, &ImagesStart, &ImagesNum);
	if(pLayer->m_Image < 0 || pLayer->m_Image >= ImagesNum)
		return false;

	const auto *pImage = static_cast<const CMapItemImage_v2 *>(pMap->GetItem(ImagesStart + pLayer->m_Image));
	if(!pImage || pImage->m_ImageName < 0 || pImage->m_ImageName >= pMap->NumData())
		return false;

	// Map data is shared with collision and rendering, including aliased data indices.
	const char *pImageName = pMap->GetDataString(pImage->m_ImageName);
	return IsMovingWaterName(pImageName);
}

void CMovingTiles::Reset()
{
	m_vQuads.clear();
	m_HasGoresOrAxiomQuads = false;
}

bool CMovingTiles::ShouldRenderGoresOrAxiomWater() const
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return false;

	const CServerInfo &ServerInfo = Client()->ServerInfo();
	const char *pServerGameType = ServerInfo.m_aGameType;
	const char *pCommunityId = ServerInfo.m_aCommunityId;
	IServerBrowser *pServerBrowser = GameClient()->ServerBrowser();
	const IServerBrowser::CServerEntry *pEntry = pServerBrowser ? pServerBrowser->Find(Client()->ServerAddress()) : nullptr;
	if(pEntry)
	{
		if(!pServerGameType || pServerGameType[0] == '\0')
			pServerGameType = pEntry->m_Info.m_aGameType;
		pCommunityId = pEntry->m_Info.m_aCommunityId;
	}
	else if(GameClient()->m_ConnectServerInfo)
	{
		if(!pServerGameType || pServerGameType[0] == '\0')
			pServerGameType = GameClient()->m_ConnectServerInfo->m_aGameType;
		pCommunityId = GameClient()->m_ConnectServerInfo->m_aCommunityId;
	}

	const CCommunity *pCommunity = pServerBrowser && pCommunityId && pCommunityId[0] != '\0' ? pServerBrowser->Community(pCommunityId) : nullptr;
	const char *pCommunityName = pCommunity ? pCommunity->Name() : nullptr;
	return (GameClient()->m_GameInfo.m_aGameType[0] && str_find_nocase(GameClient()->m_GameInfo.m_aGameType, "gores")) ||
		(pServerGameType && str_find_nocase(pServerGameType, "gores")) ||
		(pCommunityId && str_find_nocase(pCommunityId, "axiom")) ||
		(pCommunityName && str_find_nocase(pCommunityName, "axiom"));
}

void CMovingTiles::OnStateChange(int NewState, int OldState)
{
	if(NewState != OldState && NewState != IClient::STATE_ONLINE)
		Reset();
}

void CMovingTiles::OnMapLoad()
{
	Reset();

	int GroupsStart, LayersStart, GroupsNum, LayersNum;

	GameClient()->Map()->GetType(MAPITEMTYPE_GROUP, &GroupsStart, &GroupsNum);
	GameClient()->Map()->GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);

	// size_t NumQuadLayers = 0;
	for(int GroupIndex = 0; GroupIndex < GroupsNum; GroupIndex++)
	{
		CMapItemGroup *pGroup = static_cast<CMapItemGroup *>(GameClient()->Map()->GetItem(GroupsStart + GroupIndex));
		for(int LayerIndex = 0; LayerIndex < pGroup->m_NumLayers; LayerIndex++)
		{
			const int LayerItemIndex = LayersStart + pGroup->m_StartLayer + LayerIndex;
			CMapItemLayer *pLayer = static_cast<CMapItemLayer *>(GameClient()->Map()->GetItem(LayerItemIndex));

			if(pLayer->m_Type != LAYERTYPE_QUADS || GameClient()->Map()->GetItemSize(LayerItemIndex) < (int)sizeof(CMapItemLayerQuads))
				continue;

				char aLayerName[30];
				CMapItemLayerQuads *pTilemap = reinterpret_cast<CMapItemLayerQuads *>(pLayer);
				if(pTilemap->m_Version < 2 || pTilemap->m_NumQuads <= 0)
					continue;
				QuadName(pTilemap->m_aName, std::size(pTilemap->m_aName), aLayerName, std::size(aLayerName));

				EQType Type = EQType::NONE;
				bool RequiresGoresOrAxiom = false;
				for(size_t Names = 0; Names < std::size(ValidQuadNames); Names++)
				{
					if(!str_comp(ValidQuadNames[Names], aLayerName))
					{
						Type = static_cast<EQType>(Names);
						break;
					}
				}
				if(Type == EQType::NONE && IsMovingWaterName(aLayerName))
				{
					Type = EQType::FREEZE;
					RequiresGoresOrAxiom = true;
				}
				if(Type == EQType::NONE && QuadLayerUsesMovingWaterImage(GameClient()->Map(), pTilemap))
				{
					Type = EQType::FREEZE;
					RequiresGoresOrAxiom = true;
				}
				if(Type == EQType::NONE)
					continue;
				if(!m_RenderAbove && (Type == EQType::HOOKABLE || Type == EQType::UNHOOKABLE))
					continue;
				if(m_RenderAbove && Type != EQType::HOOKABLE && Type != EQType::UNHOOKABLE)
					continue;

				CQuad *pQuads = static_cast<CQuad *>(GameClient()->Map()->GetDataSwapped(pTilemap->m_Data));
				if(pQuads == nullptr || (size_t)pTilemap->m_NumQuads > (size_t)GameClient()->Map()->GetDataSize(pTilemap->m_Data) / sizeof(CQuad))
					continue;
				for(int NumQuads = 0; NumQuads < pTilemap->m_NumQuads; NumQuads++)
				{
					CQuadData QuadData;
					QuadData.m_pQuad = &pQuads[NumQuads];
					QuadData.m_pGroup = pGroup;
					QuadData.m_pLayer = pTilemap;
					QuadData.m_Type = Type;
					QuadData.m_RequiresGoresOrAxiom = RequiresGoresOrAxiom;
					m_HasGoresOrAxiomQuads = m_HasGoresOrAxiomQuads || RequiresGoresOrAxiom;
					for(int j = 0; j < 5; j++)
						QuadData.m_Pos[j] = vec2(fx2f(QuadData.m_pQuad->m_aPoints[j].x), fx2f(QuadData.m_pQuad->m_aPoints[j].y));
					m_vQuads.push_back(QuadData);
				}

		}
	}
	// log_info("moving-tiles", "%" PRIzu " valid quadlayer%s with %d quads", NumQuadLayers, NumQuadLayers == 1 ? "" : "s", (int)m_vQuads.size());
	m_EnvEvaluator = CEnvelopeState(GameClient()->Map(), true);
	m_EnvEvaluator.OnInterfacesInit(GameClient());
}

void CMovingTiles::OnRender()
{
	if(g_Config.m_ClOverlayEntities != 100)
		return;
	if(!g_Config.m_TcMovingTilesEntities)
		return;
	if(m_vQuads.empty())
		return;

	// EnvelopeEval reads cached time; refresh it once for both position and color.
	m_EnvEvaluator.Update();
	const bool RenderGoresOrAxiomWater = !m_HasGoresOrAxiomQuads || ShouldRenderGoresOrAxiomWater();

	const vec2 Center = GameClient()->m_Camera.m_Center;
	const float Zoom = GameClient()->m_Camera.m_Zoom;

	auto ApplyGroupState = [&](const CMapItemGroup *pGroup) -> bool {
		Graphics()->ClipDisable();

		if(!pGroup)
			return false;

		if(!g_Config.m_GfxNoclip && pGroup->m_Version >= 2 && pGroup->m_UseClipping)
		{
			Graphics()->MapScreenToInterface(Center.x, Center.y, Zoom);

			CScreenRect ScreenRect = Graphics()->GetScreen();
			float ScreenWidth = ScreenRect.Width();
			float ScreenHeight = ScreenRect.Height();
			float Left = pGroup->m_ClipX - ScreenRect.m_TopLeft.x;
			float Top = pGroup->m_ClipY - ScreenRect.m_TopLeft.y;
			float Right = pGroup->m_ClipX + pGroup->m_ClipW - ScreenRect.m_TopLeft.x;
			float Bottom = pGroup->m_ClipY + pGroup->m_ClipH - ScreenRect.m_TopLeft.y;

			if(Right < 0.0f || Left > ScreenWidth || Bottom < 0.0f || Top > ScreenHeight)
				return false;

			int ClipX = (int)std::round(Left * Graphics()->ScreenWidth() / ScreenWidth);
			int ClipY = (int)std::round(Top * Graphics()->ScreenHeight() / ScreenHeight);

			Graphics()->ClipEnable(
				ClipX,
				ClipY,
				(int)std::round(Right * Graphics()->ScreenWidth() / ScreenWidth) - ClipX,
				(int)std::round(Bottom * Graphics()->ScreenHeight() / ScreenHeight) - ClipY);
		}

		int ParallaxZoom = std::clamp(std::max(pGroup->m_ParallaxX, pGroup->m_ParallaxY), 0, 100);
		CScreenRect ScreenRect = Graphics()->MapScreenToWorld(
			Center.x, Center.y,
			pGroup->m_ParallaxX, pGroup->m_ParallaxY, (float)ParallaxZoom,
			pGroup->m_OffsetX, pGroup->m_OffsetY,
			Graphics()->ScreenAspect(), Zoom);
		Graphics()->MapScreen(ScreenRect);

		return true;
	};

	constexpr float ColorConv = 1.0f / 255.0f;
	Graphics()->BlendNormal();

	size_t QuadStart = 0;
	while(QuadStart < m_vQuads.size())
	{
		const CMapItemGroup *pGroup = m_vQuads[QuadStart].m_pGroup;
		const CMapItemLayerQuads *pLayer = m_vQuads[QuadStart].m_pLayer;
		if(!pLayer)
		{
			QuadStart++;
			continue;
		}

		size_t QuadEnd = QuadStart + 1;
		while(QuadEnd < m_vQuads.size() &&
			m_vQuads[QuadEnd].m_pGroup == pGroup &&
			m_vQuads[QuadEnd].m_pLayer == pLayer)
		{
			QuadEnd++;
		}

		if(!ApplyGroupState(pGroup))
		{
			QuadStart = QuadEnd;
			continue;
		}

		if(pLayer->m_Image >= 0 && pLayer->m_Image < GameClient()->m_MapImages.Num())
			Graphics()->TextureSet(GameClient()->m_MapImages.Get(pLayer->m_Image));
		else
			Graphics()->TextureClear();

		Graphics()->TrianglesBegin();

		for(size_t QuadIndex = QuadStart; QuadIndex < QuadEnd; QuadIndex++)
		{
			const CQuadData &QuadData = m_vQuads[QuadIndex];
			if(QuadData.m_RequiresGoresOrAxiom && !RenderGoresOrAxiomWater)
				continue;
			const CQuad *pQuad = QuadData.m_pQuad;
			if(!pQuad)
				continue;

			ColorRGBA Color(1.0f, 1.0f, 1.0f, 1.0f);
			m_EnvEvaluator.EnvelopeEval(pQuad->m_ColorEnvOffset, pQuad->m_ColorEnv, Color, 4);
			if(Color.a <= 0.0f)
				continue;

			Graphics()->QuadsSetSubsetFree(
				fx2f(pQuad->m_aTexcoords[0].x), fx2f(pQuad->m_aTexcoords[0].y),
				fx2f(pQuad->m_aTexcoords[1].x), fx2f(pQuad->m_aTexcoords[1].y),
				fx2f(pQuad->m_aTexcoords[2].x), fx2f(pQuad->m_aTexcoords[2].y),
				fx2f(pQuad->m_aTexcoords[3].x), fx2f(pQuad->m_aTexcoords[3].y));

			ColorRGBA Position(0.0f, 0.0f, 0.0f, 0.0f);
			m_EnvEvaluator.EnvelopeEval(pQuad->m_PosEnvOffset, pQuad->m_PosEnv, Position, 3);

			const vec2 Offset(Position.r, Position.g);
			const float Rotation = Position.b / 180.0f * pi + QuadData.m_Angle;

			Graphics()->SetColor4(
				ColorRGBA(pQuad->m_aColors[0].r, pQuad->m_aColors[0].g, pQuad->m_aColors[0].b, pQuad->m_aColors[0].a).Multiply(Color).Multiply(ColorConv),
				ColorRGBA(pQuad->m_aColors[1].r, pQuad->m_aColors[1].g, pQuad->m_aColors[1].b, pQuad->m_aColors[1].a).Multiply(Color).Multiply(ColorConv),
				ColorRGBA(pQuad->m_aColors[3].r, pQuad->m_aColors[3].g, pQuad->m_aColors[3].b, pQuad->m_aColors[3].a).Multiply(Color).Multiply(ColorConv),
				ColorRGBA(pQuad->m_aColors[2].r, pQuad->m_aColors[2].g, pQuad->m_aColors[2].b, pQuad->m_aColors[2].a).Multiply(Color).Multiply(ColorConv));

			vec2 aPoints[4] = {
				QuadData.m_Pos[0],
				QuadData.m_Pos[1],
				QuadData.m_Pos[2],
				QuadData.m_Pos[3],
			};

			if(Rotation != 0.0f)
			{
				for(vec2 &Point : aPoints)
					RotatePoint(QuadData.m_Pos[4], Point, Rotation);
			}

			const IGraphics::CFreeformItem Freeform(
				aPoints[0] + Offset,
				aPoints[1] + Offset,
				aPoints[2] + Offset,
				aPoints[3] + Offset);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		Graphics()->TrianglesEnd();
		QuadStart = QuadEnd;
	}

	Graphics()->ClipDisable();
}
