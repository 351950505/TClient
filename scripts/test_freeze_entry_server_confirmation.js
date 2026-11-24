// node scripts/test_freeze_entry_server_confirmation.js
// Standalone regression: local freeze-entry highlighting must follow the current server snapshot.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const players = fs.readFileSync(path.join(root, 'src/game/client/components/players.cpp'), 'utf8').replace(/\r\n/g, '\n');

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
assert.match(update, /m_ExtendedData\.m_Flags\s*&\s*CHARACTERFLAG_IN_FREEZE/, 'Freeze entry must wait for the server snapshot flag');
assert.match(update, /m_Cur\.m_X/, 'Freeze tile lookup must use the server snapshot X position');
assert.match(update, /m_Cur\.m_Y/, 'Freeze tile lookup must use the server snapshot Y position');
assert.doesNotMatch(update, /\.m_(?:Regular)?Predicted\b/, 'Prediction must not trigger or locate the freeze tile');
assert.match(update, /ActiveLocalTee\s*&&\s*Spectating\s*&&\s*Enabled[\s\S]*?\.Suspend\(\);/, 'Spectator transitions must suspend visibility without clearing the triggering freeze state');
const render = functionBody(players, 'void CPlayers::RenderFreezeEntryTileHighlight()');
assert.match(render, /m_SpecInfo\.m_Active[\s\S]*?return;/, 'Never draw the local highlight while spectating');
console.log('Freeze-entry highlight waits for the server-confirmed snapshot and survives a spectator-view gap.');
