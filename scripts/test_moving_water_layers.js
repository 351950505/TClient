#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const ROOT = path.resolve(__dirname, '..');
const read = (relativePath) => fs.readFileSync(path.join(ROOT, relativePath), 'utf8');
const movingSource = read('src/game/client/components/tclient/moving_tiles.cpp');
const movingHeader = read('src/game/client/components/tclient/moving_tiles.h');
const configSource = read('src/engine/shared/config_variables_tclient.h');
const envelopeSource = read('src/game/client/components/envelope_state.cpp');

function extractBlock(source, marker) {
  const markerIndex = source.indexOf(marker);
  assert.notEqual(markerIndex, -1, `missing source seam: ${marker}`);
  const openIndex = source.indexOf('{', markerIndex + marker.length);
  assert.notEqual(openIndex, -1, `missing body for source seam: ${marker}`);

  let depth = 0;
  let state = 'code';
  for (let i = openIndex; i < source.length; i += 1) {
    const current = source[i];
    const next = source[i + 1];
    if (state === 'line-comment') {
      if (current === '\n') state = 'code';
      continue;
    }
    if (state === 'block-comment') {
      if (current === '*' && next === '/') {
        state = 'code';
        i += 1;
      }
      continue;
    }
    if (state === 'string' || state === 'character') {
      if (current === '\\') {
        i += 1;
      } else if ((state === 'string' && current === '"') || (state === 'character' && current === "'")) {
        state = 'code';
      }
      continue;
    }
    if (current === '/' && next === '/') {
      state = 'line-comment';
      i += 1;
      continue;
    }
    if (current === '/' && next === '*') {
      state = 'block-comment';
      i += 1;
      continue;
    }
    if (current === '"') {
      state = 'string';
      continue;
    }
    if (current === "'") {
      state = 'character';
      continue;
    }
    if (current === '{') depth += 1;
    if (current === '}') {
      depth -= 1;
      if (depth === 0) return source.slice(openIndex + 1, i);
    }
  }
  throw new Error(`unterminated source seam: ${marker}`);
}

function check(condition, message) {
  assert.ok(condition, message);
}

// Preserve the established user-facing switch and its default-off behavior.
const configMatch = configSource.match(
  /MACRO_CONFIG_INT\(TcMovingTilesEntities,\s*tc_moving_tiles_entities,\s*(\d+),\s*(\d+),\s*(\d+),\s*CFGFLAG_CLIENT\s*\|\s*CFGFLAG_SAVE/
);
check(configMatch, 'the existing tc_moving_tiles_entities config entry must remain registered');
assert.deepEqual(configMatch.slice(1).map(Number), [0, 0, 1], 'moving entities stays binary and default-off');
console.log('PASS config: existing entity-layer switch remains 0/1, default 0.');

// Interpret the production name table and render-pass gate as a semantic classifier.
const typeMatch = movingHeader.match(/enum class EQType\s*\{([\s\S]*?)\};/);
const namesMatch = movingHeader.match(/static constexpr char ValidQuadNames\[\]\[30\]\s*=\s*\{([\s\S]*?)\};/);
check(typeMatch && namesMatch, 'production quad type enum and name table must be present');
const types = typeMatch[1].split(',').map((part) => part.match(/[A-Z_]+/)?.[0]).filter(Boolean);
const quadTypes = types.filter((type) => type !== 'NONE' && type !== 'NUM');
const quadNames = [...namesMatch[1].matchAll(/"([^"]+)"/g)].map((match) => match[1]);
assert.deepEqual(quadNames, ['QFr', 'QUnFr', 'QDeath', 'QStopa', 'QCfrm', 'QHook', 'QUnHook']);
assert.deepEqual(quadTypes, ['FREEZE', 'UNFREEZE', 'DEATH', 'STOPA', 'CFRM', 'HOOKABLE', 'UNHOOKABLE']);
check(movingSource.includes('Type = static_cast<EQType>(Names);'), 'layer name index must map to its corresponding quad type');
check(
  /if\(!m_RenderAbove\s*&&\s*\(Type == EQType::HOOKABLE\s*\|\|\s*Type == EQType::UNHOOKABLE\)\)\s*continue;\s*if\(m_RenderAbove\s*&&\s*Type != EQType::HOOKABLE\s*&&\s*Type != EQType::UNHOOKABLE\)\s*continue;/.test(movingSource),
  'production render-pass gate must keep hook layers above and other moving layers below'
);
const classifyName = (name) => {
  const index = quadNames.indexOf(name);
  return index < 0 ? null : quadTypes[index];
};
const drawInPass = (type, above) => above ? type === 'HOOKABLE' || type === 'UNHOOKABLE' : type !== 'HOOKABLE' && type !== 'UNHOOKABLE';
for (let i = 0; i < quadNames.length; i += 1) {
  const type = classifyName(quadNames[i]);
  assert.equal(type, quadTypes[i], `${quadNames[i]} maps to ${quadTypes[i]}`);
  assert.equal(drawInPass(type, false), i < 5, `${quadNames[i]} lower-pass selection`);
  assert.equal(drawInPass(type, true), i >= 5, `${quadNames[i]} upper-pass selection`);
}
assert.equal(classifyName('QWater'), null, 'unknown water labels must not be guessed as a supported quad type');
console.log('PASS layer selection: known names map to types; hook layers render above, the rest below.');

// Ensure image indices bind only their matching loaded-map slot; invalid indices clear stale binding.
check(
  /if\(pLayer->m_Image >= 0 && pLayer->m_Image < GameClient\(\)->m_MapImages\.Num\(\)\)\s*Graphics\(\)->TextureSet\(GameClient\(\)->m_MapImages\.Get\(pLayer->m_Image\)\);\s*else\s*Graphics\(\)->TextureClear\(\);/.test(movingSource),
  'production image binding must range-check the layer image index and clear on invalid indices'
);
const mapImages = [{ id: 'image-0' }, { id: 'image-1' }, { id: 'image-2' }];
const resolveImageBinding = (index, images) => index >= 0 && index < images.length ? images[index] : null;
assert.equal(resolveImageBinding(1, mapImages), mapImages[1], 'valid layer image index binds the same image item');
assert.equal(resolveImageBinding(-1, mapImages), null, 'negative image index cannot bind an unrelated image');
assert.equal(resolveImageBinding(mapImages.length, mapImages), null, 'out-of-range image index cannot bind an unrelated image');
console.log('PASS image binding: valid indices select their own image; invalid indices clear the prior texture.');

// Tie the source seam to the evaluator's cache, then exercise animated position and color at two times.
const renderBody = extractBlock(movingSource, 'void CMovingTiles::OnRender()');
const updateCall = 'm_EnvEvaluator.Update();';
const updateIndex = renderBody.indexOf(updateCall);
check(updateIndex >= 0, 'active moving-quad render must refresh the envelope clock');
assert.equal(renderBody.split(updateCall).length - 1, 1, 'refresh the evaluator exactly once per active render');
const firstEvalIndex = renderBody.indexOf('m_EnvEvaluator.EnvelopeEval(');
check(firstEvalIndex > updateIndex, 'refresh cached time before evaluating quad envelopes');
assert.equal((renderBody.match(/m_EnvEvaluator\.EnvelopeEval\(/g) || []).length, 2, 'position and color both use the updated evaluator');
const earlyReturns = renderBody.match(
  /if\(g_Config\.m_ClOverlayEntities != 100\)\s*return;\s*if\(!g_Config\.m_TcMovingTilesEntities\)\s*return;\s*if\(m_vQuads\.empty\(\)\)\s*return;/
);
check(earlyReturns && renderBody.indexOf(earlyReturns[0]) < updateIndex, 'disabled/empty render paths must return before refreshing the evaluator');

const updateBody = extractBlock(envelopeSource, 'void CEnvelopeState::Update()');
const evalBody = extractBlock(envelopeSource, 'void CEnvelopeState::EnvelopeEval(');
check(/m_CachedTime\s*=/.test(updateBody), 'CEnvelopeState::Update must advance its cached time');
check(/m_CachedTime\s*\+\s*std::chrono::milliseconds\(TimeOffsetMillis\)/.test(evalBody), 'EnvelopeEval must sample from the cached time');
check(/Client\(\)->PredGameTick|Client\(\)->GameTick/.test(updateBody), 'online cached time must derive from the current game tick');

function sampleLinear(keys, timeMs, valueKey) {
  if (timeMs <= keys[0].time) return keys[0][valueKey];
  const last = keys[keys.length - 1];
  if (timeMs >= last.time) return last[valueKey];
  for (let i = 1; i < keys.length; i += 1) {
    if (timeMs <= keys[i].time) {
      const before = keys[i - 1];
      const after = keys[i];
      const t = (timeMs - before.time) / (after.time - before.time);
      return before[valueKey] + (after[valueKey] - before[valueKey]) * t;
    }
  }
  throw new Error('unreachable envelope sample');
}

const positionEnvelope = [
  { time: 0, x: 0, rotationDeg: 0 },
  { time: 1000, x: 100, rotationDeg: 90 },
];
const colorEnvelope = [
  { time: 0, alpha: 1 },
  { time: 1000, alpha: 0 },
];
const evaluator = { cachedTime: 0, updates: 0 };
function renderFrame({ overlayEntities, enabled, quadCount, timeMs }) {
  const active = overlayEntities === 100 && enabled && quadCount > 0;
  if (active) {
    // This path is guarded by the production-source seam checks above.
    evaluator.cachedTime = timeMs;
    evaluator.updates += 1;
  }
  return {
    position: {
      x: sampleLinear(positionEnvelope, evaluator.cachedTime, 'x'),
      rotationDeg: sampleLinear(positionEnvelope, evaluator.cachedTime, 'rotationDeg'),
    },
    color: { alpha: sampleLinear(colorEnvelope, evaluator.cachedTime, 'alpha') },
  };
}
const firstFrame = renderFrame({ overlayEntities: 100, enabled: true, quadCount: 1, timeMs: 250 });
const secondFrame = renderFrame({ overlayEntities: 100, enabled: true, quadCount: 1, timeMs: 750 });
assert.deepEqual(firstFrame, { position: { x: 25, rotationDeg: 22.5 }, color: { alpha: 0.75 } });
assert.deepEqual(secondFrame, { position: { x: 75, rotationDeg: 67.5 }, color: { alpha: 0.25 } });
assert.notDeepEqual(secondFrame, firstFrame, 'position, rotation, and color envelopes advance between frames');
assert.equal(evaluator.updates, 2, 'one envelope-time refresh per active frame');
const updatesBeforeDisabledFrame = evaluator.updates;
const disabledFrame = renderFrame({ overlayEntities: 100, enabled: false, quadCount: 1, timeMs: 900 });
assert.equal(evaluator.updates, updatesBeforeDisabledFrame, 'default-off path does not update or render moving envelopes');
assert.deepEqual(disabledFrame, secondFrame, 'inactive path leaves the prior evaluator cache untouched');
const reenabledFrame = renderFrame({ overlayEntities: 100, enabled: true, quadCount: 1, timeMs: 900 });
assert.equal(reenabledFrame.position.x, 90, 're-enabling refreshes the cached time before sampling');
console.log('PASS envelope time: active frames advance position/rotation/color; inactive path stays gated and re-enable catches up.');

// Adapted from QmClient's licensed moving-water implementation: a layer name or
// its assigned image name containing "water" but not "sun" is freeze-water art;
// it is shown only on Gores/Axiom servers. Alpha's source is unavailable, so we
// claim Qm-equivalent behavior, not a verbatim Meow alpha port.
const waterNameBody = extractBlock(movingSource, 'static bool IsMovingWaterName(');
check(/str_find_nocase\(pName, "sun"\)\s*==\s*nullptr/.test(waterNameBody), 'exclude sun art from water-name matching');
check(/str_find_nocase\(pName, "water"\)\s*!=\s*nullptr/.test(waterNameBody), 'match water art case-insensitively');
const waterImageBody = extractBlock(movingSource, 'static bool QuadLayerUsesMovingWaterImage(');
check(waterImageBody.includes('pLayer->m_Image < 0 || pLayer->m_Image >= ImagesNum'), 'water image lookup must validate the layer image index');
check(!waterImageBody.includes('UnloadData('), 'image-name data is borrowed and may alias collision or rendered quad data');
const mapLoadBody = extractBlock(movingSource, 'void CMovingTiles::OnMapLoad()');
check(/if\(Type == EQType::NONE && IsMovingWaterName\(aLayerName\)\)[\s\S]*?Type = EQType::FREEZE;[\s\S]*?RequiresGoresOrAxiom = true;/.test(mapLoadBody), 'water quad layer names become freeze-water layers');
check(/if\(Type == EQType::NONE && QuadLayerUsesMovingWaterImage\([\s\S]*?Type = EQType::FREEZE;[\s\S]*?RequiresGoresOrAxiom = true;/.test(mapLoadBody), 'quad layers using water images become freeze-water layers');
const gateBody = extractBlock(movingSource, 'bool CMovingTiles::ShouldRenderGoresOrAxiomWater()');
check(gateBody.includes('IClient::STATE_ONLINE') && gateBody.includes('"gores"') && gateBody.includes('"axiom"'), 'water visibility must follow online Gores/Axiom server detection');
const gatedRender = extractBlock(movingSource, 'void CMovingTiles::OnRender()');
check(gatedRender.includes('!m_HasGoresOrAxiomQuads || ShouldRenderGoresOrAxiomWater()'), 'avoid server lookup unless the map has water art');
check(gatedRender.includes('QuadData.m_RequiresGoresOrAxiom && !RenderGoresOrAxiomWater'), 'only water quads use the Gores/Axiom visibility gate');
function matchesWaterName(value) {
  return Boolean(value) && !value.toLowerCase().includes('sun') && value.toLowerCase().includes('water');
}
for(const value of ['water', 'Waterfall', 'blackwater', 'dark_water'])
  assert.equal(matchesWaterName(value), true, `${value} should be recognized as water artwork`);
for(const value of ['', 'sunwater', 'water_sun', 'freeze'])
  assert.equal(matchesWaterName(value), false, `${value} should not be classified as moving water artwork`);
console.log('PASS water matching: quad-layer/image names follow the Qm rule and exclude sun artwork.');
console.log('PASS server gate: only water quads are restricted to online Gores/Axiom contexts.');
console.log('Moving-water source regression checks passed (Qm-equivalent behavior; alpha source unavailable).');
