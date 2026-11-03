/* TClient: 客户端身份标识（记分板企鹅徽标）。机制见 client_badge.h。 */
#include "client_badge.h"

#include <base/dbg.h>
#include <engine/storage.h>

#include <cmath>
#include <cstdlib>

bool CClientBadge::DetectAndStrip(int ClientId, int &Body, int &Feet)
{
	const unsigned BodyAlpha = (unsigned)Body >> 24;
	const unsigned FeetAlpha = (unsigned)Feet >> 24;
	// 渲染管线与颜色 UI 不需要线上的标识 alpha，统一剥掉
	Body &= 0x00FFFFFF;
	Feet &= 0x00FFFFFF;

	const bool IsTClient = BodyAlpha == CCID_COLOR_BODY_TCLIENT && FeetAlpha == CCID_COLOR_FEET_TCLIENT;
	if(IsTClient)
		m_aIsTClient |= std::uint64_t(1) << ClientId;
	return IsTClient;
}

namespace
{
// ---- 内置图标：32×32 矢量光栅化 mini-Tux（SDF 覆盖率抗锯齿），png 可覆盖 ----

struct SVec2
{
	float x, y;
};

float Dot(SVec2 a, SVec2 b) { return a.x * b.x + a.y * b.y; }
float Length(SVec2 a) { return std::sqrt(Dot(a, a)); }

// 椭圆近似 SDF（缩放到圆再归一回半径）
float SdEllipse(SVec2 Point, SVec2 Center, float Rx, float Ry)
{
	const float K0 = Length(SVec2{(Point.x - Center.x) / Rx, (Point.y - Center.y) / Ry});
	const float K1 = Length(SVec2{(Point.x - Center.x) / (Rx * Rx), (Point.y - Center.y) / (Ry * Ry)});
	return K0 * (K0 - 1.0f) / std::max(K1, 1e-6f);
}

// 圆角矩形 SDF
float SdRoundRect(SVec2 Point, SVec2 Center, SVec2 Half, float Radius)
{
	SVec2 D{std::abs(Point.x - Center.x) - Half.x + Radius, std::abs(Point.y - Center.y) - Half.y + Radius};
	return std::min(std::max(D.x, D.y), 0.0f) + Length(SVec2{std::max(D.x, 0.0f), std::max(D.y, 0.0f)}) - Radius;
}

// 凸四边形 SDF（半平面交集近似）
float SdQuad(SVec2 Point, const SVec2 (&aVerts)[4])
{
	float Dist = -1e9f;
	for(int i = 0; i < 4; i++)
	{
		const SVec2 &A = aVerts[i];
		const SVec2 &B = aVerts[(i + 1) % 4];
		SVec2 Edge{B.x - A.x, B.y - A.y};
		SVec2 Normal{-Edge.y, Edge.x};
		const float Norm = Length(Normal);
		Dist = std::max(Dist, ((Point.x - A.x) * Normal.x + (Point.y - A.y) * Normal.y) / Norm);
	}
	return Dist;
}

float Coverage(float Sd) { return std::clamp(0.5f - Sd, 0.0f, 1.0f); }

void MixPixel(uint8_t *pPixel, float r, float g, float b, float A)
{
	if(A <= 0.0f)
		return;
	const float OldA = pPixel[3] / 255.0f;
	const float NewA = A + OldA * (1.0f - A);
	if(NewA <= 0.0f)
		return;
	pPixel[0] = (uint8_t)std::round((r * A + pPixel[0] / 255.0f * OldA * (1.0f - A)) / NewA * 255.0f);
	pPixel[1] = (uint8_t)std::round((g * A + pPixel[1] / 255.0f * OldA * (1.0f - A)) / NewA * 255.0f);
	pPixel[2] = (uint8_t)std::round((b * A + pPixel[2] / 255.0f * OldA * (1.0f - A)) / NewA * 255.0f);
	pPixel[3] = (uint8_t)std::round(NewA * 255.0f);
}

void GeneratePenguin(uint8_t *pData)
{
	for(int y = 0; y < 32; y++)
	{
		for(int x = 0; x < 32; x++)
		{
			const SVec2 P{(float)x + 0.5f, (float)y + 0.5f};
			uint8_t *pPixel = &pData[(y * 32 + x) * 4];

			// 脚（在身体后面探出）
			const float Feet = std::min(
				SdRoundRect(P, SVec2{11.5f, 28.6f}, SVec2{3.0f, 1.6f}, 1.4f),
				SdRoundRect(P, SVec2{20.5f, 28.6f}, SVec2{3.0f, 1.6f}, 1.4f));
			MixPixel(pPixel, 0.965f, 0.647f, 0.122f, Coverage(Feet));

			// 身体
			MixPixel(pPixel, 0.094f, 0.094f, 0.125f, Coverage(SdEllipse(P, SVec2{16.0f, 16.5f}, 11.0f, 13.5f)));

			// 白肚
			MixPixel(pPixel, 0.941f, 0.941f, 0.953f, Coverage(SdEllipse(P, SVec2{16.0f, 20.6f}, 7.2f, 9.0f)));

			// 眼白
			const float Eyes = std::min(
				SdEllipse(P, SVec2{12.6f, 10.3f}, 2.0f, 2.5f),
				SdEllipse(P, SVec2{19.4f, 10.3f}, 2.0f, 2.5f));
			MixPixel(pPixel, 0.941f, 0.941f, 0.953f, Coverage(Eyes));

			// 喙（梯形）
			const SVec2 aBeak[4] = {{12.4f, 13.4f}, {19.6f, 13.4f}, {17.4f, 16.4f}, {14.6f, 16.4f}};
			MixPixel(pPixel, 0.965f, 0.647f, 0.122f, Coverage(SdQuad(P, aBeak)));
		}
	}
}
} // namespace

void CClientBadge::TryLoadTexture()
{
	if(m_TriedLoad)
		return;
	m_TriedLoad = true;

	// 允许 data/tclient/penguin.png 覆盖内置图标
	m_Texture = Graphics()->LoadTexture("tclient/penguin.png", IStorage::TYPE_ALL);
	if(m_Texture.IsValid())
	{
		dbg_msg("tcbadge", "using override tclient/penguin.png");
		return;
	}

	CImageInfo Info;
	Info.m_Width = 32;
	Info.m_Height = 32;
	Info.m_Format = CImageInfo::FORMAT_RGBA;
	Info.m_pData = static_cast<uint8_t *>(calloc(32 * 32, 4));
	if(!Info.m_pData)
		return;
	GeneratePenguin(Info.m_pData);
	dbg_msg("tcbadge", "generated builtin: px[0]=%02x%02x%02x%02x px[16*4]=%02x%02x%02x%02x",
		Info.m_pData[0], Info.m_pData[1], Info.m_pData[2], Info.m_pData[3],
		Info.m_pData[64], Info.m_pData[65], Info.m_pData[66], Info.m_pData[67]);
	m_Texture = Graphics()->LoadTextureRawMove(Info, 0, "tclient-penguin");
}

void CClientBadge::RenderBadge(vec2 Pos, float Size, float Alpha)
{
	TryLoadTexture();
	if(!m_Texture.IsValid())
		return;

	Graphics()->TextureSet(m_Texture);
	Graphics()->BlendNormal();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
	const IGraphics::CQuadItem Quad(Pos.x, Pos.y, Size, Size);
	Graphics()->QuadsDrawTL(&Quad, 1);
	Graphics()->QuadsEnd();
}
