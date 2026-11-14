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
  assert(draw.includes('TeeRow.y + TeeSize * (0.7f) + CurrentRow * TeeSize'));
  assert(draw.includes('TextRender()->SetRenderFlags(OldTextFlags);') && draw.includes('Graphics()->MapScreen(OldScreen);'));
  assert(draw.includes('TextRender()->TextColor(OldTextColor);') && draw.includes('TextRender()->TextOutlineColor(OldTextOutlineColor);'));
  assert(draw.includes('m_FrozenHudFrame.m_Counts') && draw.includes('m_FrozenHudFrame.m_aIncluded[i]') && draw.includes('m_FrozenHudFrame.m_aStates[i]'), 'Count/tees must use the captured pending frame');
  assert(!draw.includes('m_FreezeEnd') && !draw.includes('m_DeepFrozen') && !draw.includes('m_Teams.Team(') && !draw.includes('m_apPlayerInfos'), 'Delayed render must not reclassify state or membership');
  assert(draw.includes('g_Config.m_TcShowFrozenText == 1 ? Counts.m_KnownUnfrozen : Counts.m_Frozen') && draw.includes('"%d / %d  ? %d"'));
  assert(draw.includes('if(Frozen && !g_Config.m_TcShowFrozenHudSkins)') && draw.includes('TeeInfo = FreezeInfo;'));
  assert(draw.includes('if(Unknown)') && draw.includes('TeeRenderPos, 0.45f)') && draw.includes('FontSize, "?"'));
  assert(draw.includes('TeeSize * 0.55f / std::max(1.0f, g_Config.m_TcFontSize / 100.0f)'), 'Unknown marker must cap global-font-scaled size to its Tee slot');
  assert(draw.includes('TeeRenderPos.y - TeeSize * 0.45f, FontSize, "?"'), 'Marker placement must stay within the original Tee row');
  assert(draw.includes('Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.4f);') && draw.includes('EMOTE_PAIN, vec2(1.0f, 0.0f), TeeRenderPos, Alpha'), 'Existing background/frozen tee style must remain');
  assert(draw.includes('Overflow ? 3 : 1') && draw.includes('TcFrozenHudOverflowPass(State) != OverflowIndex'));
  assert(island.indexOf('if(Idle && !ShowGameTime && !ShowLocalClock && !ShowLink)') < island.indexOf('OccupiedRect = {Left'));
  const render = between(island, 'void CIslandMusic::OnRender()', 'void CIslandMusic::OnShutdown()');
  assert(render.includes('CUIRect OccupiedRect = {};') && render.indexOf('m_pImpl->Render(OccupiedRect);') < render.indexOf('m_Hud.RenderFrozenHud(OccupiedRect);'));
  assert(render.indexOf('#endif') < render.indexOf('m_Hud.RenderFrozenHud(OccupiedRect);'), 'Unsupported platforms must finish pending HUD too');

  const helper = between(hud, 'float CHud::FrozenHudRowY(', 'void CHud::RenderFrozenHud(');
  const markerSizing = between(draw, 'const float FontSize = TeeSize * 0.55f', 'const float MarkWidth =');
  const metrics = between(island, 'const float UiScale =', 'const int PillCorners =');
  const x = between(island, 'const float X = std::clamp((ScreenW - Width)', '// ---- draw the capsule body');
  fs.writeFileSync(path.join(out, 'test.cpp'), `
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <game/client/ui_rect.h>
class CHud { public: static float FrozenHudRowY(const CUIRect &, const CUIRect &, float); };
${helper}
struct Config { int m_TcIslandSize, m_TcIslandNotch, m_TcIslandOffsetX, m_TcIslandOffsetY; int m_TcFontSize = 100; } g_Config;
struct Graphics { float m_Aspect; float ScreenAspect() const { return m_Aspect; } };
CUIRect Bounds(int RealW, int RealH, float Width, float Aspect) {
 Graphics G{Aspect}; const auto *m_pGraphics = &G; CUIRect OccupiedRect{};
 ${metrics}
 ${x}
 return OccupiedRect;
}
// Numerical model of production font scaling/pixel alignment, not GPU ink.
float MarkerAlignedFontSize(float TeeSize, float RealHeight) {
 ${markerSizing}
 const float FakeToScreenY = RealHeight / 300.0f;
 return std::trunc(FontSize * (g_Config.m_TcFontSize / 100.0f) * FakeToScreenY) / FakeToScreenY;
}
void Check(bool Value, const char *Message) { if(!Value) throw std::runtime_error(Message); }
bool Overlaps(CUIRect A, CUIRect B) { return A.w > 0 && A.h > 0 && B.w > 0 && B.h > 0 && A.x < B.x+B.w && A.x+A.w > B.x && A.y < B.y+B.h && A.y+A.h > B.y; }
int main() {
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
 // Regression: at 200% an unbounded tee20 marker had baseline/line bottom
 // 26.94, outside the original row bottom 23, so an island at 24.61 slipped
 // through the row-only overlap test. Capped marker keeps the original box.
 g_Config.m_TcFontSize=200;
 const float PixelScale1080=1080.0f/300.0f;
 const float OldMarkerBottom=std::round(5.0f*PixelScale1080)/PixelScale1080+std::trunc(20.0f*0.55f*2.0f*PixelScale1080)/PixelScale1080;
 const CUIRect BelowIsland{298,24.61f,130,30};
 const CUIRect OriginalRow{298,0,20,23};
 Check(OldMarkerBottom>BelowIsland.y && !Overlaps(OriginalRow,BelowIsland), "missing historical 200-percent below-island regression");
 Check(CHud::FrozenHudRowY(OriginalRow,BelowIsland,300)==0, "fix must not enlarge/move known Tee row");
 int MarkerCases=0;
 for(int RealHeight : {480,720,1080,1440,2160}) for(int Percent : {50,100,150,200})
 for(float TeeSize : {8.0f,13.0f,20.0f,30.0f}) for(int Row : {0,1,2}) {
  g_Config.m_TcFontSize=Percent;
  const float PixelScale=RealHeight/300.0f;
  const float AlignedSize=MarkerAlignedFontSize(TeeSize,(float)RealHeight);
  const float MarkY=std::round((Row*TeeSize+TeeSize*(0.7f-0.45f))*PixelScale)/PixelScale;
  const float MarkBottom=MarkY+AlignedSize;
  const CUIRect TeeBounds{298,0,TeeSize,TeeSize*(Row+1)+3};
  const CUIRect IslandBelow{298,TeeBounds.h+1.61f,TeeSize,30};
  Check(AlignedSize<=TeeSize*0.55f+0.001f, "global font exceeds marker size cap");
  Check(MarkY>=Row*TeeSize && MarkBottom<=(Row+1)*TeeSize, "aligned marker baseline/line box leaves Tee slot");
  Check(!Overlaps({298,MarkY,TeeSize,AlignedSize},IslandBelow), "capped marker overlaps below-row island");
  Check(CHud::FrozenHudRowY(TeeBounds,IslandBelow,300)==0, "capped marker changes original box placement");
  ++MarkerCases;
 }
 std::printf("PASS: %d resolution/size/style/offset layout cases plus hidden, side, bottom, stretched and screen fallback checks\\n", Cases);
 std::printf("PASS: %d modeled marker scale/alignment/row cases including 200-percent font and below-row island (not GPU ink)\\n", MarkerCases);
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
  console.log('PASS: production fresh-snapshot classification, pending states/counts, visibility, same-frame handoff, team/scoreboard gates, notification and draw-state wiring');
  console.log('NOTE: pure helper/layout and static wiring regression only; not an in-game or server/demo integration test.');
} catch(error) {
  console.error(error.message);
  process.exitCode=1;
} finally {
  fs.rmSync(out,{recursive:true,force:true});
}
