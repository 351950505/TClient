#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_ASSETS_EDITOR_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_ASSETS_EDITOR_H

// TClient: 素材编辑器（移植自 QmClient 的 Assets Editor）。
//
// 把内置贴图按精灵槽位切开，每个槽位可以：
//   - 从任意素材（默认贴图或用户自定义资产）取一块矩形作为来源
//   - 叠加颜色（16 种混合模式 + 强度）
// 合成结果导出成 assets/<类别>/<名字>.png，并自动设为当前使用的资产。
//
// 与 QmClient 的差异：这里只做「换源 + 调色 + 导出」，不做拖拽到槽位之外的
// 自由摆位；槽位几何固定来自内置 sprite 定义，保证导出的贴图坐标与引擎
// 读取时一致（否则贴图错位）。

#include <base/color.h>
#include <base/types.h>

#include <engine/image.h>

#include <string>
#include <vector>

class CGameClient;
class IGraphics;
class IStorage;

class CAssetsEditor
{
	CGameClient *m_pClient = nullptr;

public:
	// 需要 GameClient 才能拿 Storage()/Graphics()；UI 侧在构造后立刻设置。
	void SetGameClient(CGameClient *pClient) { m_pClient = pClient; }

private:
	// 便捷访问：素材编辑器不是 CComponent，没有基类的这些快捷方法。
	IGraphics *Graphics() const;
	IStorage *Storage() const;

public:
	enum EType
	{
		TYPE_GAME = 0,
		TYPE_EMOTICONS,
		TYPE_ENTITIES,
		TYPE_SKIN,
		TYPE_HUD,
		TYPE_PARTICLES,
		TYPE_GUI_CURSOR,
		TYPE_ARROW,
		TYPE_STRONG_WEAK,
		TYPE_EXTRAS,
		TYPE_COUNT,
	};

	// 颜色混合模式（与 QmClient 的 16 种一致）
	enum EBlendMode
	{
		BLEND_MULTIPLY = 0,
		BLEND_NORMAL,
		BLEND_SCREEN,
		BLEND_OVERLAY,
		BLEND_DARKEN,
		BLEND_COLOR_BURN,
		BLEND_LIGHTEN,
		BLEND_COLOR_DODGE,
		BLEND_SOFT_LIGHT,
		BLEND_HARD_LIGHT,
		BLEND_DIFFERENCE,
		BLEND_EXCLUSION,
		BLEND_HUE,
		BLEND_SATURATION,
		BLEND_COLOR,
		BLEND_LUMINOSITY,
		BLEND_COUNT,
	};

	// 一个可编辑槽位。坐标全部是目标贴图上的像素矩形。
	struct SSlot
	{
		int m_DstX = 0, m_DstY = 0, m_DstW = 0, m_DstH = 0;
		// 来源矩形（相对于来源素材的像素坐标）
		int m_SrcX = 0, m_SrcY = 0, m_SrcW = 0, m_SrcH = 0;
		// 来源素材名（"default" = 内置贴图）
		char m_aSource[64] = "default";
		// 调色
		ColorRGBA m_Tint = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
		int m_BlendMode = BLEND_MULTIPLY;
		int m_BlendStrength = 100; // 0..100
		// 同族槽位（同一逻辑部件的动画帧）；同族共享来源与调色
		char m_aFamily[64] = "";
		// sprite 名字，仅用于 UI 显示
		char m_aLabel[64] = "";
	};

	// 一个可选素材（默认贴图或用户资产）
	struct SAssetEntry
	{
		char m_aName[64] = "";
		char m_aPath[IO_MAX_PATH_LENGTH] = "";
		bool m_IsDefault = false;
	};

	int Type() const { return m_Type; }
	void SetType(int Type);

	// 切换类别时重建槽位。来源素材列表会在第一次需要时扫描。
	void EnsureLoaded();
	void ReloadAssets();
	void ResetSlots();

	const std::vector<SAssetEntry> &Assets() const { return m_vAssets; }
	const std::vector<SSlot> &Slots() const { return m_vSlots; }
	std::vector<SSlot> &Slots() { return m_vSlots; }

	// 该类别导出时的固定尺寸；0 表示尚无（还没加载默认图）。
	int CanvasWidth() const { return m_CanvasW; }
	int CanvasHeight() const { return m_CanvasH; }

	// 合成：按槽位从来源素材取块、缩放、调色，写进 Out。
	// Out 会被重新分配为 CanvasWidth x CanvasHeight 的 RGBA。
	bool Compose(CImageInfo &Out) const;

	// 导出到 assets/<类别名>/<Name>.png。成功后返回完整路径，失败返回空。
	bool Export(const char *pName, char *pOutPath, int OutPathSize, char *pError, int ErrorSize);

	// 该类别对应的资产配置缓冲（可写，用于导出后自动选中）；不支持运行时切换的类别返回 nullptr。
	char *ConfigVariable() const;

	static const char *TypeName(int Type);
	static bool TypeHasImage(int Type);

	// 颜色混合（供 UI 预览用）
	static ColorRGBA ApplyBlend(const ColorRGBA &Base, const ColorRGBA &Tint, int BlendMode, int Strength);

private:
	int m_Type = TYPE_GAME;
	bool m_Loaded = false;

	std::vector<SAssetEntry> m_vAssets;
	std::vector<SSlot> m_vSlots;

	int m_CanvasW = 0;
	int m_CanvasH = 0;

	// 图像缓存：按素材路径缓存解码后的 RGBA
	struct SCachedImage
	{
		std::string m_Path;
		CImageInfo m_Image;
	};
	mutable std::vector<SCachedImage> m_vImageCache;

	const CImageInfo *LoadImage(const char *pAssetName) const;
	bool BuildSlots();
	void BuildFamilyFromSprites(const char *pLabelSuffix);
};

#endif
