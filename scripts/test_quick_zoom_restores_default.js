// node scripts/test_quick_zoom_restores_default.js
// Source-seam regression: quick zoom off restores the configured default zoom.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const camera = fs.readFileSync(path.join(root, 'src/game/client/components/camera.cpp'), 'utf8').replace(/\r\n/g, '\n');

function functionBody(source, signature) {
  const start = source.indexOf(signature);
  assert(start >= 0, `Missing production seam: ${signature}`);
  const open = source.indexOf('{', start + signature.length);
  assert(open >= 0, `Missing function body: ${signature}`);
  let depth = 0;
  for(let i = open; i < source.length; i++) {
    if(source[i] === '{') depth++;
    if(source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  assert.fail(`Unterminated function body: ${signature}`);
}

const quickZoom = functionBody(camera, 'void CCamera::ConQuickZoom(');
assert(quickZoom.includes('CCamera::ZoomStepsToValue(CCamera::DefaultZoomSteps())'), 'Restore target must use the migrated/clamped configured default zoom');
assert(quickZoom.includes('tclient_quick_zoom::SelectTarget(pSelf->m_QuickZoomActive, DefaultZoom, QuickZoom)'), 'Toggle-off must select default zoom; toggle-on must select quick zoom');
assert(!quickZoom.includes('m_QuickZoomSaved'), 'Do not restore a stale pre-toggle camera value');
assert(quickZoom.includes('m_MultiViewPersonalZoom = TargetLevel - 10.0f'), 'MultiView must receive the same restored default target');
const helper = fs.readFileSync(path.join(root, 'src/game/client/components/quick_zoom.h'), 'utf8');
assert.match(helper, /return QuickZoomActive \? DefaultZoom : QuickZoom;/, 'The tested production selector restores default when toggled off');
const unitTest = fs.readFileSync(path.join(root, 'src/test/camera_quick_zoom_test.cpp'), 'utf8');
assert(unitTest.includes('TurningOnSelectsQuickTargetAndTurningOffSelectsConfiguredDefault'), 'Add behavioral regression coverage for both toggle states');
const cmake = fs.readFileSync(path.join(root, 'CMakeLists.txt'), 'utf8');
assert(cmake.includes('components/quick_zoom.h') && cmake.includes('camera_quick_zoom_test.cpp'), 'Register helper and unit test');
console.log('Quick zoom toggle-on selects the quick target; toggle-off restores configured default.');
