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
assert.match(config, /tc_map_progress_style[^\n]*percentage inside the Dynamic Island/i);

const client = read('src/game/client/components/tclient/tclient.cpp');
const onUpdate = functionBody(client, 'void CTClient::OnUpdate()');
assert(onUpdate.includes('UpdateMapProgress(false)'), 'Incremental map/distance-field work must advance on regular updates');
assert(!onUpdate.includes('ResetMapProgress()'), 'Do not discard an enabled map scan every frame');
const snapshot = functionBody(client, 'void CTClient::OnNewSnapshot()');
assert(snapshot.includes('UpdateMapProgress(true)'), 'Snapshot observations must update the current route estimate');
const update = functionBody(client, 'void CTClient::UpdateMapProgress(bool NewSnapshot)');
assert.match(update, /if\(!g_Config\.m_TcMapProgress\)[\s\S]*?ResetMapProgress\(\)/, 'Disabling the cvar must clear cached progress');
assert(update.includes('MAP_PROGRESS_TILE_SCAN_BUDGET') && update.includes('MAP_PROGRESS_DIJKSTRA_BUDGET'), 'Map scan and route solving must be budgeted');
assert(update.includes('m_MapProgressMapCrc != CurrentMapCrc') && update.includes('m_pMapProgressCollision != pCollision'), 'Map/geometry identity changes must invalidate stale routes');
assert(update.includes('m_MapProgressFailedForMap') && update.includes('m_MapProgressFailedMapCrc == CurrentMapCrc'), 'A failed map identity must stay disabled without retrying allocations each frame');
assert(update.includes('TClientMapProgress::IsMapSizeWithinProgressBudget') && update.includes('TClientMapProgress::IsMapProgressVisualMetadataWithinBudget'), 'Tile and visual metadata budgets must fail closed before scanning');
assert.match(update, /catch\(const std::bad_alloc &\)[\s\S]*?FailMapProgressForMap\(pFailureMap, pFailureCollision, FailureMapCrc\)/, 'Allocation failures must latch the current map as unavailable');
assert.doesNotMatch(update, /UnloadData\(/, 'Map-progress scans borrow datafile blocks and must not release shared indices');
const failMap = functionBody(client, 'void CTClient::FailMapProgressForMap(');
assert(failMap.includes('ResetMapProgress()') && failMap.includes('m_MapProgressFailedForMap = true'), 'Fail-closed must clear partial route state then latch map identity');
const visualScan = functionBody(client, 'void CTClient::ScanMapProgressVisualLayers(');
assert(visualScan.includes('pMap->GetDataSize(pTilemap->m_Data) < RequiredBytes') && visualScan.includes('if(!pVisualTiles)'), 'Malformed or unavailable borrowed visual tile data must fail closed');
assert.match(update, /ScanMapProgressVisualLayers\(MAP_PROGRESS_TILE_SCAN_BUDGET\)[\s\S]*?if\(m_MapProgressFailedForMap\)[\s\S]*?return;/, 'The frame must stop immediately after a visual scan latches failure');
assert(/if\(!pCurrentMap \|\| !pCurrentMap->IsLoaded\(\)\)[\s\S]*?ResetMapProgress\(\);[\s\S]*?return;[\s\S]*?pCurrentMap->BaseName\(\)/.test(update), 'Never query the map name while its datafile is closed');
const reset = functionBody(client, 'void CTClient::ResetMapProgress()');
assert.doesNotMatch(reset, /BaseName\(|Crc\(|UnloadData\(/, 'Reset must discard cached state without reading or releasing shared map data');
assert(reset.includes('m_MapProgressFailedForMap = false'), 'A map identity change clears the failed-map latch');
const estimate = functionBody(client, 'TClientMapProgress::SEstimate CTClient::MapProgressEstimate(int Conn) const');
assert(/if\(!pCurrentMap \|\| !pCurrentMap->IsLoaded\(\)\)[\s\S]*?return \{\};[\s\S]*?pCurrentMap->BaseName\(\)/.test(estimate), 'HUD queries must tolerate the map-unload window');
assert(update.includes('TClientMapProgress::MakeTile(') && update.includes('m_aMapProgressPlayers[Slot].Observe(') && update.includes('m_aMapProgressPlayers[Slot].Update('), 'Production sampling must feed the tested Qm-derived algorithm');

const hud = read('src/game/client/components/hud.cpp');
const onHudRender = functionBody(hud, 'void CHud::OnRender()');
assert(onHudRender.includes('UpdateMapProgress()'), 'HUD smoothing must update from the production estimate');
assert(onHudRender.includes('RenderMapProgressBelowBoard'), 'The percent must be rendered below the drown board when there is room');
assert(hud.includes('std::optional<CHud::SDrownBoardPlacement> CHud::RenderDrownBoard()'), 'The drown board must report its actual drawn bounds');
const boardRender = functionBody(hud, 'bool CHud::RenderMapProgressBelowBoard(');
assert(boardRender.includes('TClientHudMapProgress::BoardValue(Board)') && boardRender.includes('TextRender()->Text('), 'Below-board output is a percentage text, not a bar');
assert.doesNotMatch(boardRender, /DrawRect\(/, 'No progress rail is drawn below the board');
const board = functionBody(hud, 'std::optional<CHud::SDrownBoardPlacement> CHud::RenderDrownBoard()');
assert(board.includes('SafeBottom = BoardBottom') && board.includes('BOARD_VALUE_HEIGHT') && board.includes('std::max(BoardBottom - BoardHeight, 32.0f)'), 'Reserve one row under the drown board and raise the panel to make room');
const fallback = functionBody(hud, 'void CHud::RenderMapProgressStandalone(int Dummy)');
assert(fallback.includes('TextRender()->Text(') && !fallback.includes('Graphics()->DrawRect('), 'Fallback also contains only the percentage text');
assert(onHudRender.includes('RenderMapProgressBelowBoard') && onHudRender.includes('RenderMapProgressStandalone'), 'Render below the board, with a text-only fallback if unavailable');
const islandMusic = read('src/game/client/components/tclient/island_music.cpp');
assert(islandMusic.includes('GetMapProgressIslandValue(g_Config.m_ClDummy, MapProgressValue)'), 'Island reads the HUD-sampled percentage');
assert(islandMusic.includes('MapProgressCellW') && islandMusic.includes('aMapProgress'), 'Island budgets and draws the numeric cell');
assert.doesNotMatch(islandMusic, /RenderMapProgressIsland\(/, 'No separate external map strip is drawn by the island wrapper');
assert.match(islandMusic, /tc_island_show_song_progress[\s\S]*?RailH = 3\.0f \* Scale/, 'The song rail remains an independent island feature');

const menus = read('src/game/client/components/tclient/menus_tclient.cpp');
assert(menus.includes('if(g_Config.m_TcMapProgress)'), 'Advanced map-progress settings should be gated by the disabled master switch');
assert(menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcMapProgress,'));

const sharedLocaleKeys = [
  'Map progress',
  'Show estimated finish percentage below the drown board',
  'Also show the percentage inside the Dynamic Island',
  'Map progress is estimated locally on Gores and DDRace maps; it can stay unknown until a route is available.',
  'Map progress percentage color',
  'Fallback percentage width',
  'Fallback percentage height',
  'Fallback horizontal position',
  'Fallback vertical position',
];
for(const locale of ['simplified_chinese.txt', 'traditional_chinese.txt']) {
  const dictionary = read(path.join('data/tclient/languages', locale));
  for(const key of sharedLocaleKeys)
    assert(dictionary.includes(`\n${key}\n== `), `${locale} is missing a translation for: ${key}`);
}
const simplifiedChinese = read(path.join('data/tclient/languages', 'simplified_chinese.txt'));
assert(simplifiedChinese.includes('\nShow the estimated finish percentage below the drown board on Gores and DDRace maps.\n== '), 'Simplified Chinese is missing the map-progress hover explanation');

const cmake = read('CMakeLists.txt');
assert(cmake.includes('components/tclient/map_progress.h') && cmake.includes('components/tclient/hud_map_progress.h'));
assert(cmake.includes('tclient_map_progress_test.cpp') && cmake.includes('tclient_hud_map_progress_test.cpp'));
console.log('Map-progress default, budget, reset, HUD, settings, locale and test wiring verified.');
