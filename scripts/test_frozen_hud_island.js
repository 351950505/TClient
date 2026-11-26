// node scripts/test_frozen_hud_island.js
// Standalone regression: compile the actual frozen-state tests, layout helper and pixel-to-HUD bounds.
// No CMake changes, build tree writes, media session or GPU needed.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const hud = fs.readFileSync(path.join(root, 'src/game/client/components/hud.cpp'), 'utf8').replace(/\r\n/g, '\n');
const island = fs.readFileSync(path.join(root, 'src/game/client/components/tclient/island_music.cpp'), 'utf8').replace(/\r\n/g, '\n');
const config = fs.readFileSync(path.join(root, 'src/engine/shared/config_variables_tclient.h'), 'utf8').replace(/\r\n/g, '\n');
const menus = fs.readFileSync(path.join(root, 'src/game/client/components/tclient/menus_tclient.cpp'), 'utf8').replace(/\r\n/g, '\n');
const readLang = name => fs.readFileSync(path.join(root, 'data/tclient/languages', name + '.txt'), 'utf8').replace(/\r\n/g, '\n');
function between(source, start, end) {
  const first = source.indexOf(start);
  const last = source.indexOf(end, first + start.length);
  assert(first >= 0 && last > first, `Missing production seam: ${start}`);
  return source.slice(first, last);
}
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'frozen-hud-island-'));
function run(executable, args, env = process.env) {
  const result = cp.spawnSync(executable, args, { cwd: out, env, encoding: 'utf8', timeout: 120000, maxBuffer: 1024 * 1024 });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, `${executable}: ${result.stdout || ''}\n${result.stderr || ''}`);
  return result.stdout || '';
}
try {
  assert(!hud.includes('IslandSuppressed'), 'Island must not suppress frozen controls');
  assert(hud.includes('const bool ShowFrozenText = g_Config.m_TcShowFrozenText > 0;'));
  assert(hud.includes('const bool ShowFrozenHud = g_Config.m_TcShowFrozenHud > 0;'));
  assert(hud.includes('ShowFrozenHud && !GameClient()->m_Scoreboard.IsActive() && !(LocalTeamID == 0 && g_Config.m_TcFrozenHudTeamOnly)'));
  const info = between(hud, 'void CHud::RenderTextInfo()', 'float CHud::FrozenHudRowY(');
  assert(info.includes('g_Config.m_TcNotifyWhenLastX / 100.0f') && info.includes('g_Config.m_TcNotifyWhenLastY / 100.0f'));
  assert(info.indexOf('// Notify when last') < info.indexOf('m_FrozenHudPending = true;'));
  const state = between(info, '// render team in freeze text and last notify', '// Notify when last');
  assert(state.includes('SnapFindItem(IClient::SNAP_CURRENT, NETOBJTYPE_CHARACTER, i) != nullptr'), 'Presence must come from fresh current Character, including first frame in view');
  assert(state.includes('TcFrozenHudState(HasFreshCharacter, Character.m_HasExtendedData, Character.m_ExtendedData.m_FreezeEnd)'));
  assert(!state.includes('m_aClients') && !state.includes('Character.m_Active'), 'Freeze caches and two-frame render activity cannot classify HUD state');
  assert(state.includes('TcFrozenHudTeamClientId(GameClient()->m_Snap.m_LocalClientId,') && state.includes('TeamClientId >= 0 ? GameClient()->m_Teams.Team(TeamClientId) : 0'), 'Independent range-checked local/target team selection');
  assert(state.includes('!GameClient()->m_Snap.m_apPlayerInfos[i] || GameClient()->m_Teams.Team(i) != LocalTeamID'), 'Preserve filtered PlayerInfo team list');
  assert(!state.includes('TEAM_SPECTATORS') && !state.includes('m_LiveFrozen') && !state.includes('CHARACTERFLAG_IN_FREEZE'));
  assert(info.includes('Counts.NotifyWhenLast()') && info.includes('m_FrozenHudFrame = Frame;'));
  assert(!info.includes('NumInTeam - NumFrozen'), 'Unknown cannot be derived as unfrozen');
  assert(hud.includes('void CHud::OnRender()\n{\n\tm_FrozenHudPending = false;'));
  const draw = between(hud, 'void CHud::RenderFrozenHud(', 'void CHud::RenderConnectionWarning()');
  assert(draw.includes('m_FrozenHudPending = false;'));
  assert(draw.includes('TeeRow.y + TeeSize * (0.7f) + CurrentRow * (TeeSize + TeeGap)'), 'Row anchor keeps the 0.7 body offset; rows gain the scaled tee gap');
  assert(draw.includes('TextRender()->SetRenderFlags(OldTextFlags);') && draw.includes('Graphics()->MapScreen(OldScreen);'));
  assert(draw.includes('TextRender()->TextColor(OldTextColor);') && draw.includes('TextRender()->TextOutlineColor(OldTextOutlineColor);'));
  assert(draw.includes('m_FrozenHudFrame.m_Counts') && draw.includes('m_FrozenHudFrame.m_aIncluded[i]') && draw.includes('m_FrozenHudFrame.m_aStates[i]'), 'Count/tees must use the captured pending frame');
  assert(!draw.includes('m_FreezeEnd') && !draw.includes('m_DeepFrozen') && !draw.includes('m_Teams.Team(') && !draw.includes('m_apPlayerInfos'), 'Delayed render must not reclassify state or membership');
  assert(draw.includes('str_format(aCountBuf, sizeof(aCountBuf), "%d / %d", NumShown, NumInTeam);') && draw.includes('TextRender()->Text(CountX, TextY, CountFontSize, aCountBuf);'), 'Frozen HUD count text follows the configured frozen/unfrozen mode');
  assert(draw.includes('if(Frozen && !g_Config.m_TcShowFrozenHudSkins)') && draw.includes('TeeInfo = FreezeInfo;'));
  assert(draw.includes('if(Unknown)') && draw.includes('TeeRenderPos, 0.45f)'), 'Unknown keeps the dimmed own skin');
  assert(!draw.includes('FontSize, "?"') && !draw.includes('*= 0.4f'), 'Per-tee "?" overlay and x0.4 darkening are retired');
  assert(draw.includes('const float CountFontSize = TeeSize * 0.7f / std::max(1.0f, g_Config.m_TcFontSize / 100.0f)'), 'Capsule count size derives from TeeSize*0.7 and cancels global font scaling');
  assert(draw.includes('TeeRow.y + CurrentRow * (TeeSize + TeeGap) + TeeSize - DotR'), 'Status dot sits in the tee cell bottom-right corner');
  assert(draw.includes('if(Frozen || Unknown)') && draw.includes('Graphics()->DrawCircle(DotX, DotY, DotR, 16)'), 'Two-layer status dots only for frozen/unknown tees');
  assert(draw.includes('ColorRGBA(DotColor.r, DotColor.g, DotColor.b, 0.22f)') && draw.includes('ColorRGBA(DotColor.r, DotColor.g, DotColor.b, 0.55f)'), 'Status dot layers use the 0.22/0.55 alphas');
  assert(draw.includes('Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.30f);') && draw.includes('IGraphics::CORNER_ALL'), 'Capsule plate is a translucent rounded capsule');
  assert(draw.includes('Graphics()->DrawRectExt4(') && draw.includes('ColorRGBA(1.0f, 1.0f, 1.0f, 0.05f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.05f)') && draw.includes('ColorRGBA(1.0f, 1.0f, 1.0f, 0.0f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.0f)'), 'Top sheen fades white 0.05 to transparent');
  assert(draw.includes('EMOTE_PAIN, vec2(1.0f, 0.0f), TeeRenderPos, Alpha'), 'Frozen tee emote unchanged');
  assert(draw.includes('Alpha = 0.85f;') && draw.includes('TeeInfo.m_ColorBody.Multiply(AccentColor)') && draw.includes('TeeInfo.m_ColorFeet.Multiply(AccentColor)'), 'Skins-mode frozen tint multiplies toward the accent color');
  assert(draw.includes('g_Config.m_TcFrozenTeesScale / 100.0f') && draw.includes('g_Config.m_TcFrozenTeesOffsetX * Scale') && draw.includes('g_Config.m_TcFrozenTeesOffsetY * Scale'), 'Offset/scale cvars drive capsule placement');
  assert(draw.includes('FrozenHudCapsulePlacement(Capsule, Island, m_Width, m_Height, OffsetX, OffsetY)'), 'production uses the tested right-side and vertical-follow placement');
  assert(hud.includes('Capsule.y = Island.y;') && hud.includes('std::clamp(Capsule.y, 0.0f,'), 'island vertical offset follows with on-screen clamping');
  assert(draw.includes('Overflow ? 3 : 1') && draw.includes('TcFrozenHudOverflowPass(State) != OverflowIndex'));
  assert(config.includes('MACRO_CONFIG_INT(TcFrozenHudTeeSize, tc_frozen_tees_size, 10, 8, 20,'), 'tee size defaults to 10');
  assert(config.includes('MACRO_CONFIG_INT(TcFrozenTeesInlineIsland, tc_frozen_tees_inline_island, 1, 0, 1,'), 'placement to the right of the island is enabled by default');
  assert(config.includes('MACRO_CONFIG_INT(TcFrozenTeesAliveOnly, tc_frozen_tees_alive_only, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show only alive (unfrozen) teammates in the frozen tee HUD")'), 'alive-only cvar declared');
  assert(config.indexOf('MACRO_CONFIG_INT(TcFrozenTeesInlineIsland') > config.indexOf('MACRO_CONFIG_COL(TcFrozenTeesAccentColor'), 'inline-island cvar follows the accent color entry');
  assert(!menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFrozenTeesInlineIsland'), 'right-side anchor no longer depends on the retired checkbox');
  assert(!draw.includes('g_Config.m_TcFrozenTeesInlineIsland'), 'legacy saved inline setting cannot disable right-side anchoring');
  assert(menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFrozenTeesAliveOnly, TCLocalize("Show only alive teammates"), &g_Config.m_TcFrozenTeesAliveOnly, &Column, LineSize);'), 'alive-only checkbox rendered');
  assert(menus.indexOf('TcFrozenTeesAliveOnly') > menus.indexOf('TCLocalize("Only show after joining a team")') && menus.indexOf('TcFrozenTeesAliveOnly') < menus.indexOf('Ui()->DoScrollbarOption(&g_Config.m_TcFrozenMaxRows'), 'alive-only checkbox remains inside the Frozen Tee Display block');
  for(const [lang, inline, alive] of [['simplified_chinese', '与灵动岛同行', '只显示存活的队友'], ['traditional_chinese', '與靈動島同行', '只顯示存活的隊友']]) {
    const dict = readLang(lang);
    assert(dict.includes('Same line as the dynamic island\n== ' + inline), lang + ' inline-island label translated');
    assert(dict.includes('Show only alive teammates\n== ' + alive), lang + ' alive-only label translated');
  }
  assert(hud.includes('Capsule.x = Island.x + Island.w + 2.0f;') && hud.includes('Capsule.y = Island.y;'), 'capsule anchors to the current island right and y');
  assert(draw.includes('Graphics()->MapScreenToSize(m_Width / CapsuleScale, m_Height / CapsuleScale);') && draw.includes('Capsule.x = PlacedCapsule.x / CapsuleScale;'), 'fitting scales contents without moving the right-side anchor');
  assert(config.includes('MACRO_CONFIG_INT(TcFrozenTeesMaxDisplay, tc_frozen_tees_max_display, 7, 1, 10,'), 'tee display defaults to 7 and accepts only 1 through 10');
  assert(menus.includes('TCLocalize("Max displayed"), 1, 10)'), 'menu exposes only the bounded display count');
  assert(!menus.includes('Max displayed (0 = all)'), 'unlimited display option is retired');
  assert(draw.includes('const int MaxDisplayLimit = std::clamp(g_Config.m_TcFrozenTeesMaxDisplay, 1, 10);') && draw.includes('const int MaxDisplayed = std::min(NumDrawn, MaxDisplayLimit);'), 'rendering enforces the icon limit independently of row count');
  assert(draw.includes('i < MAX_CLIENTS && NumDisplayed < MaxDisplayed;') && !draw.includes('NumDisplayed < MaxTees * MaxRows'), 'additional rows cannot multiply the total icon limit');
  assert(!draw.includes('Island.x + Island.w + 2.0f + Capsule.w <='), 'narrow right-hand space must not send the capsule back to the old position');
  assert(draw.includes('const bool AliveOnly = g_Config.m_TcFrozenTeesAliveOnly != 0;') && draw.includes('const int NumDrawn = AliveOnly ? m_FrozenHudFrame.m_Counts.m_KnownUnfrozen : NumInTeam;'), 'alive-only count defined from the captured frame');
  assert(draw.includes('if(CapsuleShown && (NumDrawn > 0 || ShowFrozenText))') && draw.includes('int TotalRows = std::min(MaxRows, (MaxDisplayed + MaxTees - 1) / MaxTees);') && draw.includes('const int TeesInRow = std::min(MaxDisplayed, MaxTees);') && draw.includes('bool Overflow = NumDrawn > MaxDisplayed;'), 'capped icon count drives the layout while the full count drives overflow priority');
  {
    const classifyIdx = draw.indexOf('const bool Unknown = State == ETcFrozenHudState::UNKNOWN;');
    const aliveSkipIdx = draw.indexOf('if(AliveOnly && State != ETcFrozenHudState::UNFROZEN)');
    const overflowPassIdx = draw.indexOf('TcFrozenHudOverflowPass(State) != OverflowIndex');
    assert(aliveSkipIdx > classifyIdx && aliveSkipIdx < overflowPassIdx, 'alive-only skip sits between state classification and the overflow pass');
  }
  assert(draw.includes('"%d / %d", NumShown, NumInTeam)'), 'count text still reports the whole team');
  assert(island.indexOf('if(Idle && !ShowGameTime && !ShowLocalClock && !ShowLink)') < island.indexOf('OccupiedRect = {Left'));
  const render = between(island, 'void CIslandMusic::OnRender()', 'void CIslandMusic::OnShutdown()');
  assert(render.includes('CUIRect OccupiedRect = {};') && render.indexOf('m_pImpl->Render(OccupiedRect);') < render.indexOf('m_Hud.RenderFrozenHud(OccupiedRect);'));
  assert(render.indexOf('#endif') < render.indexOf('m_Hud.RenderFrozenHud(OccupiedRect);'), 'Unsupported platforms must finish pending HUD too');

  const helper = between(hud, 'float CHud::FrozenHudRowY(', 'namespace\n{');
  const countSizing = between(draw, 'const float CountFontSize = TeeSize * 0.7f', 'const float CountMainW =');
  const scaleBlock = between(draw, 'const float Scale = std::clamp(g_Config.m_TcFrozenTeesScale / 100.0f', 'const ColorRGBA AccentColor');
  const sizeBlock = between(draw, 'float TeeSize = g_Config.m_TcFrozenHudTeeSize * Scale;', '// The display limit');
  const startPosBlock = between(draw, 'float StartPos =', 'int TotalRows =');
  const contentBlock = between(draw, 'const bool HasTees =', 'char aCountBuf');
  const capsuleBlock = between(draw, 'const float CountTeeGap =', 'if(CapsuleScale <= 0.0f)');
  const dotBlock = between(draw, 'const float DotR =', 'const ColorRGBA DotColor =');
  const metrics = between(island, 'const float UiScale =', 'const int PillCorners =');
  const x = between(island, 'const float X = std::clamp((ScreenW - Width)', '// ---- draw the capsule body');
  const lineOf = (source, start) => {
    const first = source.indexOf(start);
    assert(first >= 0, `Missing production seam: ${start}`);
    return source.slice(first, source.indexOf('\n', first)).trim();
  };
  const displayLimitBlock = between(draw, 'const int MaxDisplayLimit =', 'float StartPos =');
  const rowsBlock = between(draw, 'int TotalRows =', 'const float ContentW =');
  const numDrawnLine = lineOf(draw, 'const int NumDrawn =');
  const overflowLine = lineOf(draw, 'bool Overflow =');
  const capsuleGuard = between(draw, 'if(CapsuleShown && ', ')\n\t{').slice('if(CapsuleShown && '.length);
  fs.writeFileSync(path.join(out, 'test.cpp'), `
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <game/client/ui_rect.h>
struct Config { int m_TcIslandSize, m_TcIslandNotch, m_TcIslandOffsetX, m_TcIslandOffsetY; int m_TcFontSize = 100; int m_TcFrozenTeesScale = 100, m_TcFrozenTeesOffsetX = 0, m_TcFrozenTeesOffsetY = 0, m_TcFrozenHudTeeSize = 10, m_TcFrozenTeesInlineIsland = 1, m_TcFrozenTeesMaxDisplay = 7, m_TcFrozenMaxRows = 1; } g_Config;
struct Graphics { float m_Aspect; float ScreenAspect() const { return m_Aspect; } };
class CHud {
 public:
	static float FrozenHudRowY(const CUIRect &, const CUIRect &, float);
	static CUIRect FrozenHudCapsulePlacement(CUIRect, const CUIRect &, float, float, float, float);
	static CUIRect FrozenCapsule(bool ShowFrozenText, float CountW, int TeesInRow, int TotalRows, const CUIRect &Island, float m_Width, float m_Height);
	static vec2 FrozenDot(bool ShowFrozenText, float CountW, int TeesInRow, int TotalRows, int Row, int TeeInRow, const CUIRect &Island, float m_Width, float m_Height);
};
${helper}
// Capsule geometry model: production expressions verbatim.
CUIRect CHud::FrozenCapsule(bool ShowFrozenText, float CountW, int TeesInRow, int TotalRows, const CUIRect &Island, float m_Width, float m_Height) {
 ${scaleBlock}
 ${sizeBlock}
 ${startPosBlock}
 ${contentBlock}
 ${capsuleBlock}
 return PlacedCapsule;
}
// Status dot placement model, again production expressions verbatim.
vec2 CHud::FrozenDot(bool ShowFrozenText, float CountW, int TeesInRow, int TotalRows, int Row, int TeeInRow, const CUIRect &Island, float m_Width, float m_Height) {
 ${scaleBlock}
 ${sizeBlock}
 ${startPosBlock}
 ${contentBlock}
 ${capsuleBlock}
 if(CapsuleScale<=0.0f) return {};
 Capsule.x=PlacedCapsule.x/CapsuleScale;
 Capsule.y=PlacedCapsule.y/CapsuleScale;
 const CUIRect TeeRow{Capsule.x + Pad + (ShowFrozenText ? CountW + CountTeeGap : 0.0f), Capsule.y + Pad, ContentW, ContentH};
 vec2 TeeRenderPos(TeeRow.x + TeeSize / 2.0f + TeeInRow * (TeeSize + TeeGap), TeeRow.y + TeeSize * (0.7f) + Row * (TeeSize + TeeGap));
 const int CurrentRow = Row;
 ${dotBlock}
 return vec2(DotX * CapsuleScale, DotY * CapsuleScale);
}
// Alive-only count model: production NumDrawn/row/overflow expressions verbatim.
void AliveLayout(bool AliveOnly, int NumInTeam, int KnownUnfrozen, bool CapsuleShown, bool ShowFrozenText, int DisplayLimit, int RowLimit, int *pTotalRows, int *pTeesInRow, bool *pOverflow, bool *pVisible, int *pDisplayed = nullptr) {
 struct { struct { int m_KnownUnfrozen; } m_Counts; } m_FrozenHudFrame;
 m_FrozenHudFrame.m_Counts.m_KnownUnfrozen = KnownUnfrozen;
 g_Config.m_TcFrozenTeesMaxDisplay = DisplayLimit;
 g_Config.m_TcFrozenMaxRows = RowLimit;
 ${numDrawnLine}
 ${displayLimitBlock}
 ${rowsBlock}
 ${overflowLine}
 *pTotalRows = TotalRows;
 *pTeesInRow = TeesInRow;
 *pOverflow = Overflow;
 *pVisible = CapsuleShown && ${capsuleGuard};
 if(pDisplayed) *pDisplayed = MaxDisplayed;
}
CUIRect Bounds(int RealW, int RealH, float Width, float Aspect) {
 Graphics G{Aspect}; const auto *m_pGraphics = &G; CUIRect OccupiedRect{};
 ${metrics}
 ${x}
 return OccupiedRect;
}
// Numerical model of production font scaling/pixel alignment, not GPU ink.
float CountAlignedFontSize(float TeeSize, float RealHeight) {
 ${countSizing}
 const float FakeToScreenY = RealHeight / 300.0f;
 return std::trunc(CountFontSize * (g_Config.m_TcFontSize / 100.0f) * FakeToScreenY) / FakeToScreenY;
}
void Check(bool Value, const char *Message) { if(!Value) throw std::runtime_error(Message); }
bool Overlaps(CUIRect A, CUIRect B) { return A.w > 0 && A.h > 0 && B.w > 0 && B.h > 0 && A.x < B.x+B.w && A.x+A.w > B.x && A.y < B.y+B.h && A.y+A.h > B.y; }
int main() {
try {
 const CUIRect Count{245,12,42,10}, Tees{298,0,130,29};
 Check(CHud::FrozenHudRowY(Count, {}, 300)==12, "hidden/disabled/unsupported island moves count");
 Check(CHud::FrozenHudRowY(Tees, {}, 300)==0, "hidden island moves tees");
 Check(CHud::FrozenHudRowY(Count, {0,0,100,50}, 300)==12, "sideways island moves count");
 Check(CHud::FrozenHudRowY(Tees, {200,220,200,40}, 300)==0, "bottom island moves tees");
 Check(CHud::FrozenHudRowY(Count, {245,22,42,20}, 300)==12, "touching edges count as overlap");
 Check(CHud::FrozenHudRowY({245,270,42,20}, {200,260,200,40}, 300)==238, "bottom overlap lacks above fallback");
 Check(CHud::FrozenHudRowY({245,0,42,20}, {200,0,200,300}, 300)==280, "no-space fallback leaves screen");
 Check(CHud::FrozenHudRowY({245,0,42,400}, {200,0,200,300}, 300)==0, "oversized fallback underflows");
 int Cases=0;
 for(int H : {480,720,1080,1440,2160}) for(float Aspect : {1.25f,1.333333f,1.777778f,2.333333f,3.555556f})
 for(int Size : {40,100,200}) for(int Notch : {0,1}) for(int OffsetY : {-600,0,15,600}) {
  const int W=(int)(H*Aspect); g_Config={Size,Notch,0,OffsetY};
  const CUIRect Island=Bounds(W,H,W*0.6f,Aspect);
  Check(Island.x>=0 && Island.y>=0 && Island.x+Island.w<=300*Aspect+0.001f && Island.y+Island.h<=300.001f, "bounds outside HUD");
  for(CUIRect Row : {CUIRect{150*Aspect-21,12,42,10}, CUIRect{150*Aspect+30,0,130,29}}) {
   const float Original=Row.y; const bool Hit=Overlaps(Row,Island);
   Row.y=CHud::FrozenHudRowY(Row,Island,300);
   Check(Row.y>=0 && Row.y+Row.h<=300, "row outside screen");
   Check(Hit || Row.y==Original, "non-intersecting island moves row");
   Check(!Overlaps(Row,Island), "row still intersects island"); ++Cases;
  }
 }
 // Conversion remains correct under stretched HUD aspect, independently per axis.
 g_Config={100,0,0,0}; const CUIRect Stretched=Bounds(1920,1080,600,1.333333f);
 Check(std::fabs(Stretched.w-602.0f*400.0f/1920.0f)<0.001f, "stretched x conversion");
 Check(std::fabs(Stretched.h-74.0f*300.0f/1080.0f)<0.001f, "pixel y conversion");
 g_Config={100,0,-600,0}; const CUIRect Side=Bounds(1920,1080,180,1.777778f);
 Check(CHud::FrozenHudRowY(Count,Side,300)==12, "sideways idle capsule moves count");
 // Capsule layout model: offset/scale/aspect combinations, padding math and
 // the horizontal/bottom margins and top-edge clamp, using production expressions.
 int CapsuleCases=0;
 for(float Aspect : {1.25f,1.333333f,1.777778f,2.333333f,3.555556f})
 for(int ScalePct : {60,100,150,200}) for(int OffX : {-600,-15,0,15,600}) for(int OffY : {-600,0,15,600})
 for(bool Text : {false,true}) for(int TeesInRow : {1,6}) {
  const float W=300.0f*Aspect;
  g_Config.m_TcFrozenTeesScale=ScalePct; g_Config.m_TcFrozenTeesOffsetX=OffX; g_Config.m_TcFrozenTeesOffsetY=OffY;
  g_Config.m_TcFrozenHudTeeSize=15; g_Config.m_TcFontSize=100;
  const CUIRect Capsule=CHud::FrozenCapsule(Text, 42.0f, TeesInRow, 1, {}, W, 300.0f);
  const float Scale=std::clamp(ScalePct/100.0f,0.6f,2.0f);
  const float TeeSize=15.0f*Scale, TeeGap=3.0f*Scale, Pad=2.0f*Scale;
  const float ContentW=TeesInRow*TeeSize+(TeesInRow-1)*TeeGap;
  const float ContentH=TeeSize+3.0f*Scale;
  Check(std::fabs(Capsule.w-(2.0f*Pad+(Text?42.0f+TeeGap:0.0f)+ContentW))<0.001f, "capsule width wraps padding, count text and tee grid");
  Check(std::fabs(Capsule.h-(2.0f*Pad+ContentH))<0.001f, "capsule height wraps padding and feet margin");
  Check(Capsule.x>=8.0f-0.001f && Capsule.y>=0.0f, "capsule violates the horizontal margin or top edge");
  Check(Capsule.x+Capsule.w<=std::max(8.0f,W-8.0f)+0.001f, "capsule crosses the right margin");
  Check(Capsule.y+Capsule.h<=std::max(8.0f,300.0f-8.0f)+0.001f, "capsule crosses the bottom margin");
  const float BaseX=W/2.0f+38.0f*(W/300.0f)/1.78f-TeeSize/2.0f-Pad-(Text?42.0f+TeeGap:0.0f);
  Check(std::fabs(Capsule.x-std::clamp(BaseX+(float)OffX*Scale, 8.0f, std::max(8.0f, W-8.0f-Capsule.w)))<0.001f, "offset applied to the base position then clamped");
  Check(std::fabs(Capsule.y-std::clamp((float)OffY*Scale, 0.0f, std::max(0.0f, 300.0f-8.0f-Capsule.h)))<0.001f, "vertical offset applied while allowing a flush top edge");
  ++CapsuleCases;
 }
 // Legacy disabled inline setting cannot restore the overlapping old anchor.
 g_Config.m_TcFrozenTeesScale=100; g_Config.m_TcFrozenTeesOffsetX=0; g_Config.m_TcFrozenHudTeeSize=15; g_Config.m_TcFontSize=100; g_Config.m_TcFrozenTeesInlineIsland=0;
 for(int OffY : {-600,-15,0,15}) {
  g_Config.m_TcFrozenTeesOffsetY=OffY;
  const CUIRect Island{200,40,200,24};
  const CUIRect Capsule=CHud::FrozenCapsule(true,42.0f,6,1,Island,300.0f*1.777778f,300.0f);
  Check(std::fabs(Capsule.x-402.0f)<0.001f && !Overlaps(Capsule,Island),"legacy inline off cannot disable the right anchor");
  Check(std::fabs(Capsule.y-std::max(0.0f,40.0f+(float)OffY))<0.001f,"tee vertical offset is relative to the island");
 }
 // Status dot placement and count-font fit, modeled on production expressions.
 int MarkerCases=0;
 for(int RealHeight : {480,720,1080,1440,2160}) for(int Percent : {50,100,150,200})
 for(float TeeSize : {8.0f,13.0f,20.0f,30.0f}) for(int Row : {0,1,2}) {
  g_Config.m_TcFontSize=Percent;
  g_Config.m_TcFrozenTeesScale=100; g_Config.m_TcFrozenTeesOffsetX=0; g_Config.m_TcFrozenTeesOffsetY=0;
  g_Config.m_TcFrozenHudTeeSize=(int)TeeSize;
  const float AlignedSize=CountAlignedFontSize(TeeSize,(float)RealHeight);
  const CUIRect Capsule=CHud::FrozenCapsule(true, 42.0f, 4, Row+1, {}, 300.0f*1.777778f, 300.0f);
  const vec2 Dot=CHud::FrozenDot(true, 42.0f, 4, Row+1, Row, 1, {}, 300.0f*1.777778f, 300.0f);
  const float DotR=std::max(1.0f,TeeSize*0.16f);
  const float Pad=2.0f, TeeGap=3.0f;
  const float AnchorX=Capsule.x+Pad+42.0f+TeeGap+TeeSize/2.0f+1*(TeeSize+TeeGap);
  const float RowTop=Capsule.y+Pad+Row*(TeeSize+TeeGap);
  Check(AlignedSize<=TeeSize*0.7f+0.001f, "global font exceeds count size cap");
  Check(1.4f*AlignedSize<=Capsule.h, "count line box leaves the capsule height");
  Check(std::fabs(Dot.x-(AnchorX+TeeSize/2.0f-DotR))<0.001f, "dot hugs the tee cell right edge");
  Check(std::fabs(Dot.y-(RowTop+TeeSize-DotR))<0.001f, "dot hugs the tee cell bottom edge");
  Check(Dot.x-DotR>=Capsule.x+0.001f && Dot.y-DotR>=Capsule.y+0.001f, "dot leaves the capsule interior");
  Check(Dot.y+DotR<=Capsule.y+Capsule.h-3.0f+0.001f, "dot stays above the capsule feet margin");
  Check(CHud::FrozenHudRowY(CUIRect{Capsule.x,Capsule.y,Capsule.w,Capsule.h},{},300.0f)==Capsule.y, "island-free placement untouched");
  ++MarkerCases;
 }
 // Inline-island placement: the production capsule expressions already embed
 // the inline branch, re-driven here across island widths, offsets and scales.
 int InlineCases=0;
 g_Config.m_TcFontSize=100; g_Config.m_TcFrozenHudTeeSize=15; g_Config.m_TcFrozenTeesOffsetX=0; g_Config.m_TcFrozenTeesOffsetY=0; g_Config.m_TcFrozenTeesInlineIsland=1;
 for(float IslandW : {0.0f, 80.0f, 200.0f, 380.0f}) for(int ScalePct : {60,100,200})
 for(int OffX : {-600,-15,0,15,600}) for(int OffY : {-600,0,15,600}) {
  const float W=300.0f*1.777778f;
  const CUIRect Island{200.0f,10.0f,IslandW,24.0f};
  g_Config.m_TcFrozenTeesScale=ScalePct; g_Config.m_TcFrozenTeesOffsetX=OffX; g_Config.m_TcFrozenTeesOffsetY=OffY;
  const CUIRect Capsule=CHud::FrozenCapsule(true, 42.0f, 6, 1, Island, W, 300.0f);
  const float Scale=std::clamp(ScalePct/100.0f,0.6f,2.0f);
  const float TeeSize=15.0f*Scale, TeeGap=3.0f*Scale, Pad=2.0f*Scale;
  const float ContentW=6*TeeSize+5*TeeGap;
  const float CapsuleW=2.0f*Pad+(42.0f+TeeGap)+ContentW;
  const float BaseX=W/2.0f+38.0f*(W/300.0f)/1.78f-TeeSize/2.0f-Pad-(42.0f+TeeGap);
  if(IslandW>0) {
   const float AnchorX=std::clamp(200.0f+IslandW+2.0f+(float)OffX*Scale,8.0f,std::max(8.0f,W-8.0f));
   const float Fit=std::min(1.0f,std::max(0.0f,W-8.0f-AnchorX)/CapsuleW);
   if(Fit<=0.0f) Check(Capsule.w==0.0f && Capsule.h==0.0f,"no space must not cause an overlapping fallback");
   else {
    Check(std::fabs(Capsule.x-AnchorX)<0.001f,"island right anchor survives fitting");
    Check(std::fabs(Capsule.w-CapsuleW*Fit)<0.001f,"narrow space scales the whole capsule");
    Check(std::fabs(Capsule.y-std::clamp(10.0f+(float)OffY*Scale,0.0f,std::max(0.0f,300.0f-8.0f-Capsule.h)))<0.001f,"island vertical offset follows before explicit tee offset");
   }
  } else {
   Check(std::fabs(Capsule.x-std::clamp(BaseX+(float)OffX*Scale,8.0f,std::max(8.0f,W-8.0f-CapsuleW)))<0.001f,"hidden island keeps the standalone base x");
   Check(std::fabs(Capsule.y-std::clamp((float)OffY*Scale,0.0f,std::max(0.0f,300.0f-8.0f-Capsule.h)))<0.001f,"hidden island keeps the standalone base y");
  }
  ++InlineCases;
 }
 // Default capsule: top-flush and anchored to the island right, even when
 // its previous centered x position did not overlap the island.
 int TopCases=0;
 g_Config={100,0,0,0};
 Check(g_Config.m_TcFrozenHudTeeSize==10 && g_Config.m_TcFrozenTeesInlineIsland==1, "default size/placement regression");
 for(float Aspect : {1.25f,1.333333f,1.777778f,2.333333f,3.555556f})
 for(float IslandY : {0.0f,10.0f,80.0f}) for(int Tees : {1,7,10}) {
  const float W=300.0f*Aspect;
  const CUIRect Island{W*0.2f,IslandY,80.0f,24.0f};
  const CUIRect Capsule=CHud::FrozenCapsule(true,20.0f,Tees,1,Island,W,300.0f);
  Check(std::fabs(Capsule.x-(Island.x+Island.w+2.0f))<0.001f, "default capsule must start to the right of the island");
  Check(Capsule.y==IslandY, "default capsule follows the island y, including vertical offset");
  Check(!Overlaps(Capsule,Island), "default capsule overlaps the island");
  Check(Capsule.x+Capsule.w<=W-8.0f+0.001f, "default capsule exceeds right screen boundary");
  ++TopCases;
 }
 for(int OffY : {-600,0,1,15}) {
  g_Config.m_TcFrozenTeesOffsetY=OffY;
  const CUIRect Capsule=CHud::FrozenCapsule(false,0.0f,7,1,{},533.3333f,300.0f);
  Check(Capsule.y==std::max(0,OffY), "top edge remains reachable with or without the island");
  ++TopCases;
 }
 std::printf("PASS: %d default-right/vertical-follow placement cases with size 10 and up to 10 icons\\n", TopCases);
 // Alive-only counts: production NumDrawn/row/overflow expressions verbatim.
 int AliveCases=0;
 int AliveTotalRows, AliveTeesInRow; bool AliveOverflow, AliveVisible;
 AliveLayout(true, 8, 3, true, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==3 && !AliveOverflow && AliveVisible, "alive-only layout sizes to the unfrozen count");
 ++AliveCases;
 AliveLayout(false, 8, 3, true, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==8 && !AliveOverflow && AliveVisible, "option off keeps the whole-team layout");
 ++AliveCases;
 AliveLayout(true, 8, 0, true, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==0 && AliveTeesInRow==0 && !AliveOverflow && AliveVisible, "empty alive-only capsule keeps a count-only capsule");
 ++AliveCases;
 AliveLayout(true, 8, 0, true, false, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==0 && AliveTeesInRow==0 && !AliveOverflow && !AliveVisible, "empty capsule is skipped when the count text is off");
 ++AliveCases;
 AliveLayout(true, 20, 15, true, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==8 && AliveOverflow && AliveVisible, "alive-only overflow still driven by the unfrozen count");
 ++AliveCases;
 AliveLayout(true, 20, 5, true, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==5 && !AliveOverflow, "alive-only can clear an otherwise overflowing team");
 ++AliveCases;
 AliveLayout(true, 30, 20, true, true, 8, 2, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==2 && AliveTeesInRow==4 && AliveOverflow && AliveVisible, "multiple rows share the total icon limit");
 ++AliveCases;
 int LimitCases=0;
 for(int Team=0; Team<=64; ++Team) for(int Alive=0; Alive<=Team; ++Alive)
 for(bool AliveOnly : {false,true}) for(int Limit : {-2147483647,-1,0,1,6,7,10,11,64,2147483647})
 for(int Rows : {-1,0,1,2,6,7}) {
  int Displayed=0;
  AliveLayout(AliveOnly, Team, Alive, true, true, Limit, Rows, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible, &Displayed);
  const int Available=AliveOnly?Alive:Team;
  const int Expected=std::min(Available,std::clamp(Limit,1,10));
  Check(Displayed==Expected && Displayed<=10, "total icon count exceeds the configured/global limit");
  Check(AliveTotalRows>=0 && AliveTotalRows<=std::clamp(Rows,1,6), "row count exceeds its bound");
  Check(AliveTeesInRow>=0 && AliveTotalRows*AliveTeesInRow>=Displayed, "layout cannot fit the capped icons");
  Check(AliveOverflow==(Available>Expected), "overflow selection must still prioritize the whole team");
  Check(AliveVisible, "count text keeps the capsule visible when the capped set is empty");
  ++LimitCases;
 }
 AliveLayout(false, 64, 32, true, true, 7, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==7 && AliveOverflow, "default settings must show seven tees");
 std::printf("PASS: %d total-display-limit cases, including legacy values and multi-row/64-player teams\\n", LimitCases);
 std::printf("PASS: %d resolution/size/style/offset layout cases plus hidden, side, bottom, stretched and screen fallback checks\\n", Cases);
 std::printf("PASS: %d capsule offset/scale/aspect layout cases with padding math, margin clamp and island interaction\\n", CapsuleCases);
 std::printf("PASS: %d modeled count-font scale/alignment and status-dot placement cases including 200-percent font (not GPU ink)\\n", MarkerCases);
 std::printf("PASS: %d inline-island placement cases across island widths, offsets and scales\\n", InlineCases);
 std::printf("PASS: %d alive-only count/layout cases including count-only empty capsules and overflow\\n", AliveCases);
} catch(const std::exception &e) { std::fprintf(stderr, "FAIL: %s\\n", e.what()); return 1; }
}
`);
  let compile;
  if (process.platform === 'win32') {
    const programFiles = process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)';
    const vswhere = path.join(programFiles, 'Microsoft Visual Studio/Installer/vswhere.exe');
    const installation = cp.spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' });
    assert(installation.status === 0 && installation.stdout.trim(), 'MSVC Build Tools not found');
    const tools = path.join(installation.stdout.trim(), 'VC/Tools/MSVC');
    const msvc = path.join(tools, fs.readdirSync(tools).sort((a,b) => a.localeCompare(b, undefined, { numeric:true })).pop());
    const kits = path.join(programFiles, 'Windows Kits/10');
    const version = fs.readdirSync(path.join(kits, 'Include')).filter(v => /^10\./.test(v)).sort((a,b) => a.localeCompare(b, undefined, { numeric:true })).pop();
    const env = { ...process.env, VSLANG: '1033', INCLUDE: [path.join(msvc,'include'), ...['ucrt','shared','um'].map(n => path.join(kits,'Include',version,n))].join(';'), LIB: [path.join(msvc,'lib/x64'), ...['ucrt','um'].map(n => path.join(kits,'Lib',version,n,'x64'))].join(';') };
    compile = (source, binary, standalone) => run(path.join(msvc,'bin/Hostx64/x64/cl.exe'), ['/nologo','/std:c++20','/EHsc','/W4','/utf-8',`/I${path.join(root,'src')}`,...(standalone ? ['/DTC_FROZEN_HUD_STANDALONE'] : []),source,`/Fo${path.join(out,path.basename(binary)+'.obj')}`,`/Fe${binary}`],env);
  } else {
    compile = (source, binary, standalone) => run(process.env.CXX || 'c++', ['-std=c++20','-Wall','-Wextra','-I',path.join(root,'src'),...(standalone ? ['-DTC_FROZEN_HUD_STANDALONE'] : []),source,'-o',binary]);
  }
  const stateBinary = path.join(out, process.platform === 'win32' ? 'frozen-state.exe' : 'frozen-state');
  compile(path.join(root, 'src/test/tclient_frozen_hud_test.cpp'), stateBinary, true);
  process.stdout.write(run(stateBinary, []));
  const binary = path.join(out, process.platform === 'win32' ? 'test.exe' : 'test');
  compile(path.join(out, 'test.cpp'), binary, false);
  process.stdout.write(run(binary, []));
  console.log('PASS: production fresh-snapshot classification, pending states/counts, visibility, same-frame handoff, team/scoreboard gates, notification, draw-state wiring, inline-island placement and alive-only counts');
  console.log('NOTE: pure helper/layout and static wiring regression only; not an in-game or server/demo integration test.');
} catch(error) {
  console.error(error.message);
  process.exitCode=1;
} finally {
  fs.rmSync(out,{recursive:true,force:true});
}
