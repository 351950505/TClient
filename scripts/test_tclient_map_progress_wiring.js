// node scripts/test_tclient_map_progress_wiring.js
// Source-seam regression for disabled-by-default map-progress integration.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const read = rel => fs.readFileSync(path.join(root, rel), 'utf8').replace(/\r\n/g, '\n');

function functionBody(source, signature) {
  const start = source.indexOf(signature);
  assert(start >= 0, `Missing production seam: ${signature}`);
  const open = source.indexOf('{', start + signature.length);
  assert(open >= 0, `Missing body: ${signature}`);
  let depth = 0;
  for(let i = open; i < source.length; i++) {
    if(source[i] === '{') depth++;
    if(source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  assert.fail(`Unterminated body: ${signature}`);
}

const config = read('src/engine/shared/config_variables_tclient.h');
assert.match(config, /MACRO_CONFIG_INT\(TcMapProgress\s*,\s*tc_map_progress\s*,\s*0\s*,\s*0\s*,\s*1/,
  'Map progress must remain disabled for new and existing users by default');
assert.match(config, /MACRO_CONFIG_INT\(TcMapProgressStyle\s*,\s*tc_map_progress_style\s*,\s*0\s*,\s*0\s*,\s*1/);
assert.match(config, /tc_map_progress_style[^\n]*instead of at the bottom right/i);
assert.match(config, /MACRO_CONFIG_INT\(TcMapProgressFollowAccent,\s*tc_map_progress_follow_accent,\s*0,\s*0,\s*1,\s*CFGFLAG_CLIENT \| CFGFLAG_SAVE,/,
  'Standalone HUD should default to white instead of following the accent');
const hudMapProgress = read('src/game/client/components/tclient/hud_map_progress.h');
assert(hudMapProgress.includes('ShouldShowStandaloneValue') && hudMapProgress.includes('ShouldReserveIslandValue') && hudMapProgress.includes('DYNAMIC_ISLAND_AVAILABLE'), 'HUD and Island destinations must be exclusive and account for Island availability');
assert.match(hudMapProgress, /MAP_PROGRESS_HUD_MAX_WIDTH = 25\.0f/);
assert.match(hudMapProgress, /MAP_PROGRESS_HUD_FONT_SIZE = 4\.5f/);
assert.match(hudMapProgress, /MOVEMENT_INFORMATION_TEXT_INSET = 2\.0f/);
assert(hudMapProgress.includes('StandaloneHudValueX'), 'Standalone value must share the movement panel right inset');
const mapProgressMenus = read('src/game/client/components/tclient/menus_tclient.cpp');
assert(mapProgressMenus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcMapProgressFollowAccent, TCLocalize("Follow the interface accent color")'),
  'Accent color preference should be an explicit checkbox');
const mapProgressHudCpp = read('src/game/client/components/hud.cpp');
const standaloneHud = functionBody(mapProgressHudCpp, 'void CHud::RenderMapProgressHud(float MovementInformationTop)');
assert.match(standaloneHud, /const ColorRGBA ProgressColor = g_Config\.m_TcMapProgressFollowAccent \? TClientModernUi::Accent\(X, Top\) : ColorRGBA\(1\.0f, 1.0f, 1.0f, 1.0f\)/,
  'Standalone label and value should default to white and optionally use the interface accent');
assert.match(standaloneHud, /m_MapProgressHudLabel\.Render\(TextRender\(\), vec2\(LabelX, LabelY\), ProgressColor\)/);
assert.match(standaloneHud, /m_MapProgressHudText\.Render\(TextRender\(\), vec2\(X, Top\), ProgressColor\)/);
const mapProgressSimplifiedChinese = read('data/tclient/languages/simplified_chinese.txt');
assert(mapProgressSimplifiedChinese.includes('\nFollow the interface accent color\n== '), 'Accent checkbox should have a Simplified Chinese label');

const mapProgress = read('src/game/client/components/tclient/map_progress.h');
const client = read('src/game/client/components/tclient/tclient.cpp');
const onUpdate = functionBody(client, 'void CTClient::OnUpdate()');
assert(onUpdate.includes('UpdateMapProgress(false)'), 'Incremental map/distance-field work must advance on regular updates');
assert(!onUpdate.includes('ResetMapProgress()'), 'Do not discard an enabled map scan every frame');
const snapshot = functionBody(client, 'void CTClient::OnNewSnapshot()');
assert(snapshot.includes('UpdateMapProgress(true)'), 'Snapshot observations must update the current route estimate');
const update = functionBody(client, 'void CTClient::UpdateMapProgress(bool NewSnapshot)');
assert.match(update, /if\(!IsMapProgressEnabled\(\)\)[\s\S]*?ResetMapProgress\(\);[\s\S]*?return;/, 'Do not scan or solve outside Gores');
assert.doesNotMatch(update, /NUT_race9|m_PredictDDRace/, 'No non-Gores exception');
assert(update.includes('TClientMapProgress::MAP_PROGRESS_SCAN_BUDGET') && update.includes('TClientMapProgress::MAP_PROGRESS_DIJKSTRA_BUDGET'), 'Map scan and route solving must have bounded budgets');
assert.match(update, /if\(!NewSnapshot\)[\s\S]*?int ScanBudget = TClientMapProgress::MAP_PROGRESS_SCAN_BUDGET[\s\S]*?ScanBudget -= End - Start/, 'the full scan budget must be spent only from the regular update path');
assert.match(update, /if\(NewSnapshot\)[\s\S]*?m_aMapProgressPlayers\[Slot\]\.Observe[\s\S]*?return;[\s\S]*?if\(!m_MapProgressMap\.Ready\(\)\)/, 'snapshot callbacks must update observations and return before distance-field advancement');
assert.match(update, /ImageScanned < ImageBudget[\s\S]*?m_MapProgressWaterImageScanCursor/, 'water image-name metadata must be loaded incrementally');
assert.match(update, /ShareMapProgressBudget\(DijkstraBudget, ActiveSlots\)/, 'all active player routes must share one per-frame search budget');
assert(update.includes('m_MapProgressMapCrc != CurrentMapCrc') && update.includes('m_pMapProgressCollision != pCollision'), 'Map/geometry identity changes must invalidate stale routes');
assert(update.includes('m_MapProgressFailedForMap') && update.includes('m_MapProgressFailedMapCrc == CurrentMapCrc'), 'A failed map identity must stay disabled without retrying allocations each frame');
assert(update.includes('TClientMapProgress::IsMapSizeWithinProgressBudget') && update.includes('TClientMapProgress::IsMapProgressVisualMetadataWithinBudget'), 'Tile and visual metadata budgets must fail closed before scanning');
assert.match(update, /catch\(const std::bad_alloc &\)[\s\S]*?FailMapProgressForMap\(pFailureMap, pFailureCollision, FailureMapCrc\)/, 'Allocation failures must latch the current map as unavailable');
assert.doesNotMatch(update, /UnloadData\(/, 'Map-progress scans borrow datafile blocks and must not release shared indices');
const failMap = functionBody(client, 'void CTClient::FailMapProgressForMap(');
assert(failMap.includes('ResetMapProgress()') && failMap.includes('m_MapProgressFailedForMap = true'), 'Fail-closed must clear partial route state then latch map identity');
const visualScan = functionBody(client, 'void CTClient::ScanMapProgressVisualLayers(');
assert(visualScan.includes('pMap->GetDataSize(pTilemap->m_Data) < RequiredBytes') && visualScan.includes('if(!pVisualTiles)'), 'Malformed or unavailable borrowed visual tile data must fail closed');
assert.match(update, /ScanMapProgressVisualLayers\(ScanBudget\)[\s\S]*?if\(m_MapProgressFailedForMap\)[\s\S]*?return;/, 'The shared frame budget must stop after a visual scan latches failure');
assert(/if\(!pCurrentMap \|\| !pCurrentMap->IsLoaded\(\)\)[\s\S]*?ResetMapProgress\(\);[\s\S]*?return;[\s\S]*?pCurrentMap->BaseName\(\)/.test(update), 'Never query the map name while its datafile is closed');
const reset = functionBody(client, 'void CTClient::ResetMapProgress()');
assert.doesNotMatch(reset, /BaseName\(|Crc\(|UnloadData\(/, 'Reset must discard cached state without reading or releasing shared map data');
assert(reset.includes('m_MapProgressFailedForMap = false'), 'A map identity change clears the failed-map latch');
const estimate = functionBody(client, 'TClientMapProgress::SEstimate CTClient::MapProgressEstimate(int Conn) const');
assert(/if\(!pCurrentMap \|\| !pCurrentMap->IsLoaded\(\)\)[\s\S]*?return \{\};[\s\S]*?pCurrentMap->BaseName\(\)/.test(estimate), 'HUD queries must tolerate the map-unload window');
assert(update.includes('TClientMapProgress::MakeTile(') && update.includes('m_aMapProgressPlayers[Slot].Observe(') && update.includes('m_aMapProgressPlayers[Slot].Update('), 'Production sampling must feed the tested Qm-derived algorithm');
assert(update.includes('Slot <= MapProgressFreeviewSlot()') && update.includes('GameClient()->m_Camera.m_Center'), 'Freeview must sample the camera center in a separate progress slot');
assert(update.includes('PositionToIndex(Position)') && update.includes('ObserveView(ReadTile(Index), Index)'), 'Freeview position must be resampled and bounded on regular updates');
assert(update.includes('MapProgressRaceTimeStarted(Slot)') &&
	update.includes('Observe(Tile, ObserveAt, RaceTimeStarted)'), 'Tee progress uses the synchronized race-timer state');
const raceTimer = functionBody(client, 'bool CTClient::MapProgressRaceTimeStarted(int Slot) const');
assert(raceTimer.includes('GAMESTATEFLAG_RACETIME') && raceTimer.includes('m_pGameInfoObj'), 'Use the server-synchronized race-timer state');
assert(raceTimer.includes('SpectatorId < 0 || SpectatorId >= MAX_CLIENTS') && raceTimer.includes('MapProgressFreeviewSlot()'), 'Followed Tee timer and freeview must resolve to separate slots');
const resolveClient = functionBody(client, 'int CTClient::ResolveMapProgressClientId(int Slot) const');
assert(resolveClient.includes('m_SpecInfo.m_Active)') && resolveClient.includes('return -1;'), 'A local spectator must not reuse the followed Tee timer for its own slot');
assert(update.includes('pCharacter->m_Cur.m_X') && update.includes('m_SpecCharPresent') && !update.includes('m_pSpectatorInfo'), 'Followed Tee progress uses target snapshots only and stays unknown when target position is missing');
const progressModel = read('src/game/client/components/tclient/map_progress.h');
assert.match(progressModel, /bool Ready\(\) const \{ return m_Finalized && !m_vFinishes\.empty\(\); \}/);
assert(progressModel.includes('void Observe(const STile &Tile, int Index, bool RaceTimeStarted)'));
assert(progressModel.includes('void ObserveView(const STile &Tile, int Index)'));
assert.match(progressModel, /inline bool RaceTimeStarted\(bool HasGameInfo, bool RaceTimeFlag\)/);

const hud = read('src/game/client/components/hud.cpp');
const onHudUpdate = functionBody(hud, 'void CHud::OnUpdate()');
assert(onHudUpdate.includes('UpdateMapProgress()'), 'HUD smoothing must update from the cached production estimate outside render');
const textInfo = functionBody(hud, 'void CHud::RenderTextInfo()');
assert(textInfo.includes('MiniDebugCoordinateOffsetY'), 'MiniDebug must retain its own independent coordinate offsets');
assert.doesNotMatch(textInfo, /TcMapProgress|MapProgressHud/, 'Progress must not render at the MiniDebug anchor');
assert.doesNotMatch(hud, /RenderMapProgressBelowBoard|RenderMapProgressStandalone/, 'The progress must not jump between board and fallback positions');
const movement = functionBody(hud, 'void CHud::RenderMovementInformation(');
assert(movement.includes('RenderMapProgressHud(StartY)') && movement.includes('RightHudTop'), 'The independent readout and stack must share the actual coordinates top, including dummy rows');
const progressHud = functionBody(hud, 'void CHud::RenderMapProgressHud(');
assert(movement.includes('IsMapProgressEnabled()'), 'A non-Gores server must not reserve a HUD row');
assert.doesNotMatch(progressHud, /DrawRect|QuadsBegin|MapProgressEstimate|TextWidth\(/, 'Transparent readout must not draw a panel, solve routes or measure glyphs every frame');
assert(progressHud.includes('TClientModernUi::Accent(') && progressHud.includes('m_MapProgressHudText.Render(') && progressHud.includes('m_MapProgressHudLabel.Render('), 'The percentage uses the visual accent and both pieces use cached text');
assert(hud.includes('TCLocalize("Finish", "Map progress")'), 'Use the localized finish label');
for(const part of ['RenderSpectatorCount', 'RenderDummyActions', 'RenderDrownBoard'])
  assert(functionBody(hud, 'void CHud::' + part + '(').includes('RightHudTop'), part + ' must use the occupied right-hand stack');
assert.doesNotMatch(functionBody(hud, 'void CHud::OnRender()'), /UpdateMapProgress\(/, 'Render must not update or generate map progress');
const islandMusic = read('src/game/client/components/tclient/island_music.cpp');
assert.match(islandMusic, /const bool ShowMapProgressSlot[\s\S]{0,260}IsMapProgressEnabled\(\)/, 'The island must not reserve progress space outside Gores');
assert(islandMusic.includes('GetMapProgressIslandValue(g_Config.m_ClDummy, MapProgressValue)'), 'Island reads the HUD-sampled percentage');
assert.match(islandMusic, /MapProgressIconSize, FontIcon::FLAG_CHECKERED, EFontPreset::ICON_FONT/, 'Measure the finish flag with the same font used to draw it');
assert.match(islandMusic, /m_MapProgressIcon\.Update\([^;]+;\s*m_pTextRender->SetFontPreset\(EFontPreset::DEFAULT_FONT\)/, 'Restore the text font before drawing the percentage or track title');
assert.match(islandMusic, /MapProgressTextW = ShowMapProgressSlot \? MeasureCached\([^;]*"100\.0%"/, 'Unknown and valid progress must reserve identical numeric widths');
assert.match(hudMapProgress, /ISLAND_MAP_PROGRESS_FONT_SCALE = 0\.875f/);
assert.match(hudMapProgress, /ISLAND_MAP_PROGRESS_ICON_SIZE = 10\.0f/);
assert.match(hudMapProgress, /ISLAND_MAP_PROGRESS_ICON_GAP = 3\.0f/);
assert.match(hudMapProgress, /ISLAND_MAP_PROGRESS_PADDING = 14\.0f/);
assert.match(islandMusic, /const float MapProgressTextSize = TimerSize \* TClientHudMapProgress::ISLAND_MAP_PROGRESS_FONT_SCALE/);
assert.match(islandMusic, /const float MapProgressPadding = TClientHudMapProgress::ISLAND_MAP_PROGRESS_PADDING \* Scale/);
const islandProgressDraw = functionBody(islandMusic, 'auto DrawMapProgressCell =');
assert.match(islandProgressDraw, /g_Config\.m_TcMapProgressFollowAccent \?\s*TClientModernUi::Accent\([\s\S]*?:\s*ColorRGBA\(1\.0f, 1\.0f, 1\.0f, 1\.0f\)/,
  'Island finish flag and percentage must default to white with opt-in accent color');
assert.match(islandProgressDraw, /m_MapProgressIcon\.Render\([^;]*ProgressColor\)/);
assert.match(islandProgressDraw, /m_MapProgressText\.Render\([^;]*ProgressColor\)/);
assert(islandMusic.includes('DrawMapProgressCell(CursorX,') && islandMusic.includes('DrawMapProgressCell(CellX,'), 'Idle and playing must draw the same cached flag and value');
assert.doesNotMatch(islandMusic, /RenderMapProgressIsland\(/, 'No separate external map strip is drawn by the island wrapper');
assert.match(islandMusic, /tc_island_show_song_progress[\s\S]*?RailH = 3\.0f \* Scale/, 'The song rail remains an independent island feature');

const menus = read('src/game/client/components/tclient/menus_tclient.cpp');
assert(menus.includes('if(g_Config.m_TcMapProgress)'), 'Advanced map-progress settings should be gated by the disabled master switch');
assert(menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcMapProgress,'));

assert(!menus.includes('m_TcMapProgressColor'), 'Do not show a separate color picker when progress follows the visual accent');
assert(islandMusic.includes('FontIcon::FLAG_CHECKERED') && islandMusic.includes('TClientModernUi::Accent('), 'The Island finish mark must use the native icon and visual accent');
const sharedLocaleKeys = [
  'Map progress',
		'Show estimated finish percentage',
		'Use the Dynamic Island instead of the HUD',
		'Show progress in the Dynamic Island instead of at the bottom right. If the island is off or unsupported, show it at the bottom right.',
		'On Gores, shows your finish progress or that of the player you are watching. No target or route: --.',
		'On Gores, show your finish progress. When spectating, show the selected player instead.',
];
for(const locale of ['simplified_chinese.txt', 'traditional_chinese.txt']) {
  const dictionary = read(path.join('data/tclient/languages', locale));
  for(const key of sharedLocaleKeys)
    assert(dictionary.includes(`\n${key}\n== `), `${locale} is missing a translation for: ${key}`);
}
const simplifiedChinese = read(path.join('data/tclient/languages', 'simplified_chinese.txt'));
assert(simplifiedChinese.includes('\nOn Gores, show your finish progress. When spectating, show the selected player instead.\n== '), 'Simplified Chinese is missing the exclusive-display hover explanation');
assert(menus.includes('Show estimated finish percentage'), 'The master switch must describe the enabled progress display');
assert(menus.includes('Use the Dynamic Island instead of the HUD'), 'The destination choice must be explicitly exclusive');
assert(!menus.includes('Fallback horizontal position') && !menus.includes('Fallback vertical position'), 'Legacy fallback-position sliders must not imply that the fixed HUD anchor moves');
assert(config.includes('Legacy map progress fallback horizontal position (unused)'), 'Legacy position cvars must remain available without changing saved values');

const cmake = read('CMakeLists.txt');
assert(cmake.includes('components/tclient/map_progress.h') && cmake.includes('components/tclient/hud_map_progress.h'));
assert(cmake.includes('tclient_map_progress_test.cpp') && cmake.includes('tclient_hud_map_progress_test.cpp'));
console.log('Map-progress default, budget, reset, HUD, settings, locale and test wiring verified.');
