// Run with: node scripts/run_island_music_test.js
// No CMake build, music service requests or shared build artifacts are used.
// The test seam is the complete, verbatim lyrics state machine from the current
// island_music.cpp. HTTP is a deterministic queue that requires Run() before a
// response can complete. Production LRC/YRC and JSON parsers are linked directly.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');

const root = path.resolve(__dirname, '..');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'island-music-test-'));

function between(source, start, end) {
  const first = source.indexOf(start);
  const last = source.indexOf(end, first + start.length);
  if (first < 0 || last < 0) throw new Error(`Production test seam not found: ${start}`);
  return source.slice(first, last);
}

function run(executable, args, env = process.env) {
  const result = cp.spawnSync(executable, args, { cwd: out, env, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    process.stdout.write(result.stdout || '');
    process.stderr.write(result.stderr || '');
    throw new Error(`${path.basename(executable)} exited with ${result.status}; artifacts: ${out}`);
  }
  return result.stdout || '';
}

try {
  const source = fs.readFileSync(path.join(root, 'src/game/client/components/tclient/island_music.cpp'), 'utf8').replace(/\r\n/g, '\n');
  const track = between(source, '\tstruct STrack\n', '\t// ---------------- SMTC worker').trimEnd();
  const lyrics = between(source, '\t// ---------------- lyrics', '\tint FindLineIndex(');
  const jsonSource = fs.readFileSync(path.join(root, 'src/engine/shared/json.cpp'), 'utf8');
  const jsonAccessors = between(jsonSource, 'const struct _json_value *json_object_get', 'static char EscapeJsonChar');
  fs.writeFileSync(path.join(out, 'island_music_test_impl.h'), `
${jsonAccessors}
class CIslandMusicTest
{
public:
${track}
  IHttp *m_pHttp = nullptr;
  static constexpr size_t COVER_MAX_BYTES = 16u * 1024u * 1024u;
  struct SIslandShm { double ClientDurationMs; };
  void *m_pIslandView = nullptr;
  std::mutex m_Mutex;
  std::vector<unsigned char> m_CoverPng;
  std::string m_CoverKey;
  bool m_CoverDirty = false;
${lyrics}
};
`);
  const testSource = path.join(__dirname, 'island_music_test.cpp');
  const parser = path.join(root, 'src/game/client/components/tclient/lyric_parser.cpp');
  const jsonParser = path.join(root, 'src/engine/external/json-parser/json.c');
  let binary;
  if (process.platform === 'win32') {
    const vswhere = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Microsoft Visual Studio/Installer/vswhere.exe');
    const installation = cp.spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' });
    if (installation.status !== 0 || !installation.stdout.trim()) throw new Error('MSVC Build Tools not found');
    const tools = path.join(installation.stdout.trim(), 'VC/Tools/MSVC');
    const msvc = path.join(tools, fs.readdirSync(tools).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop());
    const kits = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Windows Kits/10');
    const version = fs.readdirSync(path.join(kits, 'Include')).filter(v => /^10\./.test(v)).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop();
    const includes = [path.join(msvc, 'include'), ...['ucrt', 'shared', 'um', 'winrt'].map(n => path.join(kits, 'Include', version, n))];
    const libraries = [path.join(msvc, 'lib/x64'), ...['ucrt', 'um'].map(n => path.join(kits, 'Lib', version, n, 'x64'))];
    const compiler = path.join(msvc, 'bin/Hostx64/x64/cl.exe');
    const env = { ...process.env, INCLUDE: includes.join(';'), LIB: libraries.join(';') };
    const jsonObject = path.join(out, 'json.obj');
    run(compiler, ['/nologo', '/c', '/TC', '/W3', '/utf-8', jsonParser, `/Fo${jsonObject}`], env);
    binary = path.join(out, 'island_music_test.exe');
    run(compiler, ['/nologo', '/std:c++17', '/EHsc', '/W3', '/utf-8', '/DCONF_FAMILY_WINDOWS', `/I${path.join(root, 'src')}`, `/I${out}`, testSource, parser, jsonObject, `/Fe${binary}`], env);
  } else {
    binary = path.join(out, 'island_music_test');
    const jsonObject = path.join(out, 'json.o');
    run(process.env.CC || 'cc', ['-c', jsonParser, '-o', jsonObject]);
    run(process.env.CXX || 'c++', ['-std=c++17', '-DCONF_FAMILY_WINDOWS', '-I', path.join(root, 'src'), '-I', out, testSource, parser, jsonObject, '-o', binary]);
  }
  process.stdout.write(run(binary, []));
  console.log('PASS: isolated offline tests used the current production lyrics state machine');
  fs.rmSync(out, { recursive: true, force: true });
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
