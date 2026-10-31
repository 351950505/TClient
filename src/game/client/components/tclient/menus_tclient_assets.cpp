/* TClient: 素材编辑器 UI（ME 设置页独立标签）。
   左侧选类别 + 槽位列表，中间实时预览画布，右侧是命中槽位的来源/调色设置。 */

#include "assets_editor.h"

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
const float HeadlineFontSize = 20.0f;
const float HeadlineHeight = HeadlineFontSize;
const float Margin = 10.0f;
const float MarginSmall = 5.0f;
const float MarginBetweenSections = 30.0f;
} // namespace

// 类别显示名（TypeName() 是资产目录名，不能本地化；UI 用这张表）。
static const char *s_apTypeDisplayNames[CAssetsEditor::TYPE_COUNT] = {
	"Game", "Emoticons", "Entities", "Skin", "HUD", "Particles", "Cursor", "Arrow", "Strong/Weak", "Extras"};

static const char *s_apBlendNames[CAssetsEditor::BLEND_COUNT] = {
	"Multiply", "Normal", "Screen", "Overlay", "Darken", "Color Burn", "Lighten", "Color Dodge",
	"Soft Light", "Hard Light", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity"};

void CMenus::RenderSettingsTClientAssets(CUIRect MainView)
{
	CAssetsEditor &Editor = GameClient()->m_AssetsEditor;
	Editor.SetGameClient(GameClient());
	Editor.EnsureLoaded();

	CUIRect Left, Center, Right, Row, Label, Button;
	MainView.VSplitLeft(190.0f, &Left, &MainView);
	MainView.VSplitLeft(Margin, nullptr, &MainView);
	MainView.VSplitRight(240.0f, &Center, &Right);
	Center.VSplitRight(Margin, &Center, nullptr);

	// ---- 左侧：类别 + 槽位列表 ---------------------------------------------
	Left.HSplitTop(HeadlineHeight, &Label, &Left);
	Ui()->DoLabel(&Label, TCLocalize("Asset Type"), HeadlineFontSize, TEXTALIGN_ML);
	Left.HSplitTop(MarginSmall, nullptr, &Left);

	static CButtonContainer s_aTypeButtons[CAssetsEditor::TYPE_COUNT];
	for(int Type = 0; Type < CAssetsEditor::TYPE_COUNT; Type++)
	{
		Left.HSplitTop(LineSize, &Button, &Left);
		const bool Active = Editor.Type() == Type;
		if(DoButton_Menu(&s_aTypeButtons[Type], TCLocalize(s_apTypeDisplayNames[Type]), Active, &Button))
		{
			Editor.SetType(Type);
			Editor.EnsureLoaded();
		}
	}

	Left.HSplitTop(MarginSmall, nullptr, &Left);
	Left.HSplitTop(LineSize, &Button, &Left);
	if(DoButton_Menu(&s_aTypeButtons[0], TCLocalize("Reset slots"), 0, &Button))
		Editor.ResetSlots();

	// ---- 中间：预览画布 ----------------------------------------------------
	Center.HSplitTop(HeadlineHeight, &Label, &Center);
	char aCanvasInfo[128];
	str_format(aCanvasInfo, sizeof(aCanvasInfo), TCLocalize("Preview %dx%d"), Editor.CanvasWidth(), Editor.CanvasHeight());
	Ui()->DoLabel(&Label, aCanvasInfo, HeadlineFontSize, TEXTALIGN_ML);
	Center.HSplitTop(MarginSmall, nullptr, &Center);

	// 画布按比例缩放到可用区域，保持像素整数缩放
	const float AvailW = Center.w;
	const float AvailH = std::max(60.0f, Center.h - LineSize * 2.5f);
	const int CanvasW = std::max(1, Editor.CanvasWidth());
	const int CanvasH = std::max(1, Editor.CanvasHeight());
	const float FitScale = std::min(AvailW / (float)CanvasW, AvailH / (float)CanvasH);
	const float Scale = std::max(0.25f, std::floor(FitScale * 4.0f) / 4.0f);
	const float DrawW = CanvasW * Scale;
	const float DrawH = CanvasH * Scale;

	CUIRect Canvas;
	Center.HSplitTop(DrawH, &Canvas, &Center);
	Canvas.w = DrawW;
	Canvas.Draw(ColorRGBA(0.10f, 0.10f, 0.12f, 1.0f), IGraphics::CORNER_ALL, 3.0f);

	// 槽位高亮：鼠标悬停的槽位描边；点击选中
	static int s_SelectedSlot = 0;
	const float MouseX = Ui()->MouseX() - Canvas.x;
	const float MouseY = Ui()->MouseY() - Canvas.y;
	int HoverSlot = -1;
	for(size_t i = 0; i < Editor.Slots().size(); i++)
	{
		const CAssetsEditor::SSlot &Slot = Editor.Slots()[i];
		if(MouseX >= Slot.m_DstX * Scale && MouseX < (Slot.m_DstX + Slot.m_DstW) * Scale &&
			MouseY >= Slot.m_DstY * Scale && MouseY < (Slot.m_DstY + Slot.m_DstH) * Scale)
		{
			HoverSlot = (int)i;
			break;
		}
	}
	if(HoverSlot >= 0 && Ui()->MouseButtonClicked(0))
		s_SelectedSlot = HoverSlot;
	if(s_SelectedSlot >= (int)Editor.Slots().size())
		s_SelectedSlot = 0;
	for(const auto &Slot : Editor.Slots())
	{
		const bool Selected = (int)(&Slot - Editor.Slots().data()) == s_SelectedSlot;
		if(!Selected)
			continue;
		CUIRect R(Canvas.x + Slot.m_DstX * Scale, Canvas.y + Slot.m_DstY * Scale, Slot.m_DstW * Scale, Slot.m_DstH * Scale);
		R.DrawOutline(ColorRGBA(1.0f, 0.85f, 0.2f, 0.9f));
	}
	if(HoverSlot >= 0)
	{
		const CAssetsEditor::SSlot &Slot = Editor.Slots()[HoverSlot];
		CUIRect R(Canvas.x + Slot.m_DstX * Scale, Canvas.y + Slot.m_DstY * Scale, Slot.m_DstW * Scale, Slot.m_DstH * Scale);
		R.DrawOutline(ColorRGBA(1.0f, 1.0f, 1.0f, 0.65f));
	}

	// 合成预览图缓存：只在槽位/类别变化时重合成（合成是纯 CPU 的全画布遍历）
	static int s_LastType = -1;
	static size_t s_LastSlotsHash = 0;
	size_t SlotsHash = Editor.Slots().size() * 1315423911u;
	for(const auto &Slot : Editor.Slots())
	{
		SlotsHash ^= (size_t)Slot.m_DstX * 31 + Slot.m_DstY * 17 + Slot.m_SrcX * 7 + Slot.m_SrcY + Slot.m_BlendMode * 3 + Slot.m_BlendStrength;
		SlotsHash ^= (size_t)(Slot.m_Tint.r * 255) << 1 ^ (size_t)(Slot.m_Tint.g * 255) << 2 ^ (size_t)(Slot.m_Tint.b * 255) << 3;
		SlotsHash ^= (size_t)Slot.m_aSource[0] + (size_t)Slot.m_aSource[1];
	}

	static IGraphics::CTextureHandle s_PreviewTexture;
	static CImageInfo s_PreviewImage;
	if(s_LastType != Editor.Type() || s_LastSlotsHash != SlotsHash)
	{
		s_LastType = Editor.Type();
		s_LastSlotsHash = SlotsHash;
		CImageInfo Composed;
		if(Editor.Compose(Composed))
		{
			if(s_PreviewTexture.IsValid())
				Graphics()->UnloadTexture(&s_PreviewTexture);
			s_PreviewTexture = Graphics()->LoadTextureRawMove(Composed, 0, "assets-editor-preview");
		}
	}
	if(s_PreviewTexture.IsValid())
	{
		Graphics()->TextureSet(s_PreviewTexture);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		Graphics()->QuadsSetSubset(0.0f, 0.0f, 1.0f, 1.0f);
		const IGraphics::CQuadItem Quad(Canvas.x, Canvas.y, DrawW, DrawH);
		Graphics()->QuadsDrawTL(&Quad, 1);
		Graphics()->QuadsEnd();
		Graphics()->TextureClear();
	}

	Center.HSplitTop(MarginSmall, nullptr, &Center);
	Center.HSplitTop(LineSize, &Label, &Center);
	char aSlotInfo[160];
	if(s_SelectedSlot < (int)Editor.Slots().size())
	{
		const CAssetsEditor::SSlot &Slot = Editor.Slots()[s_SelectedSlot];
		str_format(aSlotInfo, sizeof(aSlotInfo), "%s  [%d,%d %dx%d]", Slot.m_aLabel,
			Slot.m_DstX, Slot.m_DstY, Slot.m_DstW, Slot.m_DstH);
	}
	else
		str_copy(aSlotInfo, "-");
	Ui()->DoLabel(&Label, aSlotInfo, FontSize, TEXTALIGN_ML);

	// ---- 右侧：选中槽位的属性 ----------------------------------------------
	Right.HSplitTop(HeadlineHeight, &Label, &Right);
	Ui()->DoLabel(&Label, TCLocalize("Slot Settings"), HeadlineFontSize, TEXTALIGN_ML);
	Right.HSplitTop(MarginSmall, nullptr, &Right);

	if(s_SelectedSlot < (int)Editor.Slots().size())
	{
		CAssetsEditor::SSlot &Slot = Editor.Slots()[s_SelectedSlot];

		// 来源素材下拉
		static std::vector<const char *> s_vAssetNames;
		static CUi::SDropDownState s_AssetDropDown;
		static CScrollRegion s_AssetScrollRegion;
		s_AssetDropDown.m_SelectionPopupContext.m_pScrollRegion = &s_AssetScrollRegion;
		s_vAssetNames.clear();
		for(const auto &Asset : Editor.Assets())
			s_vAssetNames.push_back(Asset.m_aName);
		int SourceIndex = 0;
		for(size_t i = 0; i < Editor.Assets().size(); i++)
		{
			if(str_comp(Editor.Assets()[i].m_aName, Slot.m_aSource) == 0)
			{
				SourceIndex = (int)i;
				break;
			}
		}
		Right.HSplitTop(LineSize, &Row, &Right);
		Row.VSplitLeft(70.0f, &Label, &Row);
		Ui()->DoLabel(&Label, TCLocalize("Source: "), FontSize, TEXTALIGN_ML);
		const int NewSource = Ui()->DoDropDown(&Row, SourceIndex, s_vAssetNames.data(), s_vAssetNames.size(), s_AssetDropDown);
		if(NewSource != SourceIndex && NewSource >= 0 && NewSource < (int)Editor.Assets().size())
			str_copy(Slot.m_aSource, Editor.Assets()[NewSource].m_aName);

		// 混合模式
		static std::vector<const char *> s_vBlendNames;
		static CUi::SDropDownState s_BlendDropDown;
		static CScrollRegion s_BlendScrollRegion;
		s_BlendDropDown.m_SelectionPopupContext.m_pScrollRegion = &s_BlendScrollRegion;
		if(s_vBlendNames.empty())
		{
			for(int i = 0; i < CAssetsEditor::BLEND_COUNT; i++)
				s_vBlendNames.push_back(TCLocalize(s_apBlendNames[i]));
		}
		Right.HSplitTop(LineSize, &Row, &Right);
		Row.VSplitLeft(70.0f, &Label, &Row);
		Ui()->DoLabel(&Label, TCLocalize("Blend: "), FontSize, TEXTALIGN_ML);
		const int NewBlend = Ui()->DoDropDown(&Row, std::clamp(Slot.m_BlendMode, 0, (int)CAssetsEditor::BLEND_COUNT - 1),
			s_vBlendNames.data(), s_vBlendNames.size(), s_BlendDropDown);
		if(NewBlend != Slot.m_BlendMode)
			Slot.m_BlendMode = NewBlend;

		// 强度
		Right.HSplitTop(LineSize, &Row, &Right);
		Ui()->DoScrollbarOption(&Slot.m_BlendStrength, &Slot.m_BlendStrength, &Row, TCLocalize("Strength"), 0, 100, &CUi::ms_LinearScrollbarScale, 0, "%");

		// 调色（取色器直接改 RGBA；内部存 ColorRGBA）
		Right.HSplitTop(LineSize, &Row, &Right);
		static CButtonContainer s_TintButton;
		unsigned TintPacked = color_cast<ColorHSLA>(Slot.m_Tint).Pack();
		DoLine_ColorPicker(&s_TintButton, 22.0f, 12.0f, 4.0f, &Right, TCLocalize("Tint"), &TintPacked,
			ColorRGBA(1.0f, 1.0f, 1.0f), false);
		Slot.m_Tint = color_cast<ColorRGBA>(ColorHSLA(TintPacked));

		Right.HSplitTop(MarginSmall, nullptr, &Right);
		Right.HSplitTop(LineSize, &Button, &Right);
		if(DoButton_Menu(&s_TintButton, TCLocalize("Clear tint"), 0, &Button))
			Slot.m_Tint = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// ---- 导出 -------------------------------------------------------------
	Right.HSplitTop(MarginBetweenSections * 0.5f, nullptr, &Right);
	Right.HSplitTop(HeadlineHeight, &Label, &Right);
	Ui()->DoLabel(&Label, TCLocalize("Export"), HeadlineFontSize, TEXTALIGN_ML);
	Right.HSplitTop(MarginSmall, nullptr, &Right);

	static CLineInputBuffered<64> s_ExportName;
	static bool s_ExportNameInitialized = false;
	if(!s_ExportNameInitialized)
	{
		s_ExportName.Set("myasset");
		s_ExportNameInitialized = true;
	}
	Right.HSplitTop(LineSize, &Row, &Right);
	Row.VSplitLeft(70.0f, &Label, &Row);
	Ui()->DoLabel(&Label, TCLocalize("Name: "), FontSize, TEXTALIGN_ML);
	Ui()->DoEditBox(&s_ExportName, &Row, EditBoxFontSize);

	static char s_aExportMessage[256] = "";
	static float s_ExportMessageTime = -100.0f;
	Right.HSplitTop(MarginSmall, nullptr, &Right);
	Right.HSplitTop(LineSize, &Button, &Right);
	static CButtonContainer s_ExportButton;
	if(DoButton_Menu(&s_ExportButton, TCLocalize("Export asset"), 0, &Button))
	{
		char aPath[IO_MAX_PATH_LENGTH];
		char aError[128];
		if(Editor.Export(s_ExportName.GetString(), aPath, sizeof(aPath), aError, sizeof(aError)))
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
				str_format(aCmd, sizeof(aCmd), "%s \"%s\"", pCmdName, s_ExportName.GetString());
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			}
			str_format(s_aExportMessage, sizeof(s_aExportMessage), TCLocalize("Exported to %s"), aPath);
			Editor.ReloadAssets();
		}
		else
			str_format(s_aExportMessage, sizeof(s_aExportMessage), "%s", aError);
		s_ExportMessageTime = Client()->LocalTime();
	}
	if(s_aExportMessage[0] && Client()->LocalTime() - s_ExportMessageTime < 8.0f)
	{
		Right.HSplitTop(MarginSmall, nullptr, &Right);
		Right.HSplitTop(LineSize * 2.0f, &Label, &Right);
		Ui()->DoLabel(&Label, s_aExportMessage, EditBoxFontSize, TEXTALIGN_TL);
	}
}
