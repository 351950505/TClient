/* TClient: 素材编辑器 UI（ME 设置页独立标签）。
   布局对齐 QmClient：左边是来源素材（donor）的部件网格，右边是
   「主素材 + 已换源槽位」的合成结果。从左侧按住部件拖到右侧槽位松开即完成
   替换；右键槽位恢复主素材原样；左键槽位选中后在下方调色。 */

#include "assets_editor.h"

#include <base/math.h>
#include <base/str.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/client/ui_scrollregion.h>
#include <game/localization.h>

#include <algorithm>
#include <vector>

// 与 menus_tclient.cpp 里的布局常量保持一致（那边是文件内 static，不能跨 TU 复用）。
namespace {
const float LineSize = 20.0f;
const float FontSize = 14.0f;
const float EditBoxFontSize = 12.0f;
const float HeadlineHeight = 20.0f;
const float MarginSmall = 5.0f;

const float DragThreshold = 4.0f;
const float DragGhostSize = 56.0f;

struct SEditorUiState
{
	int m_SelectedSlot = 0;
	bool m_ShowGrid = true;

	// 拖拽状态：m_DragPending 是按住但还没移动够，m_DragActive 才是真拖拽。
	bool m_DragPending = false;
	bool m_DragActive = false;
	int m_DragSlot = -1;
	vec2 m_PressPos = vec2(0.0f, 0.0f);
	char m_aDragAsset[64] = "";

	CLineInputBuffered<64> m_DonorSearch;
	CLineInputBuffered<64> m_MainSearch;
	CLineInputBuffered<64> m_ExportName;
	bool m_ExportNameInitialized = false;

	// 贴图缓存
	IGraphics::CTextureHandle m_PreviewTexture;
	int m_UiType = -1;
	int m_PreviewType = -1;
	int m_PreviewRevision = -1;
	size_t m_PreviewHash = 0;
	IGraphics::CTextureHandle m_DonorTexture;
	int m_DonorType = -1;
	int m_DonorRevision = -1;
	char m_aDonorTextureName[64] = "";

	// 控件状态
	CUi::SDropDownState m_TypeDropDown;
	CUi::SDropDownState m_DonorDropDown;
	CUi::SDropDownState m_MainDropDown;
	CScrollRegion m_TypeScrollRegion;
	CScrollRegion m_DonorScrollRegion;
	CScrollRegion m_MainScrollRegion;

	char m_aMessage[256] = "";
	float m_MessageTime = -100.0f;
};

SEditorUiState s_State;

const char *s_apTypeDisplayNames[CAssetsEditor::TYPE_COUNT] = {
	"Game", "Emoticons", "Entities", "Skin", "HUD", "Particles", "Cursor", "Arrow", "Strong/Weak", "Extras"};

const char *s_apBlendNames[CAssetsEditor::BLEND_COUNT] = {
	"Multiply", "Normal", "Screen", "Overlay", "Darken", "Color Burn", "Lighten", "Color Dodge",
	"Soft Light", "Hard Light", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity"};

bool CalcFittedRect(const CUIRect &Rect, int Width, int Height, CUIRect &Out)
{
	if(Width <= 0 || Height <= 0 || Rect.w <= 0.0f || Rect.h <= 0.0f)
		return false;
	float DrawW = Rect.w;
	float DrawH = DrawW * (float)Height / (float)Width;
	if(DrawH > Rect.h)
	{
		DrawH = Rect.h;
		DrawW = DrawH * (float)Width / (float)Height;
	}
	Out = CUIRect(Rect.x + (Rect.w - DrawW) * 0.5f, Rect.y + (Rect.h - DrawH) * 0.5f, DrawW, DrawH);
	return true;
}

void GetSlotRect(const CUIRect &Canvas, int CanvasW, int CanvasH, const CAssetsEditor::SSlot &Slot, CUIRect &Out)
{
	Out = CUIRect(
		Canvas.x + Slot.m_DstX * Canvas.w / (float)CanvasW,
		Canvas.y + Slot.m_DstY * Canvas.h / (float)CanvasH,
		Slot.m_DstW * Canvas.w / (float)CanvasW,
		Slot.m_DstH * Canvas.h / (float)CanvasH);
}

// 返回鼠标下面积最小的槽位：大部件里套小部件时优先命中小的。
int FindSlotAt(const CUIRect &Canvas, int CanvasW, int CanvasH, const std::vector<CAssetsEditor::SSlot> &vSlots, float MouseX, float MouseY)
{
	if(CanvasW <= 0 || CanvasH <= 0 || Canvas.w <= 0.0f || Canvas.h <= 0.0f)
		return -1;
	int Best = -1;
	float BestArea = 0.0f;
	for(size_t i = 0; i < vSlots.size(); i++)
	{
		CUIRect R;
		GetSlotRect(Canvas, CanvasW, CanvasH, vSlots[i], R);
		if(MouseX < R.x || MouseY < R.y || MouseX >= R.x + R.w || MouseY >= R.y + R.h)
			continue;
		const float Area = R.w * R.h;
		if(Best < 0 || Area < BestArea)
		{
			Best = (int)i;
			BestArea = Area;
		}
	}
	return Best;
}

void DrawCheckerboard(IGraphics *pGraphics, const CUIRect &Rect)
{
	const float Cell = 12.0f;
	const ColorRGBA ColorA(0.13f, 0.13f, 0.16f, 1.0f);
	const ColorRGBA ColorB(0.18f, 0.18f, 0.21f, 1.0f);
	pGraphics->TextureClear();
	pGraphics->QuadsBegin();
	int Row = 0;
	for(float y = 0.0f; y < Rect.h; y += Cell, Row++)
	{
		int Col = 0;
		for(float x = 0.0f; x < Rect.w; x += Cell, Col++)
		{
			pGraphics->SetColor((Row + Col) % 2 == 0 ? ColorA : ColorB);
			const IGraphics::CQuadItem Quad(Rect.x + x, Rect.y + y, std::min(Cell, Rect.w - x), std::min(Cell, Rect.h - y));
			pGraphics->QuadsDrawTL(&Quad, 1);
		}
	}
	pGraphics->QuadsEnd();
}

void DrawTextureSubset(IGraphics *pGraphics, const CUIRect &Rect, IGraphics::CTextureHandle Texture, float U0, float V0, float U1, float V1)
{
	if(!Texture.IsValid() || Rect.w <= 0.0f || Rect.h <= 0.0f)
		return;
	pGraphics->WrapClamp();
	pGraphics->TextureSet(Texture);
	pGraphics->QuadsBegin();
	pGraphics->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	pGraphics->QuadsSetSubset(U0, V0, U1, V1);
	const IGraphics::CQuadItem Quad(Rect.x, Rect.y, Rect.w, Rect.h);
	pGraphics->QuadsDrawTL(&Quad, 1);
	pGraphics->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
	pGraphics->QuadsEnd();
	pGraphics->TextureClear();
	pGraphics->WrapNormal();
}

void DrawSlotOutlines(IGraphics *pGraphics, const CUIRect &Canvas, int CanvasW, int CanvasH, const std::vector<CAssetsEditor::SSlot> &vSlots,
	int SelectedSlot, int HoverSlot, int DragSlot)
{
	if(CanvasW <= 0 || CanvasH <= 0 || Canvas.w <= 0.0f || Canvas.h <= 0.0f)
		return;
	pGraphics->TextureClear();
	pGraphics->LinesBegin();
	for(size_t i = 0; i < vSlots.size(); i++)
	{
		CUIRect R;
		GetSlotRect(Canvas, CanvasW, CanvasH, vSlots[i], R);
		if((int)i == SelectedSlot)
			pGraphics->SetColor(ColorRGBA(1.0f, 0.85f, 0.2f, 0.95f));
		else if((int)i == DragSlot)
			pGraphics->SetColor(ColorRGBA(1.0f, 0.85f, 0.2f, 0.95f));
		else if((int)i == HoverSlot)
			pGraphics->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.85f));
		else
			pGraphics->SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.16f));
		const IGraphics::CLineItem aLines[4] = {
			IGraphics::CLineItem(R.x, R.y, R.x + R.w, R.y),
			IGraphics::CLineItem(R.x + R.w, R.y, R.x + R.w, R.y + R.h),
			IGraphics::CLineItem(R.x + R.w, R.y + R.h, R.x, R.y + R.h),
			IGraphics::CLineItem(R.x, R.y + R.h, R.x, R.y),
		};
		pGraphics->LinesDraw(aLines, 4);
	}
	pGraphics->LinesEnd();
}

bool NameMatchesFilter(const char *pName, const char *pFilter)
{
	return !pFilter[0] || str_find_nocase(pName, pFilter) != nullptr;
}

// 生成过滤后的素材索引表；当前选中的素材不在结果里时插到最前面，
// 避免下拉框显示成别的素材。
void BuildFilteredAssets(const std::vector<CAssetsEditor::SAssetEntry> &vAssets, const char *pFilter, const char *pCurrent, std::vector<int> &vOut)
{
	vOut.clear();
	int CurrentIndex = -1;
	for(size_t i = 0; i < vAssets.size(); i++)
	{
		if(str_comp(vAssets[i].m_aName, pCurrent) == 0)
			CurrentIndex = (int)i;
		if(NameMatchesFilter(vAssets[i].m_aName, pFilter))
			vOut.push_back((int)i);
	}
	if(CurrentIndex >= 0 && std::find(vOut.begin(), vOut.end(), CurrentIndex) == vOut.end())
		vOut.insert(vOut.begin(), CurrentIndex);
}

size_t HashString(size_t Hash, const char *pStr)
{
	while(*pStr)
		Hash = (Hash ^ (unsigned char)*pStr++) * 16777619u;
	return Hash;
}

size_t HashSlots(int Type, const char *pMainAsset, const std::vector<CAssetsEditor::SSlot> &vSlots)
{
	size_t Hash = 2166136261u;
	Hash = HashString(Hash, CAssetsEditor::TypeName(Type));
	Hash = HashString(Hash, pMainAsset);
	for(const auto &Slot : vSlots)
	{
		Hash = HashString(Hash, Slot.m_aSource);
		Hash ^= (size_t)(Slot.m_SrcX + 1) * 31u;
		Hash ^= (size_t)(Slot.m_SrcY + 1) * 17u;
		Hash ^= (size_t)(Slot.m_SrcW + 1) * 13u;
		Hash ^= (size_t)(Slot.m_SrcH + 1) * 7u;
		Hash ^= (size_t)(Slot.m_BlendMode + 1) * 3u;
		Hash ^= (size_t)(Slot.m_BlendStrength + 1);
		Hash ^= (size_t)(Slot.m_Tint.r * 255.0f) << 1;
		Hash ^= (size_t)(Slot.m_Tint.g * 255.0f) << 2;
		Hash ^= (size_t)(Slot.m_Tint.b * 255.0f) << 3;
	}
	return Hash;
}

void CancelDrag()
{
	s_State.m_DragPending = false;
	s_State.m_DragActive = false;
	s_State.m_DragSlot = -1;
	s_State.m_aDragAsset[0] = '\0';
}

// 类别切换后，上一类别的贴图和拖拽状态都不能再用。
void ResetUiForTypeChange(IGraphics *pGraphics)
{
	if(s_State.m_PreviewTexture.IsValid())
		pGraphics->UnloadTexture(&s_State.m_PreviewTexture);
	if(s_State.m_DonorTexture.IsValid())
		pGraphics->UnloadTexture(&s_State.m_DonorTexture);
	s_State.m_PreviewTexture = IGraphics::CTextureHandle();
	s_State.m_DonorTexture = IGraphics::CTextureHandle();
	s_State.m_PreviewType = -1;
	s_State.m_PreviewRevision = -1;
	s_State.m_PreviewHash = 0;
	s_State.m_DonorType = -1;
	s_State.m_DonorRevision = -1;
	s_State.m_aDonorTextureName[0] = '\0';
	s_State.m_SelectedSlot = 0;
	CancelDrag();
}
} // namespace

void CMenus::RenderSettingsTClientAssets(CUIRect MainView)
{
	CAssetsEditor &Editor = GameClient()->m_AssetsEditor;
	Editor.SetGameClient(GameClient());
	Editor.EnsureLoaded();

	// ---- 顶栏：类别 / 重置 / 重新加载 / 显示网格 -----------------------------
	CUIRect Row, Label, Button, DropDown;
	MainView.HSplitTop(LineSize + 4.0f, &Row, &MainView);
	MainView.HSplitTop(4.0f, nullptr, &MainView);

	Row.VSplitLeft(40.0f, &Label, &Row);
	Ui()->DoLabel(&Label, TCLocalize("Type:"), FontSize, TEXTALIGN_ML);

	const char *apTypeNames[CAssetsEditor::TYPE_COUNT];
	for(int Type = 0; Type < CAssetsEditor::TYPE_COUNT; Type++)
		apTypeNames[Type] = TCLocalize(s_apTypeDisplayNames[Type]);
	Row.VSplitLeft(116.0f, &DropDown, &Row);
	s_State.m_TypeDropDown.m_SelectionPopupContext.m_pScrollRegion = &s_State.m_TypeScrollRegion;
	const int NewType = Ui()->DoDropDown(&DropDown, Editor.Type(), apTypeNames, CAssetsEditor::TYPE_COUNT, s_State.m_TypeDropDown);
	if(NewType != Editor.Type() && NewType >= 0 && NewType < CAssetsEditor::TYPE_COUNT)
	{
		Editor.SetType(NewType);
		Editor.EnsureLoaded();
	}
	if(s_State.m_UiType != Editor.Type())
	{
		ResetUiForTypeChange(Graphics());
		s_State.m_UiType = Editor.Type();
	}

	Row.VSplitLeft(8.0f, nullptr, &Row);
	Row.VSplitLeft(96.0f, &Button, &Row);
	static CButtonContainer s_ResetSlotsButton;
	if(DoButton_Menu(&s_ResetSlotsButton, TCLocalize("Reset slots"), 0, &Button))
	{
		CancelDrag();
		Editor.ResetSlots();
		s_State.m_SelectedSlot = 0;
		str_copy(s_State.m_aMessage, TCLocalize("All slots reset."));
		s_State.m_MessageTime = Client()->LocalTime();
	}

	Row.VSplitLeft(6.0f, nullptr, &Row);
	Row.VSplitLeft(76.0f, &Button, &Row);
	static CButtonContainer s_ReloadButton;
	if(DoButton_Menu(&s_ReloadButton, TCLocalize("Reload"), 0, &Button))
	{
		CancelDrag();
		Editor.ReloadAssets();
	}

	Row.VSplitLeft(6.0f, nullptr, &Row);
	Row.VSplitLeft(110.0f, &Button, &Row);
	if(DoButton_CheckBox(&s_State.m_ShowGrid, TCLocalize("Show grid"), s_State.m_ShowGrid, &Button))
		s_State.m_ShowGrid = !s_State.m_ShowGrid;

	// ---- 底部：选中槽位设置 + 导出 ------------------------------------------
	const float BottomHeight = std::clamp(MainView.h * 0.26f, 84.0f, 110.0f);
	CUIRect Content, Bottom;
	MainView.HSplitBottom(BottomHeight, &Content, &Bottom);

	// ---- 左右两栏 ---------------------------------------------------------
	CUIRect LeftPanel, RightPanel;
	Content.VSplitMid(&LeftPanel, &RightPanel, 8.0f);
	LeftPanel.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.06f), IGraphics::CORNER_ALL, 6.0f);
	RightPanel.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.06f), IGraphics::CORNER_ALL, 6.0f);
	LeftPanel.Margin(6.0f, &LeftPanel);
	RightPanel.Margin(6.0f, &RightPanel);

	CUIRect LeftTitle, LeftCanvas, LeftBottom, LeftSearchRow, LeftAssetRow;
	LeftPanel.HSplitTop(LineSize, &LeftTitle, &LeftPanel);
	LeftPanel.HSplitTop(2.0f, nullptr, &LeftPanel);
	LeftPanel.HSplitBottom(LineSize * 2.0f + 8.0f, &LeftCanvas, &LeftBottom);
	LeftBottom.HSplitTop(LineSize, &LeftSearchRow, &LeftBottom);
	LeftBottom.HSplitTop(4.0f, nullptr, &LeftBottom);
	LeftBottom.HSplitTop(LineSize, &LeftAssetRow, &LeftBottom);

	CUIRect RightTitle, RightCanvas, RightBottom, RightSearchRow, RightAssetRow;
	RightPanel.HSplitTop(LineSize, &RightTitle, &RightPanel);
	RightPanel.HSplitTop(2.0f, nullptr, &RightPanel);
	RightPanel.HSplitBottom(LineSize * 2.0f + 8.0f, &RightCanvas, &RightBottom);
	RightBottom.HSplitTop(LineSize, &RightSearchRow, &RightBottom);
	RightBottom.HSplitTop(4.0f, nullptr, &RightBottom);
	RightBottom.HSplitTop(LineSize, &RightAssetRow, &RightBottom);

	Ui()->DoLabel(&LeftTitle, TCLocalize("Source (drag parts from the left)"), FontSize, TEXTALIGN_ML);
	Ui()->DoLabel(&RightTitle, TCLocalize("Result (drop parts on the right)"), FontSize, TEXTALIGN_ML);

	LeftCanvas.Draw(ColorRGBA(0.07f, 0.07f, 0.09f, 1.0f), IGraphics::CORNER_ALL, 4.0f);
	RightCanvas.Draw(ColorRGBA(0.07f, 0.07f, 0.09f, 1.0f), IGraphics::CORNER_ALL, 4.0f);

	// ---- 素材选择：搜索框 + 下拉 -------------------------------------------
	const std::vector<CAssetsEditor::SAssetEntry> &vAssets = Editor.Assets();
	std::vector<int> vDonorIndices, vMainIndices;
	BuildFilteredAssets(vAssets, s_State.m_DonorSearch.GetString(), Editor.DonorAssetName(), vDonorIndices);
	BuildFilteredAssets(vAssets, s_State.m_MainSearch.GetString(), Editor.MainAssetName(), vMainIndices);

	std::vector<const char *> vDonorNames, vMainNames;
	for(const int Index : vDonorIndices)
		vDonorNames.push_back(vAssets[Index].m_aName);
	for(const int Index : vMainIndices)
		vMainNames.push_back(vAssets[Index].m_aName);

	CUIRect SearchLabel, SearchBox;
	LeftSearchRow.VSplitLeft(52.0f, &SearchLabel, &SearchBox);
	Ui()->DoLabel(&SearchLabel, TCLocalize("Search"), FontSize, TEXTALIGN_ML);
	Ui()->DoEditBox_Search(&s_State.m_DonorSearch, &SearchBox, EditBoxFontSize, false);

	RightSearchRow.VSplitLeft(52.0f, &SearchLabel, &SearchBox);
	Ui()->DoLabel(&SearchLabel, TCLocalize("Search"), FontSize, TEXTALIGN_ML);
	Ui()->DoEditBox_Search(&s_State.m_MainSearch, &SearchBox, EditBoxFontSize, false);

	CUIRect AssetLabel, AssetDropDown;
	LeftAssetRow.VSplitLeft(74.0f, &AssetLabel, &AssetDropDown);
	Ui()->DoLabel(&AssetLabel, TCLocalize("Source asset"), FontSize, TEXTALIGN_ML);

	s_State.m_DonorDropDown.m_SelectionPopupContext.m_pScrollRegion = &s_State.m_DonorScrollRegion;
	int DonorSelection = 0;
	for(size_t i = 0; i < vDonorIndices.size(); i++)
	{
		if(str_comp(vAssets[vDonorIndices[i]].m_aName, Editor.DonorAssetName()) == 0)
		{
			DonorSelection = (int)i;
			break;
		}
	}
	if(!vDonorNames.empty())
	{
		const int NewDonor = Ui()->DoDropDown(&AssetDropDown, DonorSelection, vDonorNames.data(), (int)vDonorNames.size(), s_State.m_DonorDropDown);
		if(NewDonor >= 0 && NewDonor < (int)vDonorNames.size() && NewDonor != DonorSelection)
		{
			CancelDrag();
			Editor.SetDonorAsset(vDonorNames[NewDonor]);
		}
	}
	else
		Ui()->DoLabel(&AssetDropDown, TCLocalize("No assets"), FontSize, TEXTALIGN_ML);

	RightAssetRow.VSplitLeft(74.0f, &AssetLabel, &AssetDropDown);
	Ui()->DoLabel(&AssetLabel, TCLocalize("Main asset"), FontSize, TEXTALIGN_ML);

	s_State.m_MainDropDown.m_SelectionPopupContext.m_pScrollRegion = &s_State.m_MainScrollRegion;
	int MainSelection = 0;
	for(size_t i = 0; i < vMainIndices.size(); i++)
	{
		if(str_comp(vAssets[vMainIndices[i]].m_aName, Editor.MainAssetName()) == 0)
		{
			MainSelection = (int)i;
			break;
		}
	}
	if(!vMainNames.empty())
	{
		const int NewMain = Ui()->DoDropDown(&AssetDropDown, MainSelection, vMainNames.data(), (int)vMainNames.size(), s_State.m_MainDropDown);
		if(NewMain >= 0 && NewMain < (int)vMainNames.size() && NewMain != MainSelection)
		{
			CancelDrag();
			Editor.SetMainAsset(vMainNames[NewMain]);
			s_State.m_SelectedSlot = 0;
		}
	}
	else
		Ui()->DoLabel(&AssetDropDown, TCLocalize("No assets"), FontSize, TEXTALIGN_ML);

	// ---- 画布与拖拽 -------------------------------------------------------
	const std::vector<CAssetsEditor::SSlot> &vSlots = Editor.Slots();
	if(s_State.m_SelectedSlot >= (int)vSlots.size())
		s_State.m_SelectedSlot = 0;

	const CImageInfo *pDonorImage = Editor.LoadAssetImage(Editor.DonorAssetName());
	CUIRect DonorFitted;
	const bool HasDonorImage = pDonorImage && CalcFittedRect(LeftCanvas, (int)pDonorImage->m_Width, (int)pDonorImage->m_Height, DonorFitted);

	CUIRect TargetFitted;
	const bool HasTargetCanvas = Editor.CanvasWidth() > 0 && Editor.CanvasHeight() > 0 &&
				     CalcFittedRect(RightCanvas, Editor.CanvasWidth(), Editor.CanvasHeight(), TargetFitted);

	// 合成预览：槽位/主素材变化才重合成，纯 CPU 全画布遍历不适合每帧做。
	const size_t SlotsHash = HashSlots(Editor.Type(), Editor.MainAssetName(), vSlots);
	if(s_State.m_PreviewType != Editor.Type() || s_State.m_PreviewRevision != Editor.Revision() || s_State.m_PreviewHash != SlotsHash)
	{
		if(s_State.m_PreviewTexture.IsValid())
			Graphics()->UnloadTexture(&s_State.m_PreviewTexture);
		s_State.m_PreviewTexture = IGraphics::CTextureHandle();
		CImageInfo Composed;
		if(Editor.Compose(Composed))
			s_State.m_PreviewTexture = Graphics()->LoadTextureRawMove(Composed, 0, "assets-editor-preview");
		s_State.m_PreviewType = Editor.Type();
		s_State.m_PreviewRevision = Editor.Revision();
		s_State.m_PreviewHash = SlotsHash;
	}

	// 来源素材贴图：类别/素材名变化或素材列表重载时重建。
	if(HasDonorImage && (!s_State.m_DonorTexture.IsValid() || s_State.m_DonorType != Editor.Type() ||
				    s_State.m_DonorRevision != Editor.Revision() || str_comp(s_State.m_aDonorTextureName, Editor.DonorAssetName()) != 0))
	{
		if(s_State.m_DonorTexture.IsValid())
			Graphics()->UnloadTexture(&s_State.m_DonorTexture);
		s_State.m_DonorTexture = Graphics()->LoadTextureRaw(*pDonorImage, 0, "assets-editor-donor");
		s_State.m_DonorType = Editor.Type();
		s_State.m_DonorRevision = Editor.Revision();
		str_copy(s_State.m_aDonorTextureName, Editor.DonorAssetName());
	}

	// 命中检测
	const bool PopupOpen = Ui()->IsPopupOpen();
	if(PopupOpen)
		CancelDrag();
	const vec2 Mouse = Ui()->MousePos();
	int DonorHover = -1;
	int TargetHover = -1;
	if(!PopupOpen && HasDonorImage)
		DonorHover = FindSlotAt(DonorFitted, Editor.CanvasWidth(), Editor.CanvasHeight(), vSlots, Mouse.x, Mouse.y);
	if(!PopupOpen && HasTargetCanvas)
		TargetHover = FindSlotAt(TargetFitted, Editor.CanvasWidth(), Editor.CanvasHeight(), vSlots, Mouse.x, Mouse.y);

	const bool MouseDown = Ui()->MouseButton(0);
	const bool ClickedLmb = Ui()->MouseButtonClicked(0);
	const bool ClickedRmb = Ui()->MouseButtonClicked(1);

	// 右键槽位：恢复主素材原始内容。
	if(!PopupOpen && ClickedRmb && TargetHover >= 0)
	{
		Editor.ResetSlot(TargetHover);
		s_State.m_SelectedSlot = TargetHover;
		str_copy(s_State.m_aMessage, TCLocalize("Part reset to default."));
		s_State.m_MessageTime = Client()->LocalTime();
	}

	// 左键按下来源部件：先进入待定，移动超过阈值才算拖拽，避免点一下也拖起来。
	if(!PopupOpen && ClickedLmb && DonorHover >= 0 && !s_State.m_DragActive)
	{
		s_State.m_DragPending = true;
		s_State.m_DragSlot = DonorHover;
		s_State.m_PressPos = Mouse;
		str_copy(s_State.m_aDragAsset, Editor.DonorAssetName());
	}
	if(s_State.m_DragPending)
	{
		const float DeltaX = Mouse.x - s_State.m_PressPos.x;
		const float DeltaY = Mouse.y - s_State.m_PressPos.y;
		if(!MouseDown)
		{
			s_State.m_DragPending = false;
			s_State.m_DragSlot = -1;
		}
		else if(DeltaX * DeltaX + DeltaY * DeltaY >= DragThreshold * DragThreshold)
		{
			s_State.m_DragPending = false;
			s_State.m_DragActive = true;
		}
	}

	// 松开：落在右侧槽位上就换源。
	if(s_State.m_DragActive && !MouseDown)
	{
		if(TargetHover >= 0 && s_State.m_DragSlot >= 0 && s_State.m_DragSlot < (int)vSlots.size())
		{
			Editor.ApplyDrop(TargetHover, s_State.m_DragSlot, s_State.m_aDragAsset);
			s_State.m_SelectedSlot = TargetHover;
			str_copy(s_State.m_aMessage, TCLocalize("Part replaced."));
			s_State.m_MessageTime = Client()->LocalTime();
		}
		s_State.m_DragActive = false;
		s_State.m_DragSlot = -1;
	}
	if(!PopupOpen && ClickedLmb && TargetHover >= 0 && !s_State.m_DragActive)
		s_State.m_SelectedSlot = TargetHover;

	// ---- 绘制左右画布 -----------------------------------------------------
	if(HasDonorImage)
	{
		DrawCheckerboard(Graphics(), DonorFitted);
		DrawTextureSubset(Graphics(), DonorFitted, s_State.m_DonorTexture, 0.0f, 0.0f, 1.0f, 1.0f);
		if(!s_State.m_DonorTexture.IsValid())
			Ui()->DoLabel(&DonorFitted, TCLocalize("No preview"), FontSize, TEXTALIGN_MC);
		if(s_State.m_ShowGrid)
			DrawSlotOutlines(Graphics(), DonorFitted, Editor.CanvasWidth(), Editor.CanvasHeight(), vSlots, -1, DonorHover,
				s_State.m_DragActive ? s_State.m_DragSlot : -1);
	}
	else
		Ui()->DoLabel(&LeftCanvas, TCLocalize("No preview"), FontSize, TEXTALIGN_MC);

	if(HasTargetCanvas)
	{
		DrawCheckerboard(Graphics(), TargetFitted);
		DrawTextureSubset(Graphics(), TargetFitted, s_State.m_PreviewTexture, 0.0f, 0.0f, 1.0f, 1.0f);
		if(!s_State.m_PreviewTexture.IsValid())
			Ui()->DoLabel(&TargetFitted, TCLocalize("No preview"), FontSize, TEXTALIGN_MC);
		if(s_State.m_ShowGrid)
			DrawSlotOutlines(Graphics(), TargetFitted, Editor.CanvasWidth(), Editor.CanvasHeight(), vSlots, s_State.m_SelectedSlot,
				TargetHover, -1);
		if(s_State.m_DragActive && TargetHover >= 0)
		{
			// 落点高亮：能放就是绿色框。
			CUIRect R;
			GetSlotRect(TargetFitted, Editor.CanvasWidth(), Editor.CanvasHeight(), vSlots[TargetHover], R);
			R.DrawOutline(ColorRGBA(0.35f, 1.0f, 0.35f, 0.95f));
		}
	}
	else
		Ui()->DoLabel(&RightCanvas, TCLocalize("No preview"), FontSize, TEXTALIGN_MC);

	// 拖拽跟随的小图标：直接画来源素材里这一块。
	if(s_State.m_DragActive && s_State.m_DragSlot >= 0 && s_State.m_DragSlot < (int)vSlots.size() && HasDonorImage && s_State.m_DonorTexture.IsValid() &&
		Editor.CanvasWidth() > 0 && Editor.CanvasHeight() > 0 && RightCanvas.w > 0.0f && RightCanvas.h > 0.0f)
	{
		const CAssetsEditor::SSlot &DragSlot = vSlots[s_State.m_DragSlot];
		CUIRect Frame(Mouse.x + 10.0f, Mouse.y + 10.0f, DragGhostSize + 6.0f, DragGhostSize + 6.0f);
		Frame.x = std::clamp(Frame.x, RightCanvas.x, RightCanvas.x + std::max(0.0f, RightCanvas.w - Frame.w));
		Frame.y = std::clamp(Frame.y, RightCanvas.y, RightCanvas.y + std::max(0.0f, RightCanvas.h - Frame.h));
		Frame.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.6f), IGraphics::CORNER_ALL, 4.0f);

		// 左侧画布上拖起来的就是「这一格在来源图里的位置」，也就是槽位几何本身。
		const float ImgW = (float)Editor.CanvasWidth();
		const float ImgH = (float)Editor.CanvasHeight();
		const float U0 = DragSlot.m_DstX / ImgW;
		const float V0 = DragSlot.m_DstY / ImgH;
		const float U1 = (DragSlot.m_DstX + DragSlot.m_DstW) / ImgW;
		const float V1 = (DragSlot.m_DstY + DragSlot.m_DstH) / ImgH;

		CUIRect Ghost(Frame.x + 3.0f, Frame.y + 3.0f, DragGhostSize, DragGhostSize);
		// 保比例缩放到 56x56
		const float Ratio = (float)DragSlot.m_DstH / (float)std::max(1, DragSlot.m_DstW);
		float DrawW = DragGhostSize;
		float DrawH = DrawW * Ratio;
		if(DrawH > DragGhostSize)
		{
			DrawH = DragGhostSize;
			DrawW = DrawH / std::max(Ratio, 0.001f);
		}
		CUIRect Inner(Ghost.x + (DragGhostSize - DrawW) * 0.5f, Ghost.y + (DragGhostSize - DrawH) * 0.5f, DrawW, DrawH);
		DrawTextureSubset(Graphics(), Inner, s_State.m_DonorTexture, U0, V0, U1, V1);
	}

	// ---- 底部：选中槽位设置 -----------------------------------------------
	CUIRect SettingsRow, TintRow, ExportRow;
	Bottom.HSplitTop(LineSize + 2.0f, &SettingsRow, &Bottom);
	Bottom.HSplitTop(LineSize + 2.0f, &TintRow, &Bottom);
	Bottom.HSplitTop(LineSize + 2.0f, &ExportRow, &Bottom);

	if(s_State.m_SelectedSlot >= 0 && s_State.m_SelectedSlot < (int)vSlots.size())
	{
		CAssetsEditor::SSlot &Slot = Editor.Slots()[s_State.m_SelectedSlot];

		CUIRect SelectedLabel;
		SettingsRow.VSplitLeft(220.0f, &SelectedLabel, &SettingsRow);
		char aSelectedInfo[192];
		str_format(aSelectedInfo, sizeof(aSelectedInfo), "%s: %s  [%d,%d %dx%d]", TCLocalize("Selected"),
			Slot.m_aLabel[0] ? Slot.m_aLabel : "-", Slot.m_DstX, Slot.m_DstY, Slot.m_DstW, Slot.m_DstH);
		Ui()->DoLabel(&SelectedLabel, aSelectedInfo, FontSize, TEXTALIGN_ML);

		// 混合模式
		static std::vector<const char *> s_vBlendNames;
		if(s_vBlendNames.empty())
		{
			for(int i = 0; i < CAssetsEditor::BLEND_COUNT; i++)
				s_vBlendNames.push_back(TCLocalize(s_apBlendNames[i]));
		}
		static CUi::SDropDownState s_BlendDropDown;
		static CScrollRegion s_BlendScrollRegion;
		s_BlendDropDown.m_SelectionPopupContext.m_pScrollRegion = &s_BlendScrollRegion;

		CUIRect BlendLabel, BlendDropDown;
		SettingsRow.VSplitLeft(48.0f, &BlendLabel, &SettingsRow);
		Ui()->DoLabel(&BlendLabel, TCLocalize("Blend:"), FontSize, TEXTALIGN_ML);
		SettingsRow.VSplitLeft(116.0f, &BlendDropDown, &SettingsRow);
		const int NewBlend = Ui()->DoDropDown(&BlendDropDown, std::clamp(Slot.m_BlendMode, 0, (int)CAssetsEditor::BLEND_COUNT - 1),
			s_vBlendNames.data(), (int)s_vBlendNames.size(), s_BlendDropDown);
		if(NewBlend != Slot.m_BlendMode)
			Slot.m_BlendMode = NewBlend;

		SettingsRow.VSplitLeft(8.0f, nullptr, &SettingsRow);
		Ui()->DoScrollbarOption(&Slot.m_BlendStrength, &Slot.m_BlendStrength, &SettingsRow, TCLocalize("Strength"), 0, 100, &CUi::ms_LinearScrollbarScale, 0, "%");

		// 调色
		static CButtonContainer s_TintButton;
		static unsigned TintPacked;
		static int s_TintSlot = -1;
		static int s_TintType = -1;
		if(s_TintSlot != s_State.m_SelectedSlot || s_TintType != Editor.Type() || !Ui()->IsPopupOpen())
		{
			TintPacked = color_cast<ColorHSLA>(Slot.m_Tint).Pack(false);
			s_TintSlot = s_State.m_SelectedSlot;
			s_TintType = Editor.Type();
		}
		CUIRect TintPicker = TintRow;
		TintPicker.VSplitLeft(360.0f, &TintPicker, nullptr);
		DoLine_ColorPicker(&s_TintButton, LineSize, 12.0f, 2.0f, &TintPicker, TCLocalize("Tint"), &TintPacked,
			ColorRGBA(1.0f, 1.0f, 1.0f), false);
		Slot.m_Tint = color_cast<ColorRGBA>(ColorHSLA(TintPacked));

		static CButtonContainer s_ClearTintButton;
		CUIRect ClearTint;
		TintRow.VSplitRight(96.0f, &TintRow, &ClearTint);
		if(DoButton_Menu(&s_ClearTintButton, TCLocalize("Clear tint"), 0, &ClearTint))
			Slot.m_Tint = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);

		static CButtonContainer s_ResetSlotButton;
		CUIRect ResetSlotRect;
		TintRow.VSplitRight(6.0f, &TintRow, nullptr);
		TintRow.VSplitRight(96.0f, &TintRow, &ResetSlotRect);
		if(DoButton_Menu(&s_ResetSlotButton, TCLocalize("Reset slot"), 0, &ResetSlotRect))
		{
			Editor.ResetSlot(s_State.m_SelectedSlot);
			str_copy(s_State.m_aMessage, TCLocalize("Part reset to default."));
			s_State.m_MessageTime = Client()->LocalTime();
		}
	}

	// ---- 导出 -------------------------------------------------------------
	if(!s_State.m_ExportNameInitialized)
	{
		s_State.m_ExportName.Set("myasset");
		s_State.m_ExportNameInitialized = true;
	}
	CUIRect NameLabel, NameBox, ExportButton, MessageRect;
	ExportRow.VSplitLeft(48.0f, &NameLabel, &ExportRow);
	Ui()->DoLabel(&NameLabel, TCLocalize("Name:"), FontSize, TEXTALIGN_ML);
	ExportRow.VSplitLeft(150.0f, &NameBox, &ExportRow);
	Ui()->DoEditBox(&s_State.m_ExportName, &NameBox, EditBoxFontSize);
	ExportRow.VSplitLeft(6.0f, nullptr, &ExportRow);
	ExportRow.VSplitLeft(96.0f, &ExportButton, &ExportRow);
	static CButtonContainer s_ExportButton;
	if(DoButton_Menu(&s_ExportButton, TCLocalize("Export asset"), 0, &ExportButton))
	{
		char aPath[IO_MAX_PATH_LENGTH];
		char aError[128];
		if(Editor.Export(s_State.m_ExportName.GetString(), aPath, sizeof(aPath), aError, sizeof(aError)))
		{
			// 导出成功后自动选中这条新资产。必须走控制台命令而不是直接写配置：
			// ConchainAsset* 回调里的判断是「新值 != 旧值才重载」，先写字符串再
			// 执行命令会因为两值相同而跳过重载，新素材要重启才生效。
			const char *pCmdName = nullptr;
			switch(Editor.Type())
			{
			case CAssetsEditor::TYPE_GAME: pCmdName = "cl_asset_game"; break;
			case CAssetsEditor::TYPE_EMOTICONS: pCmdName = "cl_asset_emoticons"; break;
			case CAssetsEditor::TYPE_PARTICLES: pCmdName = "cl_asset_particles"; break;
			case CAssetsEditor::TYPE_HUD: pCmdName = "cl_asset_hud"; break;
			case CAssetsEditor::TYPE_EXTRAS: pCmdName = "cl_asset_extras"; break;
			default: break;
			}
			if(pCmdName)
			{
				char aCmd[160];
				str_format(aCmd, sizeof(aCmd), "%s \"%s\"", pCmdName, s_State.m_ExportName.GetString());
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			}
			str_format(s_State.m_aMessage, sizeof(s_State.m_aMessage), TCLocalize("Exported to %s"), aPath);
			CancelDrag();
			Editor.ReloadAssets();
		}
		else
			str_format(s_State.m_aMessage, sizeof(s_State.m_aMessage), "%s", aError);
		s_State.m_MessageTime = Client()->LocalTime();
	}

	ExportRow.VSplitLeft(8.0f, nullptr, &ExportRow);
	if(s_State.m_aMessage[0] && Client()->LocalTime() - s_State.m_MessageTime < 8.0f)
	{
		MessageRect = ExportRow;
		Ui()->DoLabel(&MessageRect, s_State.m_aMessage, EditBoxFontSize, TEXTALIGN_ML);
	}
}
