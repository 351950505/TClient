// node scripts/test_freeze_entry_server_confirmation.js
// Standalone regression: freeze-entry episodes only rearm on an authoritative server thaw.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const players = fs.readFileSync(path.join(root, 'src/game/client/components/players.cpp'), 'utf8').replace(/\r\n/g, '\n');
const tracker = fs.readFileSync(path.join(root, 'src/game/client/components/tclient/frozen_entry_highlight.h'), 'utf8').replace(/\r\n/g, '\n');

function functionBody(source, signature) {
  const start = source.indexOf(signature);
  assert(start >= 0, `Missing production seam: ${signature}`);
  const open = source.indexOf('{', start + signature.length);
  assert(open >= 0, `Missing function body: ${signature}`);
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    if (source[i] === '}' && --depth === 0) return source.slice(open + 1, i);
  }
  assert.fail(`Unterminated function body: ${signature}`);
}

const update = functionBody(players, 'void CPlayers::UpdateFreezeEntryTileHighlight()');
assert.match(update, /m_HasExtendedData/, 'Freeze state must come from a valid extended server snapshot');
assert.match(update, /m_ExtendedData\.m_FreezeEnd\s*!=\s*0/, 'Timed and deep freezes must use the authoritative freeze end');
assert.match(update, /CHARACTERFLAG_MOVEMENTS_DISABLED/, 'Live freeze must use the server movement-disabled flag');
assert.match(update, /m_ExtendedData\.m_FreezeEnd\s*!=\s*0\s*\|\|\s*\(Character\.m_ExtendedData\.m_Flags\s*&\s*CHARACTERFLAG_MOVEMENTS_DISABLED\)/, 'The predicate must combine timed/deep freeze state with the live-freeze state');
assert.doesNotMatch(update, /CHARACTERFLAG_IN_FREEZE/, 'Tile occupancy flags must not define the freeze episode');
assert.doesNotMatch(update, /\.m_(?:Regular)?Predicted\b/, 'Prediction must not trigger or locate the freeze tile');
assert.match(update, /Update\(\s*ActiveLocalTee,\s*HasServerSnapshot,\s*ServerConfirmedFrozen,\s*Enabled,\s*Visible/, 'Spectator mode must continue feeding authoritative snapshots to the tracker');
assert.match(update, /const bool Visible\s*=\s*Enabled\s*&&\s*!Spectating\s*&&\s*Width\s*>\s*0\s*&&\s*Height\s*>\s*0/, 'Spectating or unavailable maps must hide the flash without interrupting tracking');
assert.match(update, /if\(in_range\(ClientId,\s*MAX_CLIENTS\s*-\s*1\)\s*&&\s*m_aFreezeEntryClientId\[Dummy\]\s*!=\s*ClientId\)/, 'Only a new valid local character identity may reset the tracker');
assert.match(update, /if\(!ActiveLocalTee\)[\s\S]*?if\(PrimaryLocalTee\s*&&\s*!Spectating\)\s*m_aLocalFreezeEntryHighlight\[Dummy\]\.Reset\(\)[\s\S]*?Update\(false,\s*false,\s*false/, 'Death resets the character episode; spectator or inactive state must not be reported as a thaw');
assert.match(update, /m_Cur\.m_X/, 'Freeze tile lookup must use the server snapshot X position');
assert.match(update, /m_Cur\.m_Y/, 'Freeze tile lookup must use the server snapshot Y position');
const render = functionBody(players, 'void CPlayers::RenderFreezeEntryTileHighlight()');
assert.match(render, /m_SpecInfo\.m_Active[\s\S]*?return;/, 'Never draw the local highlight while spectating');
assert.match(tracker, /m_EpisodeKnown/);
assert.match(tracker, /m_EpisodeFrozen/);
assert.match(tracker, /m_Armed\s*=\s*true/);
assert.match(tracker, /Frozen\s*&&\s*!m_EpisodeFrozen[\s\S]*?m_Armed\s*=\s*false/, 'A freeze entry consumes its arm exactly once');
assert.match(tracker, /!Frozen\s*&&\s*m_EpisodeFrozen[\s\S]*?m_Armed\s*=\s*true/, 'Only a valid server thaw rearms the next episode');
console.log('Freeze-entry episodes use authoritative freeze and thaw snapshots and do not reflash while frozen.');
