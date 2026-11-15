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
  assert(draw.includes('g_Config.m_TcShowFrozenText == 1 ? Counts.m_KnownUnfrozen : Counts.m_Frozen') && draw.includes('"%d / %d  ? %d"'), 'Text-only mode keeps the plain centered count');
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
  assert(draw.includes('Capsule.y = FrozenHudRowY(Capsule, Island, m_Height);'), 'Island avoidance still runs on the base rect');
  assert(draw.includes('std::clamp(Capsule.x, 8.0f,') && draw.includes('std::clamp(Capsule.y, 8.0f,'), 'Clamp keeps an 8 px screen margin');
  assert(draw.includes('Overflow ? 3 : 1') && draw.includes('TcFrozenHudOverflowPass(State) != OverflowIndex'));
  assert(config.includes('MACRO_CONFIG_INT(TcFrozenTeesInlineIsland, tc_frozen_tees_inline_island, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Place the frozen tee HUD on the same line as the dynamic island, to its right")'), 'inline-island cvar declared');
  assert(config.includes('MACRO_CONFIG_INT(TcFrozenTeesAliveOnly, tc_frozen_tees_alive_only, 0, 0, 1, CFGFLAG_CLIENT | CFGFLAG_SAVE, "Show only alive (unfrozen) teammates in the frozen tee HUD")'), 'alive-only cvar declared');
  assert(config.indexOf('MACRO_CONFIG_INT(TcFrozenTeesInlineIsland') > config.indexOf('MACRO_CONFIG_COL(TcFrozenTeesAccentColor'), 'inline-island cvar follows the accent color entry');
  assert(menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFrozenTeesInlineIsland, TCLocalize("Same line as the dynamic island"), &g_Config.m_TcFrozenTeesInlineIsland, &Column, LineSize);'), 'inline-island checkbox rendered');
  assert(menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFrozenTeesAliveOnly, TCLocalize("Show only alive teammates"), &g_Config.m_TcFrozenTeesAliveOnly, &Column, LineSize);'), 'alive-only checkbox rendered');
  assert(menus.indexOf('TcFrozenTeesInlineIsland') > menus.indexOf('TCLocalize("Only show after joining a team")') && menus.indexOf('TcFrozenTeesInlineIsland') < menus.indexOf('Ui()->DoScrollbarOption(&g_Config.m_TcFrozenMaxRows'), 'new checkboxes sit inside the Frozen Tee Display block');
  for(const [lang, inline, alive] of [['simplified_chinese', '与灵动岛同行', '只显示存活的队友'], ['traditional_chinese', '與靈動島同行', '只顯示存活的隊友']]) {
    const dict = readLang(lang);
    assert(dict.includes('Same line as the dynamic island\n== ' + inline), lang + ' inline-island label translated');
    assert(dict.includes('Show only alive teammates\n== ' + alive), lang + ' alive-only label translated');
  }
  assert(draw.includes('if(g_Config.m_TcFrozenTeesInlineIsland && Island.w > 0 && Island.h > 0 &&') && draw.includes('Capsule.x < Island.x + Island.w && Island.x + Island.w + 2.0f + Capsule.w <= m_Width - 8.0f'), 'inline-island branch guards on fit');
  assert(draw.includes('Capsule.x = Island.x + Island.w + 2.0f;') && draw.includes('Capsule.y = std::max(0.0f, Island.y + (Island.h - Capsule.h) / 2.0f);'), 'inline-island branch parks the capsule to the island right, vertically centered on the island');
  assert(draw.includes('const int MaxTees = MAX_CLIENTS;'), 'tee row grows with the team, no artificial per-row cap');
  assert(draw.indexOf('Capsule.x = Island.x + Island.w + 2.0f;') < draw.indexOf('Capsule.y = FrozenHudRowY(Capsule, Island, m_Height);'), 'inline-island placement decided before the avoidance fallback');
  assert(draw.includes('const bool AliveOnly = g_Config.m_TcFrozenTeesAliveOnly != 0;') && draw.includes('const int NumDrawn = AliveOnly ? m_FrozenHudFrame.m_Counts.m_KnownUnfrozen : NumInTeam;'), 'alive-only count defined from the captured frame');
  assert(draw.includes('if(CapsuleShown && NumDrawn > 0)') && draw.includes('int TotalRows = std::min(MaxRows, (NumDrawn + MaxTees - 1) / MaxTees);') && draw.includes('const int TeesInRow = std::min(NumDrawn, MaxTees);') && draw.includes('bool Overflow = NumDrawn > MaxTees * MaxRows;'), 'NumDrawn drives the capsule layout');
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

  const helper = between(hud, 'float CHud::FrozenHudRowY(', 'void CHud::RenderFrozenHud(');
  const countSizing = between(draw, 'const float CountFontSize = TeeSize * 0.7f', 'const float CountMainW =');
  const scaleBlock = between(draw, 'const float Scale = std::clamp(g_Config.m_TcFrozenTeesScale / 100.0f', 'const ColorRGBA AccentColor');
  const sizeBlock = between(draw, 'float TeeSize = g_Config.m_TcFrozenHudTeeSize * Scale;', 'int MaxTees =');
  const startPosBlock = between(draw, 'float StartPos =', 'int TotalRows =');
  const contentBlock = between(draw, 'const float ContentW =', 'char aCountBuf');
  const capsuleBlock = between(draw, 'const float CapsuleW =', 'CUIRect TeeRow =');
  const dotBlock = between(draw, 'const float DotR =', 'const ColorRGBA DotColor =');
  const metrics = between(island, 'const float UiScale =', 'const int PillCorners =');
  const x = between(island, 'const float X = std::clamp((ScreenW - Width)', '// ---- draw the capsule body');
  const lineOf = (source, start) => {
    const first = source.indexOf(start);
    assert(first >= 0, `Missing production seam: ${start}`);
    return source.slice(first, source.indexOf('\n', first)).trim();
  };
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
struct Config { int m_TcIslandSize, m_TcIslandNotch, m_TcIslandOffsetX, m_TcIslandOffsetY; int m_TcFontSize = 100; int m_TcFrozenTeesScale = 100, m_TcFrozenTeesOffsetX = 0, m_TcFrozenTeesOffsetY = 0, m_TcFrozenHudTeeSize = 15, m_TcFrozenTeesInlineIsland = 0; } g_Config;
struct Graphics { float m_Aspect; float ScreenAspect() const { return m_Aspect; } };
class CHud {
 public:
	static float FrozenHudRowY(const CUIRect &, const CUIRect &, float);
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
 return Capsule;
}
// Status dot placement model, again production expressions verbatim.
vec2 CHud::FrozenDot(bool ShowFrozenText, float CountW, int TeesInRow, int TotalRows, int Row, int TeeInRow, const CUIRect &Island, float m_Width, float m_Height) {
 ${scaleBlock}
 ${sizeBlock}
 ${startPosBlock}
 ${contentBlock}
 ${capsuleBlock}
 const CUIRect TeeRow{Capsule.x + Pad + (ShowFrozenText ? CountW + TeeGap : 0.0f), Capsule.y + Pad, ContentW, ContentH};
 vec2 TeeRenderPos(TeeRow.x + TeeSize / 2.0f + TeeInRow * (TeeSize + TeeGap), TeeRow.y + TeeSize * (0.7f) + Row * (TeeSize + TeeGap));
 const int CurrentRow = Row;
 ${dotBlock}
 return vec2(DotX, DotY);
}
// Alive-only count model: production NumDrawn/row/overflow expressions verbatim.
void AliveLayout(bool AliveOnly, int NumInTeam, int KnownUnfrozen, bool CapsuleShown, int MaxTees, int MaxRows, int *pTotalRows, int *pTeesInRow, bool *pOverflow, bool *pVisible) {
 struct { struct { int m_KnownUnfrozen; } m_Counts; } m_FrozenHudFrame;
 m_FrozenHudFrame.m_Counts.m_KnownUnfrozen = KnownUnfrozen;
 ${numDrawnLine}
 ${rowsBlock}
 ${overflowLine}
 *pTotalRows = TotalRows;
 *pTeesInRow = TeesInRow;
 *pOverflow = Overflow;
 *pVisible = CapsuleShown && ${capsuleGuard};
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
 // the 8 px screen-margin clamp, reusing the production expressions verbatim.
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
  Check(Capsule.x>=8.0f-0.001f && Capsule.y>=8.0f-0.001f, "capsule violates the 8 px screen margin");
  Check(Capsule.x+Capsule.w<=std::max(8.0f,W-8.0f)+0.001f, "capsule crosses the right margin");
  Check(Capsule.y+Capsule.h<=std::max(8.0f,300.0f-8.0f)+0.001f, "capsule crosses the bottom margin");
  const float BaseX=W/2.0f+38.0f*(W/300.0f)/1.78f-TeeSize/2.0f-Pad-(Text?42.0f+TeeGap:0.0f);
  Check(std::fabs(Capsule.x-std::clamp(BaseX+(float)OffX*Scale, 8.0f, std::max(8.0f, W-8.0f-Capsule.w)))<0.001f, "offset applied to the base position then clamped");
  Check(std::fabs(Capsule.y-std::clamp(-Pad+(float)OffY*Scale, 8.0f, std::max(8.0f, 300.0f-8.0f-Capsule.h)))<0.001f, "vertical offset applied then clamped");
  ++CapsuleCases;
 }
 // Default placement still dodges the island; an explicit offset wins over it.
 g_Config.m_TcFrozenTeesScale=100; g_Config.m_TcFrozenTeesOffsetX=0; g_Config.m_TcFrozenHudTeeSize=15; g_Config.m_TcFontSize=100;
 for(int OffY : {-600,-15,0,15}) {
  g_Config.m_TcFrozenTeesOffsetY=OffY;
  const CUIRect Island{200,0,200,24};
  const CUIRect Capsule=CHud::FrozenCapsule(true, 42.0f, 6, 1, Island, 300.0f*1.777778f, 300.0f);
  Check(OffY!=0 || !Overlaps(Capsule,Island), "default placement must still dodge the island");
  Check(OffY>=0 || Overlaps(Capsule,Island), "explicit negative offset wins over island avoidance");
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
  const bool Inline=IslandW>0 && BaseX<200.0f+IslandW && 200.0f+IslandW+2.0f+CapsuleW<=W-8.0f;
  if(Inline) {
   Check(std::fabs(Capsule.x-std::clamp(200.0f+IslandW+2.0f+(float)OffX*Scale, 8.0f, std::max(8.0f, W-8.0f-CapsuleW)))<0.001f, "inline island parks right of the island, then offset and clamp");
   Check(std::fabs(Capsule.y-std::clamp(std::max(0.0f,10.0f+(24.0f-Capsule.h)/2.0f)+(float)OffY*Scale, 8.0f, std::max(8.0f, 300.0f-8.0f-Capsule.h)))<0.001f, "inline island centers on the island line, then offset and clamp");
  } else {
   Check(std::fabs(Capsule.x-std::clamp(BaseX+(float)OffX*Scale, 8.0f, std::max(8.0f, W-8.0f-CapsuleW)))<0.001f, "inline fallback keeps the base x path");
   Check(std::fabs(Capsule.y-std::clamp(CHud::FrozenHudRowY(CUIRect{BaseX,-Pad,CapsuleW,Capsule.h},Island,300.0f)+(float)OffY*Scale, 8.0f, std::max(8.0f, 300.0f-8.0f-Capsule.h)))<0.001f, "inline fallback keeps the island avoidance path");
  }
  ++InlineCases;
 }
 // Alive-only counts: production NumDrawn/row/overflow expressions verbatim.
 int AliveCases=0;
 int AliveTotalRows, AliveTeesInRow; bool AliveOverflow, AliveVisible;
 AliveLayout(true, 8, 3, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==3 && !AliveOverflow && AliveVisible, "alive-only layout sizes to the unfrozen count");
 ++AliveCases;
 AliveLayout(false, 8, 3, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==8 && !AliveOverflow && AliveVisible, "option off keeps the whole-team layout");
 ++AliveCases;
 AliveLayout(true, 8, 0, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==0 && AliveTeesInRow==0 && !AliveOverflow && !AliveVisible, "empty alive-only capsule is skipped");
 ++AliveCases;
 AliveLayout(true, 20, 15, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==8 && AliveOverflow && AliveVisible, "alive-only overflow still driven by the unfrozen count");
 ++AliveCases;
 AliveLayout(true, 20, 5, true, 8, 1, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==1 && AliveTeesInRow==5 && !AliveOverflow, "alive-only can clear an otherwise overflowing team");
 ++AliveCases;
 AliveLayout(true, 30, 20, true, 8, 2, &AliveTotalRows, &AliveTeesInRow, &AliveOverflow, &AliveVisible);
 Check(AliveTotalRows==2 && AliveTeesInRow==8 && AliveOverflow && AliveVisible, "alive-only respects MaxRows");
 ++AliveCases;
 std::printf("PASS: %d resolution/size/style/offset layout cases plus hidden, side, bottom, stretched and screen fallback checks\\n", Cases);
 std::printf("PASS: %d capsule offset/scale/aspect layout cases with padding math, margin clamp and island interaction\\n", CapsuleCases);
 std::printf("PASS: %d modeled count-font scale/alignment and status-dot placement cases including 200-percent font (not GPU ink)\\n", MarkerCases);
 std::printf("PASS: %d inline-island placement cases across island widths, offsets and scales\\n", InlineCases);
 std::printf("PASS: %d alive-only count/layout cases including empty-capsule skip and overflow\\n", AliveCases);
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
