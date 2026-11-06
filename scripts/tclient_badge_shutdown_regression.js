#!/usr/bin/env node
'use strict';

// Compiles the current production layout and shutdown function in isolation.
// Uses real Windows process enumeration/termination on disposable executable copies.
// Access-denied and snapshot failures are injected at the Windows API boundary.
// node scripts/tclient_badge_shutdown_regression.js --output <directory>
// Outside a VS prompt also pass --msvc <VC/Tools/MSVC/version>,
// --sdk <Windows Kits/10>, and --sdk-version <version>.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const zlib = require('node:zlib');
const assert = require('node:assert/strict');

const options = {};
for(let i = 2; i < process.argv.length; i += 2)
{
	const key = process.argv[i].replace(/^--/, '');
	assert(['output', 'msvc', 'sdk', 'sdk-version'].includes(key) && process.argv[i + 1], 'Expected option and value');
	options[key] = process.argv[i + 1];
}
const root = path.resolve(__dirname, '..');
const output = options.output ? path.resolve(options.output) : fs.mkdtempSync(path.join(os.tmpdir(), 'tclient-badge-shutdown-'));
fs.mkdirSync(output, {recursive: true});
const env = {...process.env};
if(options.msvc)
{
	assert(options.sdk && options['sdk-version'], '--msvc also requires --sdk and --sdk-version');
	env.INCLUDE = [path.join(options.msvc, 'include'), ...['ucrt', 'shared', 'um', 'winrt'].map(p => path.join(options.sdk, 'Include', options['sdk-version'], p))].join(';');
	env.LIB = [path.join(options.msvc, 'lib/x64'), ...['ucrt', 'um'].map(p => path.join(options.sdk, 'Lib', options['sdk-version'], p, 'x64'))].join(';');
}
const compiler = options.msvc ? path.join(options.msvc, 'bin/Hostx64/x64/cl.exe') : 'cl.exe';
const scoreboard = fs.readFileSync(path.join(root, 'src/game/client/components/scoreboard.cpp'), 'utf8');
const tclient = fs.readFileSync(path.join(root, 'src/game/client/components/tclient/tclient.cpp'), 'utf8');
function between(text, first, last)
{
	const start = text.indexOf(first);
	const end = text.indexOf(last, start + first.length);
	assert(start >= 0 && end > start, `Missing production boundaries ${first} / ${last}`);
	return text.slice(start, end);
}
function compile(name, source)
{
	const file = path.join(output, name + '.cpp');
	const exe = path.join(output, name + '.exe');
	fs.writeFileSync(file, source);
	const result = cp.spawnSync(compiler, ['/nologo', '/EHsc', '/std:c++17', '/W4', '/WX', '/utf-8', file, '/Fe:' + exe, '/Fo:' + path.join(output, name + '.obj')], {cwd: output, env, encoding: 'utf8', timeout: 45000});
	fs.writeFileSync(path.join(output, name + '-compile.txt'), (result.stdout || '') + (result.stderr || '') + (result.error ? result.error.message : ''));
	assert.equal(result.status, 0, `Compile failed; see ${name}-compile.txt`);
	return exe;
}

const measurements = between(scoreboard, '// calculate measurements', '// render headlines');
const badge = between(scoreboard, 'const float BadgeX =', '\n\t\t\t}');
const layoutExe = compile('badge_layout_probe', `#include <algorithm>
#include <cstdio>
#include <cstring>
struct vec2 { float x, y; vec2(float X, float Y): x(X), y(Y) {} };
struct CUIRect { float x, y, w, h; };
struct { bool m_MeClientBadge; } g_Config;
struct CTextRender { float TextWidth(float FontSize, const char *pText) { return FontSize * 0.6f * (float)std::strlen(pText); } } Text;
CTextRender *TextRender() { return &Text; }
struct CBadge { vec2 m_Pos{0,0}; float m_Size = 0; void RenderBadge(vec2 Pos, float Size, float) { m_Pos = Pos; m_Size = Size; } };
struct CGame { CBadge m_ClientBadge; } Game;
CGame *GameClient() { return &Game; }
constexpr float MARGIN = 10.0f;
void Layout(int NumPlayers, float Width, bool UseTime, bool Enabled)
{
 g_Config.m_MeClientBadge = Enabled;
 CUIRect Scoreboard{0, 0, Width, 1000};
 const bool LowScoreboardWidth = Scoreboard.w < 350.0f;
 ${measurements}
 CUIRect Row{0, 0, Width, LineHeight};
 struct {float a = 1;} TextColor;
 ${badge}
 (void)RoundRadius; (void)ClanLength;
 std::printf("%d %.0f %d %d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\\n", NumPlayers, Width, UseTime, Enabled,
 Game.m_ClientBadge.m_Pos.x, Game.m_ClientBadge.m_Pos.y, Game.m_ClientBadge.m_Size,
 ScoreOffset, ScoreLength, LineHeight, Spacing, FontSize);
}
int main() { for(float Width : {250.0f, 349.0f, 350.0f, 800.0f}) for(int Time=0; Time<2; ++Time) for(int Enabled=0; Enabled<2; ++Enabled) for(int N=1; N<=128; ++N) Layout(N, Width, Time!=0, Enabled!=0); }
`);
const layoutRun = cp.spawnSync(layoutExe, [], {encoding: 'utf8', timeout: 10000});
assert.equal(layoutRun.status, 0);
const layouts = layoutRun.stdout.trim().split(/\r?\n/).map(line =>
{
	const [players, width, time, enabled, x, y, size, scoreOffset, scoreLength, lineHeight, spacing, fontSize] = line.split(' ').map(Number);
	return {players, width, time, enabled, x, y, size, scoreOffset, scoreLength, lineHeight, spacing, fontSize};
});

// Decode the shipped RGBA PNG with all five PNG row filters, retaining every nonzero alpha pixel.
const png = fs.readFileSync(path.join(root, 'data/tclient/penguin.png'));
assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
let width, height;
const idat = [];
for(let p = 8; p < png.length;)
{
	const length = png.readUInt32BE(p);
	const type = png.toString('ascii', p + 4, p + 8);
	const data = png.subarray(p + 8, p + 8 + length);
	if(type === 'IHDR')
	{
		width = data.readUInt32BE(0); height = data.readUInt32BE(4);
		assert.equal(data[8], 8); assert.equal(data[9], 6); assert.equal(data[12], 0);
	}
	if(type === 'IDAT') idat.push(data);
	p += length + 12;
}
const raw = zlib.inflateSync(Buffer.concat(idat));
const stride = width * 4;
const pixels = Buffer.alloc(height * stride);
function paeth(a, b, c)
{
	const p = a + b - c;
	const da = Math.abs(p - a), db = Math.abs(p - b), dc = Math.abs(p - c);
	return da <= db && da <= dc ? a : db <= dc ? b : c;
}
for(let y = 0; y < height; ++y)
{
	const filter = raw[y * (stride + 1)];
	assert(filter <= 4);
	for(let x = 0; x < stride; ++x)
	{
		const a = x >= 4 ? pixels[y * stride + x - 4] : 0;
		const b = y ? pixels[(y - 1) * stride + x] : 0;
		const c = y && x >= 4 ? pixels[(y - 1) * stride + x - 4] : 0;
		pixels[y * stride + x] = (raw[y * (stride + 1) + x + 1] + [0, a, b, Math.floor((a + b) / 2), paeth(a, b, c)][filter]) & 255;
	}
}
const occupied = [];
let left = width, top = height, right = 0, bottom = 0;
for(let y = 0; y < height; ++y) for(let x = 0; x < width; ++x) if(pixels[(y * width + x) * 4 + 3])
{
	occupied.push([x, y]);
	left = Math.min(left, x); top = Math.min(top, y); right = Math.max(right, x + 1); bottom = Math.max(bottom, y + 1);
}
assert.deepEqual([width, height, left, top, right, bottom], [128, 128, 21, 5, 107, 123]);
const seen = new Map();
for(const l of layouts)
{
	if(!l.enabled) { assert.equal(l.scoreOffset, 10); continue; }
	const visible = {left: l.x + left / width * l.size, right: l.x + right / width * l.size, top: l.y + top / height * l.size, bottom: l.y + bottom / height * l.size};
	assert(visible.left > 0 && visible.right < l.scoreOffset, 'Visible pixels invade panel edge or full-width score/time');
	assert(visible.top >= 0 && visible.bottom <= l.lineHeight, 'Visible pixels escape current row');
	assert(visible.bottom < visible.top + l.lineHeight + l.spacing, 'Adjacent visible rows overlap');
	assert.equal(l.size, Math.min(l.fontSize * 1.75, l.lineHeight));
	// Expand by one source texel as a conservative bilinear-filter footprint.
	assert(visible.right + l.size / width < l.scoreOffset);
	assert(visible.bottom + l.size / height < visible.top - l.size / height + l.lineHeight + l.spacing);
	seen.set(`${l.lineHeight}/${l.spacing}/${l.fontSize}`, {...l, visible});
}
const modes = [...seen.values()].sort((a,b) => b.lineHeight - a.lineHeight);
fs.writeFileSync(path.join(output, 'badge-layout.json'), JSON.stringify({cases: layouts.length, alpha: {width, height, bbox: [left, top, right, bottom], nonzeroPixels: occupied.length}, modes}, null, 2));
const image = 'data:image/png;base64,' + png.toString('base64');
let svg = '<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="600" viewBox="0 0 550 300"><rect width="550" height="300" fill="#171b22"/><text x="8" y="14" fill="white" font-size="8">Production layout + shipped PNG; illustrative text, not a game screenshot</text>';
modes.forEach((l, i) =>
{
	const baseY = 30 + i * 38;
	svg += `<text x="8" y="${baseY + 6}" fill="white" font-size="7">h=${l.lineHeight} size=${l.size}</text>`;
	for(let row = 0; row < 2; ++row)
	{
		const y = baseY + row * (l.lineHeight + l.spacing);
		svg += `<rect x="130" y="${y}" width="400" height="${l.lineHeight}" fill="${row ? '#343b47' : '#29313d'}"/><path d="M${130 + l.scoreOffset},${y}v${l.lineHeight}" stroke="#eb6666" stroke-width="0.2"/><image href="${image}" x="${130 + l.x}" y="${y + l.y}" width="${l.size}" height="${l.size}"/><text x="${132 + l.scoreOffset}" y="${y + (l.lineHeight + l.fontSize) / 2}" fill="white" font-family="monospace" font-size="${l.fontSize}">${row ? '00:00:00' : '99999'}</text>`;
	}
});
fs.writeFileSync(path.join(output, 'badge-layout.svg'), svg + '</svg>');
console.log(`Badge: ${layouts.length} production-layout cases, ${modes.length} row modes, actual alpha bbox ${[left, top, right, bottom]}, no score/row overlap.`);

const stop = between(tclient, 'static void StopOwnHelperProcesses(', '\n#endif');
assert(/StopOwnHelperProcesses\(aWideInstallDir, true\)/.test(tclient), 'Normal OnShutdown must request last-client check');
assert(/StopOwnHelperProcesses\(aWideTarget\)/.test(tclient), 'Update installation must keep default force cleanup');
const shutdownExe = compile('shutdown_probe', `#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <iterator>
#include <string>
static DWORD s_DenyOpen = 0, s_DenyQuery = 0, s_Vanished = 0;
static bool s_FailSnapshot = false;
static HANDLE TestOpenProcess(DWORD Access, BOOL Inherit, DWORD Pid)
{
 if(Pid == s_DenyOpen || Pid == s_Vanished) { SetLastError(Pid == s_Vanished ? ERROR_INVALID_PARAMETER : ERROR_ACCESS_DENIED); return nullptr; }
 return OpenProcess(Access, Inherit, Pid);
}
static BOOL TestQueryFullProcessImageNameW(HANDLE Process, DWORD Flags, LPWSTR Path, PDWORD Size)
{
 if(s_DenyQuery && GetProcessId(Process) == s_DenyQuery) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
 return QueryFullProcessImageNameW(Process, Flags, Path, Size);
}
static HANDLE TestCreateToolhelp32Snapshot(DWORD Flags, DWORD Pid)
{
 if(s_FailSnapshot) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
 return CreateToolhelp32Snapshot(Flags, Pid);
}
#define OpenProcess TestOpenProcess
#define QueryFullProcessImageNameW TestQueryFullProcessImageNameW
#define CreateToolhelp32Snapshot TestCreateToolhelp32Snapshot
${stop}
int main()
{
 wchar_t OwnPath[32768]; DWORD Size = std::size(OwnPath);
 if(!QueryFullProcessImageNameW(GetCurrentProcess(), 0, OwnPath, &Size)) return 2;
 const std::wstring Path(OwnPath, Size), InstallDir = Path.substr(0, Path.find_last_of(L"\\\\/"));
 std::cout << "READY" << std::endl;
 std::string Command;
 while(std::cin >> Command)
 {
  s_DenyOpen = s_DenyQuery = s_Vanished = 0; s_FailSnapshot = false;
  if(Command == "exit") return 0;
  if(Command == "open-denied") std::cin >> s_DenyOpen;
  if(Command == "query-denied") std::cin >> s_DenyQuery;
  if(Command == "vanished") std::cin >> s_Vanished;
  if(Command == "snapshot-failed") s_FailSnapshot = true;
  if(Command == "force") StopOwnHelperProcesses(InstallDir.c_str());
  else StopOwnHelperProcesses(InstallDir.c_str(), true);
  std::cout << "DONE" << std::endl;
 }
}
`);
const children = new Set();
const results = [];
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
function nextLine(child, text)
{
	return new Promise((resolve, reject) =>
	{
		const timer = setTimeout(() => { child.stdout.off('data', check); reject(new Error('Timed out waiting for ' + text)); }, 10000);
		function check()
		{
			const n = child.lines.indexOf(text);
			if(n < 0) return;
			child.lines.splice(n, 1); clearTimeout(timer); child.stdout.off('data', check); resolve();
		}
		child.stdout.on('data', check); check();
	});
}
async function start(exe)
{
	const child = cp.spawn(path.toNamespacedPath(exe), [], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
	child.lines = []; child.pending = ''; child.stderrText = '';
	child.stdout.on('data', data =>
	{
		child.pending += data.toString();
		const parts = child.pending.split(/\r?\n/); child.pending = parts.pop(); child.lines.push(...parts);
	});
	child.stderr.on('data', data => child.stderrText += data.toString());
	child.on('error', error => child.stderrText += error.message);
	children.add(child);
	await nextLine(child, 'READY'); return child;
}
async function send(child, command)
{
	const done = nextLine(child, 'DONE'); child.stdin.write(command + '\n'); await done;
}
async function expectDead(child)
{
	for(let i = 0; i < 100 && child.exitCode === null; ++i) await delay(20);
	assert.notEqual(child.exitCode, null, 'Expected disposable helper to terminate');
}
function expectAlive(child) { assert.equal(child.exitCode, null, 'Shared or unrelated helper terminated'); }
async function clean()
{
	for(const child of children) if(child.exitCode === null) child.kill();
	await Promise.all([...children].map(child => child.exitCode !== null ? Promise.resolve() : new Promise(resolve => child.once('exit', resolve))));
	children.clear();
}
async function scenario(name, body)
{
	try { await body(); results.push({name, pass: true}); console.log('PASS ' + name); }
	finally { await clean(); }
}
const unique = 'TClientProbe-' + process.pid + '.exe';
const installs = ['install-a', 'install-b', 'long-install/' + 'long-path-segment/'.repeat(17) + 'unicode-企鹅'].map(name => path.join(output, name));
for(const dir of installs)
{
	fs.mkdirSync(dir, {recursive: true});
	for(const name of [unique, 'island_media_monitor.exe', 'netease-watcher.exe']) fs.copyFileSync(shutdownExe, path.join(dir, name));
}
const [a, b, long] = installs;
const client = dir => start(path.join(dir, unique));
const helpers = dir => Promise.all(['island_media_monitor.exe', 'netease-watcher.exe'].map(name => start(path.join(dir, name))));
(async () =>
{
	await scenario('last client excludes itself and cleans both helpers', async () =>
	{
		const one = await client(a), hs = await helpers(a);
		await send(one, 'normal'); await Promise.all(hs.map(expectDead));
	});
	await scenario('first quitter preserves shared helpers; final quitter cleans while first PID still exists', async () =>
	{
		const one = await client(a), two = await client(a), hs = await helpers(a);
		await send(one, 'normal'); hs.forEach(expectAlive);
		await send(two, 'normal'); await Promise.all(hs.map(expectDead)); expectAlive(one);
	});
	await scenario('simultaneous exit: two and four PIDs, twelve repetitions', async () =>
	{
		for(let i = 0; i < 12; ++i)
		{
			const cs = await Promise.all(Array.from({length: i % 2 ? 4 : 2}, () => client(a)));
			const hs = await helpers(a);
			await Promise.all(cs.map(c => send(c, 'normal')));
			await Promise.all(hs.map(expectDead)); cs.forEach(expectAlive);
			await clean();
		}
	});
	await scenario('full image path isolates same-named clients and helpers in another install', async () =>
	{
		const one = await client(a), other = await client(b), ownHelpers = await helpers(a), otherHelpers = await helpers(b);
		await send(one, 'normal'); await Promise.all(ownHelpers.map(expectDead)); otherHelpers.forEach(expectAlive); expectAlive(other);
	});
	for(const fault of ['open-denied', 'query-denied']) await scenario(fault + ' conservatively preserves helpers; update default still force-cleans', async () =>
	{
		const one = await client(a), two = await client(a), hs = await helpers(a);
		await send(one, fault + ' ' + two.pid); hs.forEach(expectAlive);
		await send(one, 'force'); await Promise.all(hs.map(expectDead)); expectAlive(two);
	});
	await scenario('vanished PID race does not prevent last-client cleanup', async () =>
	{
		const one = await client(a), two = await client(a), hs = await helpers(a);
		await send(one, 'vanished ' + two.pid); await Promise.all(hs.map(expectDead));
	});
	await scenario('snapshot failure preserves helpers', async () =>
	{
		const one = await client(a), hs = await helpers(a);
		await send(one, 'snapshot-failed'); hs.forEach(expectAlive);
	});
	await scenario('long Unicode image paths beyond MAX_PATH', async () =>
	{
		assert(path.join(long, unique).length > 260);
		const one = await client(long), two = await client(long), hs = await helpers(long);
		await send(one, 'normal'); hs.forEach(expectAlive);
		await send(two, 'normal'); await Promise.all(hs.map(expectDead));
	});
	fs.writeFileSync(path.join(output, 'shutdown-results.json'), JSON.stringify({results, limits: ['Isolated production function; not a game/music playback test', 'API access/snapshot errors are injected', 'Older clients do not publish exit events']}, null, 2));
	console.log('Artifacts: ' + output);
})().catch(async error => { console.error(error.stack); await clean(); process.exitCode = 1; });
