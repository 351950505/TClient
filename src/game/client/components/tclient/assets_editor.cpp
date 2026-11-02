#include "assets_editor.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/client.h>
#include <engine/gfx/image_loader.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <generated/client_data.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(CONF_FAMILY_WINDOWS)
#include <base/windows.h>
#endif

// 类别 -> 内置贴图 id / 资产目录名
static const char *s_apTypeNames[CAssetsEditor::TYPE_COUNT] = {
	"game", "emoticons", "entities", "skin", "hud", "particles", "gui_cursor", "arrow", "strong_weak", "extras"};

static int ImageIdForType(int Type)
{
	switch(Type)
	{
	case CAssetsEditor::TYPE_GAME: return IMAGE_GAME;
	case CAssetsEditor::TYPE_EMOTICONS: return IMAGE_EMOTICONS;
	case CAssetsEditor::TYPE_HUD: return IMAGE_HUD;
	case CAssetsEditor::TYPE_PARTICLES: return IMAGE_PARTICLES;
	case CAssetsEditor::TYPE_EXTRAS: return IMAGE_EXTRAS;
	case CAssetsEditor::TYPE_GUI_CURSOR: return IMAGE_CURSOR;
	case CAssetsEditor::TYPE_ARROW: return IMAGE_ARROW;
	case CAssetsEditor::TYPE_STRONG_WEAK: return IMAGE_STRONGWEAK;
	default: return -1;
	}
}

IGraphics *CAssetsEditor::Graphics() const { return m_pClient ? m_pClient->Graphics() : nullptr; }
IStorage *CAssetsEditor::Storage() const { return m_pClient ? m_pClient->Storage() : nullptr; }

const char *CAssetsEditor::TypeName(int Type)
{
	if(Type < 0 || Type >= TYPE_COUNT)
		return "game";
	return s_apTypeNames[Type];
}

bool CAssetsEditor::TypeHasImage(int Type)
{
	return ImageIdForType(Type) >= 0;
}

char *CAssetsEditor::ConfigVariable() const
{
	switch(m_Type)
	{
	case TYPE_GAME: return g_Config.m_ClAssetGame;
	case TYPE_EMOTICONS: return g_Config.m_ClAssetEmoticons;
	case TYPE_PARTICLES: return g_Config.m_ClAssetParticles;
	case TYPE_HUD: return g_Config.m_ClAssetHud;
	case TYPE_EXTRAS: return g_Config.m_ClAssetExtras;
	default: return nullptr;
	}
}

const char *CAssetsEditor::MainAssetName() const
{
	const char *pName = m_aaMainAsset[m_Type];
	return pName[0] ? pName : "default";
}

const char *CAssetsEditor::DonorAssetName() const
{
	const char *pName = m_aaDonorAsset[m_Type];
	return pName[0] ? pName : "default";
}

void CAssetsEditor::SetMainAsset(const char *pName)
{
	if(!pName || !pName[0])
		pName = "default";
	if(str_comp(MainAssetName(), pName) == 0)
		return;
	str_copy(m_aaMainAsset[m_Type], pName, sizeof(m_aaMainAsset[m_Type]));
	m_Revision++;
	// 槽位的默认来源跟着主素材走，换主素材等于换画布底，重建槽位。
	if(m_Loaded)
		BuildSlots();
}

void CAssetsEditor::SetDonorAsset(const char *pName)
{
	if(!pName || !pName[0])
		pName = "default";
	str_copy(m_aaDonorAsset[m_Type], pName, sizeof(m_aaDonorAsset[m_Type]));
	m_Revision++;
}

void CAssetsEditor::SetType(int Type)
{
	if(Type < 0 || Type >= TYPE_COUNT || Type == m_Type)
		return;
	m_Type = Type;
	m_Loaded = false;
	m_vSlots.clear();
	m_vAssets.clear();
	m_CanvasW = 0;
	m_CanvasH = 0;
	m_Revision++;
	for(auto &Entry : m_vImageCache)
		Entry.m_Image.Free();
	m_vImageCache.clear();
}

static float BlendChannel(float Base, float Tint, int Mode)
{
	switch(Mode)
	{
	case 0: return Base * Tint;                             // multiply
	case 1: return Tint;                                    // normal
	case 2: return 1.0f - (1.0f - Base) * (1.0f - Tint);    // screen
	case 3: return Base < 0.5f ? 2.0f * Base * Tint : 1.0f - 2.0f * (1.0f - Base) * (1.0f - Tint); // overlay
	case 4: return std::min(Base, Tint);                     // darken
	case 5: return Tint <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - Base) / Tint); // color burn
	case 6: return std::max(Base, Tint);                     // lighten
	case 7: return Tint >= 1.0f ? 1.0f : std::min(1.0f, Base / (1.0f - Tint)); // color dodge
	case 8:                                                  // soft light
		if(Tint <= 0.5f)
			return Base - (1.0f - 2.0f * Tint) * Base * (1.0f - Base);
		return Base + (2.0f * Tint - 1.0f) * (std::sqrt(Base) - Base);
	case 9: return Tint <= 0.5f ? 2.0f * Base * Tint : 1.0f - 2.0f * (1.0f - Base) * (1.0f - Tint); // hard light
	case 10: return std::fabs(Base - Tint);                  // difference
	case 11: return Base + Tint - 2.0f * Base * Tint;        // exclusion
	case 12: case 13: case 14: case 15:                      // hue/sat/color/lum: 简化为直接替换
		return Tint;
	default: return Base * Tint;
	}
}

static float Luma(const ColorRGBA &C) { return 0.299f * C.r + 0.587f * C.g + 0.114f * C.b; }

ColorRGBA CAssetsEditor::ApplyBlend(const ColorRGBA &Base, const ColorRGBA &Tint, int BlendMode, int Strength)
{
	const float S = std::clamp((float)Strength / 100.0f, 0.0f, 1.0f);
	ColorRGBA Result;
	const int Mode = std::clamp(BlendMode, 0, (int)BLEND_COUNT - 1);

	if(Mode == BLEND_HUE || Mode == BLEND_SATURATION || Mode == BLEND_COLOR || Mode == BLEND_LUMINOSITY)
	{
		// HSL 分量替换：保持 Base 的明度，只取 Tint 的色相/饱和度分量
		const ColorHSLA BaseHsl = color_cast<ColorHSLA>(Base);
		const ColorHSLA TintHsl = color_cast<ColorHSLA>(Tint);
		ColorHSLA Mixed = BaseHsl;
		if(Mode == BLEND_HUE)
			Mixed.h = TintHsl.h;
		else if(Mode == BLEND_SATURATION)
			Mixed.s = TintHsl.s;
		else if(Mode == BLEND_COLOR)
		{
			Mixed.h = TintHsl.h;
			Mixed.s = TintHsl.s;
		}
		else
			Mixed.l = TintHsl.l;
		const ColorRGBA FromHsl = color_cast<ColorRGBA>(Mixed);
		Result = ColorRGBA(
			Base.r + (FromHsl.r - Base.r) * S * Tint.a,
			Base.g + (FromHsl.g - Base.g) * S * Tint.a,
			Base.b + (FromHsl.b - Base.b) * S * Tint.a,
			Base.a);
		return Result;
	}

	Result = ColorRGBA(
		std::clamp(Base.r + (BlendChannel(Base.r, Tint.r, Mode) - Base.r) * S * Tint.a, 0.0f, 1.0f),
		std::clamp(Base.g + (BlendChannel(Base.g, Tint.g, Mode) - Base.g) * S * Tint.a, 0.0f, 1.0f),
		std::clamp(Base.b + (BlendChannel(Base.b, Tint.b, Mode) - Base.b) * S * Tint.a, 0.0f, 1.0f),
		Base.a);
	return Result;
}

// 最近邻缩放 + 调色，写入目标矩形。
static void BlitScaledTinted(CImageInfo &Dst, const CImageInfo &Src,
	int DstX, int DstY, int DstW, int DstH,
	int SrcX, int SrcY, int SrcW, int SrcH,
	const ColorRGBA &Tint, int BlendMode, int Strength)
{
	if(!Dst.m_pData || !Src.m_pData || DstW <= 0 || DstH <= 0 || SrcW <= 0 || SrcH <= 0)
		return;
	if(Dst.m_Format != CImageInfo::FORMAT_RGBA || Src.m_Format != CImageInfo::FORMAT_RGBA)
		return;

	const bool TintIdentity = Strength <= 0 || Tint.a <= 0.0f || (BlendMode == CAssetsEditor::BLEND_MULTIPLY && Tint.r >= 0.999f && Tint.g >= 0.999f && Tint.b >= 0.999f);
	for(int y = 0; y < DstH; y++)
	{
		const int Dy = DstY + y;
		if(Dy < 0 || Dy >= (int)Dst.m_Height)
			continue;
		const int Sy = SrcY + y * SrcH / DstH;
		if(Sy < 0 || Sy >= (int)Src.m_Height)
			continue;
		for(int x = 0; x < DstW; x++)
		{
			const int Dx = DstX + x;
			if(Dx < 0 || Dx >= (int)Dst.m_Width)
				continue;
			const int Sx = SrcX + x * SrcW / DstW;
			if(Sx < 0 || Sx >= (int)Src.m_Width)
				continue;

			const uint8_t *pSrc = Src.m_pData + ((size_t)Sy * Src.m_Width + (size_t)Sx) * 4;
			uint8_t *pDst = Dst.m_pData + ((size_t)Dy * Dst.m_Width + (size_t)Dx) * 4;
			if(TintIdentity)
			{
				pDst[0] = pSrc[0];
				pDst[1] = pSrc[1];
				pDst[2] = pSrc[2];
				pDst[3] = pSrc[3];
				continue;
			}
			const ColorRGBA Base(pSrc[0] / 255.0f, pSrc[1] / 255.0f, pSrc[2] / 255.0f, pSrc[3] / 255.0f);
			const ColorRGBA Mixed = CAssetsEditor::ApplyBlend(Base, Tint, BlendMode, Strength);
			pDst[0] = (uint8_t)std::lround(std::clamp(Mixed.r, 0.0f, 1.0f) * 255.0f);
			pDst[1] = (uint8_t)std::lround(std::clamp(Mixed.g, 0.0f, 1.0f) * 255.0f);
			pDst[2] = (uint8_t)std::lround(std::clamp(Mixed.b, 0.0f, 1.0f) * 255.0f);
			pDst[3] = pSrc[3];
		}
	}
}

const CImageInfo *CAssetsEditor::LoadImage(const char *pAssetName) const
{
	char aPath[IO_MAX_PATH_LENGTH];
	if(str_comp(pAssetName, "default") == 0)
	{
		const int ImageId = ImageIdForType(m_Type);
		if(ImageId >= 0)
			str_copy(aPath, g_pData->m_aImages[ImageId].m_pFilename);
		else if(m_Type == TYPE_SKIN)
			str_copy(aPath, "skins/default.png");
		else if(m_Type == TYPE_ENTITIES)
			str_copy(aPath, "editor/entities_clear/ddnet.png");
		else
			return nullptr;
	}
	else
	{
		// 优先用扫描时解析好的路径（皮肤来自 skins/，其余来自 assets/<类别>/）
		aPath[0] = '\0';
		for(const SAssetEntry &Entry : m_vAssets)
		{
			if(str_comp(Entry.m_aName, pAssetName) == 0 && Entry.m_aPath[0])
			{
				str_copy(aPath, Entry.m_aPath);
				break;
			}
		}
		if(!aPath[0])
		{
			// 兜底：优先 assets/<类别>/<名字>/<类别>.png（目录式），其次单文件
			str_format(aPath, sizeof(aPath), "assets/%s/%s/%s.png", TypeName(m_Type), pAssetName, TypeName(m_Type));
			if(!Storage()->FileExists(aPath, IStorage::TYPE_ALL))
				str_format(aPath, sizeof(aPath), "assets/%s/%s.png", TypeName(m_Type), pAssetName);
		}
	}

	for(auto &Entry : m_vImageCache)
	{
		if(Entry.m_Path == aPath && Entry.m_Image.m_pData)
			return &Entry.m_Image;
	}

	CImageInfo Info;
	if(!Graphics()->LoadPng(Info, aPath, IStorage::TYPE_ALL))
		return nullptr;

	SCachedImage Cached;
	Cached.m_Path = aPath;
	Cached.m_Image = std::move(Info);
	m_vImageCache.push_back(std::move(Cached));
	return &m_vImageCache.back().m_Image;
}

bool CAssetsEditor::BuildSlots()
{
	m_vSlots.clear();
	m_CanvasW = 0;
	m_CanvasH = 0;
	m_Revision++;
	const CImageInfo *pDefault = LoadImage("default");
	if(!pDefault)
		return false;
	m_CanvasW = (int)pDefault->m_Width;
	m_CanvasH = (int)pDefault->m_Height;
	const char *pMainName = MainAssetName();

	const int ImageId = ImageIdForType(m_Type);
	if(ImageId < 0)
	{
		// 没有 sprite 表的类别（皮肤/实体/光标等）：按整图或固定网格切
		if(m_Type == TYPE_ENTITIES)
		{
			// entities.png 是 16x16 网格，每格固定 1 单位（按整图宽高均分）
			for(int Y = 0; Y < 16; Y++)
			{
				for(int X = 0; X < 16; X++)
				{
					SSlot Slot;
					Slot.m_DstX = m_CanvasW * X / 16;
					Slot.m_DstY = m_CanvasH * Y / 16;
					Slot.m_DstW = m_CanvasW / 16;
					Slot.m_DstH = m_CanvasH / 16;
					Slot.m_SrcX = Slot.m_DstX;
					Slot.m_SrcY = Slot.m_DstY;
					Slot.m_SrcW = Slot.m_DstW;
					Slot.m_SrcH = Slot.m_DstH;
					str_copy(Slot.m_aSource, pMainName);
					str_format(Slot.m_aFamily, sizeof(Slot.m_aFamily), "entities:%d:%d", X, Y);
					str_format(Slot.m_aLabel, sizeof(Slot.m_aLabel), "%d,%d", X, Y);
					m_vSlots.push_back(Slot);
				}
			}
		}
		else if(m_Type == TYPE_SKIN)
		{
			// 皮肤按 QmClient 的部件划分：身体/双脚两块 96x96，背部与底部 32 像素条。
			// 不再切成 8x8 小格——那样左侧拖拽区全是碎片，看不出部位。
			struct SSkinSlotDef
			{
				const char *m_pFamily;
				const char *m_pLabel;
				int m_X, m_Y, m_W, m_H;
			};
			static const SSkinSlotDef s_aSkinSlotDefs[] = {
				{"skin:body", "body", 0, 0, 96, 96},
				{"skin:feet", "feet", 96, 0, 96, 96},
				{"skin:right_strip_0", "right0", 192, 0, 32, 32},
				{"skin:right_strip_1", "right1", 224, 0, 32, 32},
				{"skin:right_strip_2", "right2", 192, 32, 64, 32},
				{"skin:right_strip_3", "right3", 192, 64, 64, 32},
				{"skin:bottom_strip_0", "bottom0", 0, 96, 32, 32},
				{"skin:bottom_strip_1", "bottom1", 32, 96, 32, 32},
				{"skin:bottom_strip_2", "bottom2", 64, 96, 32, 32},
				{"skin:bottom_strip_3", "bottom3", 96, 96, 32, 32},
				{"skin:bottom_strip_4", "bottom4", 128, 96, 32, 32},
				{"skin:bottom_strip_5", "bottom5", 160, 96, 32, 32},
				{"skin:bottom_strip_6", "bottom6", 192, 96, 32, 32},
				{"skin:bottom_strip_7", "bottom7", 224, 96, 32, 32},
			};
			for(const SSkinSlotDef &Def : s_aSkinSlotDefs)
			{
				if(Def.m_X + Def.m_W > m_CanvasW || Def.m_Y + Def.m_H > m_CanvasH)
					continue;
				SSlot Slot;
				Slot.m_DstX = Def.m_X;
				Slot.m_DstY = Def.m_Y;
				Slot.m_DstW = Def.m_W;
				Slot.m_DstH = Def.m_H;
				Slot.m_SrcX = Def.m_X;
				Slot.m_SrcY = Def.m_Y;
				Slot.m_SrcW = Def.m_W;
				Slot.m_SrcH = Def.m_H;
				str_copy(Slot.m_aSource, pMainName);
				str_copy(Slot.m_aFamily, Def.m_pFamily);
				str_copy(Slot.m_aLabel, Def.m_pLabel);
				m_vSlots.push_back(Slot);
			}
		}
		else
		{
			// 光标/箭头/strongweak/extras：整图一个槽
			SSlot Slot;
			Slot.m_DstW = m_CanvasW;
			Slot.m_DstH = m_CanvasH;
			Slot.m_SrcW = m_CanvasW;
			Slot.m_SrcH = m_CanvasH;
			str_copy(Slot.m_aSource, pMainName);
			str_copy(Slot.m_aFamily, "full");
			str_copy(Slot.m_aLabel, TypeName(m_Type));
			m_vSlots.push_back(Slot);
		}
		return true;
	}

	// 有 sprite 表的类别：按 sprite 定义切槽；同几何的 sprite 合并
	const CDataImage *pImage = &g_pData->m_aImages[ImageId];
	struct SGeom
	{
		int m_X, m_Y, m_W, m_H;
		int m_FirstSprite;
	};
	std::vector<SGeom> vGeoms;
	for(int SpriteId = 0; SpriteId < NUM_SPRITES; SpriteId++)
	{
		const CDataSprite &Sprite = g_pData->m_aSprites[SpriteId];
		if(Sprite.m_pSet == nullptr || Sprite.m_pSet->m_pImage != pImage)
			continue;
		if(Sprite.m_W <= 0 || Sprite.m_H <= 0)
			continue;
		bool Found = false;
		for(const SGeom &G : vGeoms)
		{
			if(G.m_X == Sprite.m_X && G.m_Y == Sprite.m_Y && G.m_W == Sprite.m_W && G.m_H == Sprite.m_H)
			{
				Found = true;
				break;
			}
		}
		if(Found)
			continue;
		vGeoms.push_back({Sprite.m_X, Sprite.m_Y, Sprite.m_W, Sprite.m_H, SpriteId});
	}

	// sprite 坐标是「格」单位，需要乘格尺寸换成像素
	const int GridX = std::max(1, pImage->m_Id.IsValid() ? (int)pDefault->m_Width : 1);
	for(const SGeom &G : vGeoms)
	{
		const CDataSprite &Sprite = g_pData->m_aSprites[G.m_FirstSprite];
		const int CellW = Sprite.m_pSet->m_Gridx > 0 ? (int)pDefault->m_Width / Sprite.m_pSet->m_Gridx : 1;
		const int CellH = Sprite.m_pSet->m_Gridy > 0 ? (int)pDefault->m_Height / Sprite.m_pSet->m_Gridy : 1;
		SSlot Slot;
		Slot.m_DstX = G.m_X * CellW;
		Slot.m_DstY = G.m_Y * CellH;
		Slot.m_DstW = G.m_W * CellW;
		Slot.m_DstH = G.m_H * CellH;
		Slot.m_SrcX = Slot.m_DstX;
		Slot.m_SrcY = Slot.m_DstY;
		Slot.m_SrcW = Slot.m_DstW;
		Slot.m_SrcH = Slot.m_DstH;
		str_copy(Slot.m_aSource, pMainName);
		str_copy(Slot.m_aLabel, Sprite.m_pName ? Sprite.m_pName : "part");
		str_copy(Slot.m_aFamily, Slot.m_aLabel);
		m_vSlots.push_back(Slot);
	}
	(void)GridX;
	return !m_vSlots.empty();
}

void CAssetsEditor::EnsureLoaded()
{
	if(m_Loaded)
		return;
	m_Loaded = true;
	ReloadAssets();
	BuildSlots();
}

void CAssetsEditor::ReloadAssets()
{
	for(auto &Entry : m_vImageCache)
		Entry.m_Image.Free();
	m_vImageCache.clear();
	m_vAssets.clear();
	m_Revision++;
	SAssetEntry Default;
	Default.m_IsDefault = true;
	str_copy(Default.m_aName, "default");
	str_copy(Default.m_aPath, "default");
	m_vAssets.push_back(Default);

	// 皮肤类别的来源素材就是 skins/ 下的整张皮肤（与 QmClient 一致），
	// 而不是 assets/skin 里的导出资材，否则左侧拖拽区没有可用的皮肤可选。
	if(m_Type == TYPE_SKIN)
	{
		Storage()->ListDirectory(IStorage::TYPE_ALL, "skins", [](const char *pName, int IsDir, int DirType, void *pUser) {
			CAssetsEditor *pThis = (CAssetsEditor *)pUser;
			if(IsDir || !str_endswith(pName, ".png"))
				return 0;
			SAssetEntry Entry;
			str_truncate(Entry.m_aName, sizeof(Entry.m_aName), pName, str_length(pName) - 4);
			if(str_comp(Entry.m_aName, "default") == 0)
				return 0;
			for(const SAssetEntry &Existing : pThis->m_vAssets)
			{
				if(str_comp(Existing.m_aName, Entry.m_aName) == 0)
					return 0;
			}
			str_format(Entry.m_aPath, sizeof(Entry.m_aPath), "skins/%s.png", Entry.m_aName);
			pThis->m_vAssets.push_back(Entry);
			return 0;
		}, this);
		return;
	}

	// 扫描 assets/<类别>/ 下的单文件和子目录，同时把解析后的真实路径记下来
	char aDir[IO_MAX_PATH_LENGTH];
	str_format(aDir, sizeof(aDir), "assets/%s", TypeName(m_Type));
	Storage()->ListDirectory(IStorage::TYPE_ALL, aDir, [](const char *pName, int IsDir, int DirType, void *pUser) {
		CAssetsEditor *pThis = (CAssetsEditor *)pUser;
		if(str_comp(pName, "default") == 0)
			return 0;
		if(!IsDir && !str_endswith(pName, ".png"))
			return 0;
		SAssetEntry Entry;
		if(IsDir)
		{
			str_copy(Entry.m_aName, pName);
			str_format(Entry.m_aPath, sizeof(Entry.m_aPath), "assets/%s/%s/%s.png", CAssetsEditor::TypeName(pThis->m_Type), pName, CAssetsEditor::TypeName(pThis->m_Type));
			if(!pThis->Storage()->FileExists(Entry.m_aPath, IStorage::TYPE_ALL))
			{
				str_format(Entry.m_aPath, sizeof(Entry.m_aPath), "assets/%s/%s.png", CAssetsEditor::TypeName(pThis->m_Type), pName);
				if(!pThis->Storage()->FileExists(Entry.m_aPath, IStorage::TYPE_ALL))
					return 0;
			}
		}
		else
		{
			str_truncate(Entry.m_aName, sizeof(Entry.m_aName), pName, str_length(pName) - 4);
			str_format(Entry.m_aPath, sizeof(Entry.m_aPath), "assets/%s/%s.png", CAssetsEditor::TypeName(pThis->m_Type), Entry.m_aName);
		}
		for(const SAssetEntry &Existing : pThis->m_vAssets)
		{
			if(str_comp(Existing.m_aName, Entry.m_aName) == 0)
				return 0;
		}
		pThis->m_vAssets.push_back(Entry);
		return 0;
	}, this);
}

void CAssetsEditor::ResetSlots()
{
	BuildSlots();
}

bool CAssetsEditor::Compose(CImageInfo &Out) const
{
	const char *pMainName = MainAssetName();
	const CImageInfo *pMain = LoadImage(pMainName);
	if(!pMain || !pMain->m_pData || pMain->m_Format != CImageInfo::FORMAT_RGBA || m_CanvasW <= 0 || m_CanvasH <= 0)
		return false;

	CImageInfo Result;
	Result.m_Width = (size_t)m_CanvasW;
	Result.m_Height = (size_t)m_CanvasH;
	Result.m_Format = CImageInfo::FORMAT_RGBA;
	Result.AllocateFillZero();
	if(!Result.m_pData)
		return false;

	// 底图：整张主素材。主素材与画布同尺寸时直接拷贝，否则缩放铺满。
	if((int)pMain->m_Width == m_CanvasW && (int)pMain->m_Height == m_CanvasH)
	{
		const size_t RowBytes = (size_t)m_CanvasW * 4;
		for(int y = 0; y < m_CanvasH; y++)
			mem_copy(Result.m_pData + (size_t)y * RowBytes, pMain->m_pData + (size_t)y * pMain->m_Width * 4, RowBytes);
	}
	else
	{
		BlitScaledTinted(Result, *pMain, 0, 0, m_CanvasW, m_CanvasH, 0, 0, (int)pMain->m_Width, (int)pMain->m_Height,
			ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), BLEND_MULTIPLY, 100);
	}

	for(const SSlot &Slot : m_vSlots)
	{
		const bool Identity = Slot.m_BlendStrength <= 0 || Slot.m_Tint.a <= 0.0f || (Slot.m_BlendMode == BLEND_MULTIPLY && Slot.m_Tint.r >= 0.999f && Slot.m_Tint.g >= 0.999f && Slot.m_Tint.b >= 0.999f);
		const bool SameSource = str_comp(Slot.m_aSource, pMainName) == 0;
		// 来源和调色都没改：底图里已经是这块内容
		if(Identity && SameSource && Slot.m_SrcX == Slot.m_DstX && Slot.m_SrcY == Slot.m_DstY &&
			Slot.m_SrcW == Slot.m_DstW && Slot.m_SrcH == Slot.m_DstH)
			continue;

		const CImageInfo *pSource = LoadImage(Slot.m_aSource);
		if(!pSource || !pSource->m_pData || pSource->m_Format != CImageInfo::FORMAT_RGBA)
		{
			Result.Free();
			return false;
		}
		int SrcX = Slot.m_SrcX, SrcY = Slot.m_SrcY, SrcW = Slot.m_SrcW, SrcH = Slot.m_SrcH;
		if(SameSource && SrcX == Slot.m_DstX && SrcY == Slot.m_DstY && SrcW == Slot.m_DstW && SrcH == Slot.m_DstH)
		{
			const int Right = (int)((int64_t)(SrcX + SrcW) * pSource->m_Width / m_CanvasW);
			const int Bottom = (int)((int64_t)(SrcY + SrcH) * pSource->m_Height / m_CanvasH);
			SrcX = (int)((int64_t)SrcX * pSource->m_Width / m_CanvasW);
			SrcY = (int)((int64_t)SrcY * pSource->m_Height / m_CanvasH);
			SrcW = Right - SrcX;
			SrcH = Bottom - SrcY;
		}
		if(SrcX < 0 || SrcY < 0 || SrcW <= 0 || SrcH <= 0 || (int64_t)SrcX + SrcW > (int64_t)pSource->m_Width || (int64_t)SrcY + SrcH > (int64_t)pSource->m_Height)
		{
			Result.Free();
			return false;
		}
		BlitScaledTinted(Result, *pSource,
			Slot.m_DstX, Slot.m_DstY, Slot.m_DstW, Slot.m_DstH,
			SrcX, SrcY, SrcW, SrcH,
			Slot.m_Tint, Slot.m_BlendMode, Slot.m_BlendStrength);
	}

	Out = std::move(Result);
	return true;
}

void CAssetsEditor::ApplyDrop(int TargetSlotIndex, int DonorSlotIndex, const char *pDonorName)
{
	if(TargetSlotIndex < 0 || TargetSlotIndex >= (int)m_vSlots.size() || DonorSlotIndex < 0 || DonorSlotIndex >= (int)m_vSlots.size())
		return;
	if(!pDonorName || !pDonorName[0])
		return;

	const SSlot &DonorSlot = m_vSlots[DonorSlotIndex];
	SSlot &TargetSlot = m_vSlots[TargetSlotIndex];

	// 来源素材尺寸可能和画布不同，按比例把来源矩形映射到 donor 的像素坐标。
	const CImageInfo *pDonorImage = LoadImage(pDonorName);
	if(!pDonorImage || !pDonorImage->m_pData || pDonorImage->m_Format != CImageInfo::FORMAT_RGBA || m_CanvasW <= 0 || m_CanvasH <= 0 || pDonorImage->m_Width == 0 || pDonorImage->m_Height == 0)
		return;
	const int SrcImgW = (int)pDonorImage->m_Width;
	const int SrcImgH = (int)pDonorImage->m_Height;
	int SrcX = DonorSlot.m_DstX;
	int SrcY = DonorSlot.m_DstY;
	int SrcW = DonorSlot.m_DstW;
	int SrcH = DonorSlot.m_DstH;
	if(m_CanvasW > 0 && m_CanvasH > 0 && (SrcImgW != m_CanvasW || SrcImgH != m_CanvasH))
	{
		const int Right = (int)((int64_t)(SrcX + SrcW) * SrcImgW / m_CanvasW);
		const int Bottom = (int)((int64_t)(SrcY + SrcH) * SrcImgH / m_CanvasH);
		SrcX = (int)((int64_t)SrcX * SrcImgW / m_CanvasW);
		SrcY = (int)((int64_t)SrcY * SrcImgH / m_CanvasH);
		SrcW = Right - SrcX;
		SrcH = Bottom - SrcY;
	}
	if(SrcX < 0 || SrcY < 0 || SrcW <= 0 || SrcH <= 0 || (int64_t)SrcX + SrcW > SrcImgW || (int64_t)SrcY + SrcH > SrcImgH)
		return;
	str_copy(TargetSlot.m_aSource, pDonorName);
	TargetSlot.m_SrcX = std::clamp(SrcX, 0, std::max(0, SrcImgW - 1));
	TargetSlot.m_SrcY = std::clamp(SrcY, 0, std::max(0, SrcImgH - 1));
	TargetSlot.m_SrcW = std::max(1, std::min(SrcW, SrcImgW - TargetSlot.m_SrcX));
	TargetSlot.m_SrcH = std::max(1, std::min(SrcH, SrcImgH - TargetSlot.m_SrcY));
	m_Revision++;
}

void CAssetsEditor::ResetSlot(int SlotIndex)
{
	if(SlotIndex < 0 || SlotIndex >= (int)m_vSlots.size())
		return;
	SSlot &Slot = m_vSlots[SlotIndex];
	str_copy(Slot.m_aSource, MainAssetName());
	Slot.m_SrcX = Slot.m_DstX;
	Slot.m_SrcY = Slot.m_DstY;
	Slot.m_SrcW = Slot.m_DstW;
	Slot.m_SrcH = Slot.m_DstH;
	Slot.m_Tint = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	Slot.m_BlendMode = BLEND_MULTIPLY;
	Slot.m_BlendStrength = 100;
	m_Revision++;
}

bool CAssetsEditor::Export(const char *pName, char *pOutPath, int OutPathSize, char *pError, int ErrorSize)
{
	if(pError && ErrorSize > 0)
		pError[0] = '\0';
	if(!pName || !pName[0])
	{
		if(pError)
			str_copy(pError, "empty name", ErrorSize);
		return false;
	}
	// 文件名白名单：只允许字母数字下划线横线，防止路径穿越
	for(const char *p = pName; *p; p++)
	{
		if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
		{
			if(pError)
				str_copy(pError, "name may only contain letters, digits, '_' and '-'", ErrorSize);
			return false;
		}
	}

	CImageInfo Composed;
	if(!Compose(Composed))
	{
		if(pError)
			str_copy(pError, "could not compose the asset", ErrorSize);
		return false;
	}

	char aDir[IO_MAX_PATH_LENGTH];
	if(m_Type == TYPE_SKIN)
		str_copy(aDir, "skins");
	else
		str_format(aDir, sizeof(aDir), "assets/%s", TypeName(m_Type));
	if(m_Type != TYPE_SKIN)
		Storage()->CreateFolder("assets", IStorage::TYPE_SAVE);
	Storage()->CreateFolder(aDir, IStorage::TYPE_SAVE);

	char aRel[IO_MAX_PATH_LENGTH];
	str_format(aRel, sizeof(aRel), "%s/%s.png", aDir, pName);
	char aFull[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, aRel, aFull, sizeof(aFull));
	if(Storage()->FileExists(aRel, IStorage::TYPE_ALL))
	{
		Composed.Free();
		if(pError)
			str_copy(pError, "output file already exists", ErrorSize);
		return false;
	}
#if defined(CONF_FAMILY_WINDOWS)
	IOHANDLE File = _wfopen(windows_utf8_to_wide(aFull).c_str(), L"wbx");
#else
	IOHANDLE File = fopen(aFull, "wbx");
#endif
	if(!File)
	{
		Composed.Free();
		if(pError)
			str_copy(pError, errno == EEXIST ? "output file already exists" : "could not open the output file", ErrorSize);
		return false;
	}
	const bool Ok = CImageLoader::SavePng(File, aRel, Composed);
	Composed.Free();
	if(!Ok)
	{
		if(pError)
			str_copy(pError, "could not write the PNG", ErrorSize);
		return false;
	}

	if(pOutPath && OutPathSize > 0)
		str_copy(pOutPath, aRel, OutPathSize);

	return true;
}
