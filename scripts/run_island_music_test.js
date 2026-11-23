// Run with: node scripts/run_island_music_test.js
// No CMake build, music service requests or shared build artifacts are used.
// The seam contains the verbatim production lyrics state machine and IJob.
// HTTP and job scheduling are deterministic; parsers run on real worker threads.
// Set ISLAND_MUSIC_TEST_KEEP=1 to retain the fresh temp directory and evidence.
const fs = require('node:fs');
const crypto = require('node:crypto');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const { discoverCompiler, compileCommands } = require('./test_compiler');

const root = path.resolve(__dirname, '..');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'island-music-test-'));

function between(source, start, end) {
  const first = source.indexOf(start);
  const last = source.indexOf(end, first + start.length);
  if (first < 0 || last < 0) throw new Error(`Production test seam not found: ${start}`);
  return source.slice(first, last);
}

function run(executable, args, env = process.env) {
  const result = cp.spawnSync(executable, args, { cwd: out, env, encoding: 'utf8', timeout: 120000, maxBuffer: 4 * 1024 * 1024 });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    process.stdout.write(result.stdout || '');
    process.stderr.write(result.stderr || '');
    throw new Error(`${path.basename(executable)} exited with ${result.status}; artifacts: ${out}`);
  }
  return result.stdout || '';
}

try {
  const inputs = [];
  function readSource(relative) {
    const file = path.join(root, relative);
    const bytes = fs.readFileSync(file);
    inputs.push({ path: file, sha256: crypto.createHash('sha256').update(bytes).digest('hex') });
    return bytes.toString('utf8').replace(/\r\n/g, '\n');
  }
  const source = readSource('src/game/client/components/tclient/island_music.cpp');
  const track = between(source, '\tstruct STrack\n', '\t// ---------------- SMTC worker').trimEnd();
  const lyrics = between(source, '\t// ---------------- lyrics', '\tint FindLineIndex(');
  const findLineIndex = between(source, '\tint FindLineIndex(', '\tint FindNextLineIndex(').trimEnd();
  const goneProtocol = between(source, '	// ---------------- Gone v2 protocol ----------------', '	// ---------------- Gone v2 protocol end ----------------');
  const widthCache = between(source, '\tclass CTextWidthCacheEntry', '\t// vinyl record spin angle');
  const measureCached = between(source, '\tfloat MeasureCached(', '\t// TClient: game round timer');
  const coverLimit = source.match(/static constexpr size_t COVER_MAX_BYTES[^;]*;/)?.[0];
  if (!coverLimit) throw new Error('Production cover limit not found');
  const jsonSource = readSource('src/engine/shared/json.cpp');
  const jsonAccessors = between(jsonSource, 'const struct _json_value *json_object_get', 'static char EscapeJsonChar');
  const jobHeader = readSource('src/engine/shared/jobs.h');
  const jobSource = readSource('src/engine/shared/jobs.cpp');
  fs.writeFileSync(path.join(out, 'island_music_test_job.h'),
    between(jobHeader, 'class IJob\n', 'class CJobPool\n') +
    between(jobSource, 'IJob::IJob()', 'CJobPool::CJobPool()'));
  fs.writeFileSync(path.join(out, 'island_music_test_impl.h'), `
${jsonAccessors}
class CIslandMusicTest
{
public:
${track}
${goneProtocol}
${widthCache}
${measureCached}
${findLineIndex}
  IHttp *m_pHttp = nullptr;
  IEngine *m_pEngine = nullptr;
  ${coverLimit}
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
  for (const file of [testSource, parser, jsonParser]) readSource(path.relative(root, file));
  fs.writeFileSync(path.join(out, 'production-seam.json'), JSON.stringify({ inputs }, null, 2));
  const toolchain = discoverCompiler();
  const { binary, commands } = compileCommands(toolchain, {
    out, binaryName: 'island_music_test', includes: [path.join(root, 'src'), out],
    sources: [
      { file: jsonParser, language: 'c' },
      // Exercise the simulated provider everywhere, but never leak its macro
      // into the real parser's non-Windows translation unit.
      { file: testSource, defines: ['CONF_FAMILY_WINDOWS'] },
      { file: parser, defines: process.platform === 'win32' ? ['CONF_FAMILY_WINDOWS'] : [] },
    ],
  });
  fs.writeFileSync(path.join(out, 'compiler-commands.json'), JSON.stringify(commands.map(({ executable, args }) => ({ executable, args })), null, 2));
  for (const command of commands) run(command.executable, command.args, command.env);
  const output = run(binary, []);
  fs.writeFileSync(path.join(out, 'test-output.log'), output);
  process.stdout.write(output);
  console.log('PASS: isolated offline tests used the current production async lyrics state machine and IJob');
  console.log(`Production SHA-256: ${inputs[0].sha256}`);
  if (process.env.ISLAND_MUSIC_TEST_KEEP === '1') console.log(`Artifacts: ${out}`);
  else fs.rmSync(out, { recursive: true, force: true });
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
