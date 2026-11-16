// Offline production-source regression for RT-7/8/9. No CMake/shared build/client/network.
// Run: node scripts/run_rt789_test.js [source-root]
// RT789_KEEP=1 preserves compiler/test/source-hash evidence in a unique TEMP directory.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const baseline = process.argv.includes('--baseline');
const root = path.resolve(process.argv.slice(2).find(a => a !== '--baseline') || path.join(__dirname, '..'));
const baselineTargets = new Set(['src/engine/client/text.cpp', 'src/game/client/components/tclient/gradient.cpp', 'src/game/map/envelope_extrema.cpp', 'src/game/map/envelope_extrema.h']);
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'rt789-test-'));
const inputs = [];
function read(relative) {
  const file = path.join(root, relative);
  const fromHead = baseline && baselineTargets.has(relative);
  const bytes = fromHead ? cp.execFileSync('git', ['-C', root, 'show', `HEAD:${relative}`]) : fs.readFileSync(file);
  inputs.push({ path: file, fromHead, sha256: crypto.createHash('sha256').update(bytes).digest('hex') });
  return bytes.toString('utf8').replace(/\r\n/g, '\n');
}
function between(s, a, b) {
  const first = s.indexOf(a), last = s.indexOf(b, first + a.length);
  if (first < 0 || last < 0) throw new Error(`Production boundary missing: ${a}`);
  return s.slice(first, last);
}
function method(s, signature) {
  const first = s.indexOf(signature), body = s.indexOf('\n\t{', first), end = s.indexOf('\n\t}', body + 3);
  if (first < 0 || body < 0 || end < 0) throw new Error(`Production method missing: ${signature}`);
  return s.slice(first, end + 3);
}
function freeFunction(s, signature) {
  const first = s.indexOf(signature), body = s.indexOf('\n{', first), end = s.indexOf('\n}', body + 2);
  if (first < 0 || body < 0 || end < 0) throw new Error(`Production function missing: ${signature}`);
  return s.slice(first, end + 2);
}
function write(name, text) { fs.writeFileSync(path.join(out, name), text); }
function run(executable, args, env = process.env) {
  const result = cp.spawnSync(executable, args, { cwd: out, env, encoding: 'utf8', timeout: 120000, maxBuffer: 4 * 1024 * 1024 });
  fs.appendFileSync(path.join(out, 'command-output.log'), `${path.basename(executable)}\n${result.stdout || ''}${result.stderr || ''}\n`);
  if (result.error) throw result.error;
  if (result.status !== 0) {
    process.stdout.write((result.stdout || '').split(/\r?\n/).slice(0, 30).join('\n') + '\n');
    process.stderr.write((result.stderr || '').split(/\r?\n/).slice(0, 15).join('\n') + '\n');
    throw new Error(`${path.basename(executable)} exited ${result.status}; evidence ${out}`);
  }
  return result.stdout;
}
try {
  const source = read('src/engine/client/text.cpp');
  write('nameplate_text_types.h', between(read('src/engine/textrender.h'), 'enum\n{\n\tTEXTFLAG_RENDER', 'class ITextRender'));
  const strings = read('src/base/str.cpp');
  write('nameplate_text_strings.h', ['static unsigned char str_byte_next(', 'static void str_byte_rewind(', 'int str_utf8_decode(', 'int str_utf8_rewind('].map(s => freeFunction(strings, s)).join('\n'));
  write('nameplate_text_screen.h', between(read('src/engine/graphics.h'), 'class CScreenRect', 'class IGraphics'));
  write('nameplate_text_glyphs.h', between(source, '// TClient\nstatic void ReplaceHyphensWithSpaces', '\tvoid UploadEntityLayerText(') + method(source, '\tsize_t TextureDimension() const') + '\n' + method(source, '\tIGraphics::CTextureHandle Texture(size_t TextureIndex) const') + '\n};\n');
  const containers = between(source, 'typedef vector4_base<unsigned char> STextCharQuadVertexColor;', 'struct SFontLanguageVariant');
  write('nameplate_text_impl.h', containers + `
class CTextRender
{
public:
  IGraphics *m_pGraphics;
  CGlyphMap *m_pGlyphMap;
  unsigned m_RenderFlags = 0;
  ColorRGBA m_Color = ColorRGBA(1, 1, 1, 1);
  ColorRGBA m_SelectionColor = ColorRGBA(1, 1, 1, 0.5f);
  FTextColorSplitCallback m_pfnTextColorSplitCallback = nullptr;
  void *m_pTextColorSplitCallbackUserData = nullptr;
  std::chrono::nanoseconds m_CursorRenderTime{};
  SBufferContainerInfo m_DefaultTextContainerInfo;
  std::vector<std::unique_ptr<STextContainer>> m_Containers;
  CTextRender(IGraphics *pGraphics, CGlyphMap *pGlyphMap) : m_pGraphics(pGraphics), m_pGlyphMap(pGlyphMap) {}
  IGraphics *Graphics() { return m_pGraphics; }
  ColorRGBA GetTextColor() const { return m_Color; }
  ColorRGBA DefaultTextColor() const { return ColorRGBA(1, 1, 1, 1); }
  ColorRGBA DefaultTextOutlineColor() const { return ColorRGBA(0, 0, 0, 0.3f); }
  void SetRenderFlags(unsigned Flags) { m_RenderFlags = Flags; }
  unsigned GetRenderFlags() const { return m_RenderFlags; }
  void SetFontPreset(EFontPreset Preset) { m_pGlyphMap->SetFontPreset(Preset); }
  int GetFreeTextContainerIndex() { m_Containers.emplace_back(std::make_unique<STextContainer>()); return (int)m_Containers.size() - 1; }
  STextContainer &GetTextContainer(STextContainerIndex Index) { return *m_Containers.at(Index.m_Index); }
  void FreeTextContainer(STextContainerIndex &Index) { GetTextContainer(Index).Reset(); Index.Reset(); }
  ${method(source, '\tint WordLength(')}
  float TextWidth(float FontSize, const char *pText) { CTextCursor Cursor; Cursor.m_FontSize = FontSize; Cursor.m_Flags = 0; TextEx(&Cursor, pText); return Cursor.m_LongestLineWidth; }
  ${between(source, '\tvoid TextEx(', '\tvoid UploadEntityLayerText(').replace(/ override\b/g, '')}
};
`);
  // Borrow the established real-FreeType/record-only graphics seam without changing its files/tests.
  const vk = read('src/engine/client/backend/vulkan/backend_vulkan.cpp');
  const vkStructs = between(vk, '\tstruct SBufferObject\n', '\tstruct SFrameUniformBuffers') + between(vk, '\ttemplate<typename TName>\n\tstruct SStreamMemory', '\tstruct SShaderModule');
  const vkMethods = method(vk, '\t[[nodiscard]] bool CreateBufferObject(size_t BufferIndex,') + '\n' + method(vk, '\tvoid DeleteBufferObject(size_t BufferIndex)') + '\n' + between(vk, '\ttemplate<bool FlushForRendering, typename TName>', '\tvoid CleanBufferPair(') + '\n' + between(vk, '\ttemplate<typename TStreamMemName, typename TInstanceTypeName', '\ttemplate<typename TName, size_t InstanceMaxParticleCount') + '\n' + method(vk, '\tvoid BufferContainer_FillExecuteBuffer(');
  write('nameplate_text_vulkan.h', vkStructs + '\nusing TNewMemFunc = std::function<bool(SFrameBuffers &, VkBuffer, VkDeviceSize)>;\n' + vkMethods);
  const fixture = read('scripts/nameplate_text_test.cpp');
  let support = fixture.slice(0, fixture.indexOf('class CGameClient;'));
  if (support.length === 0) throw new Error('Existing text fixture seam missing');
  support = support.replace('int m_TcGradientClan = 1;', 'int m_TcGradientClan = 1;\n\tint m_TcGradientEverything = 0;');
  write('rt789_support.h', support);
  write('nameplate_text_gradient.h', between(read('src/game/client/components/tclient/gradient_math.h'), 'namespace TClientGradient', '#endif') + '\n' + between(read('src/game/client/components/tclient/gradient_math.cpp'), 'namespace\n{', 'void TClientGradient::ApplySixupColors'));
  write('rt789_gradient.h', freeFunction(read('src/game/client/components/tclient/gradient.cpp'), 'void CTClientGradient::ApplyEverythingGradient('));
  const renderMap = read('src/game/map/render_map.cpp');
  const renderTypes = read('src/game/map/render_map.h');
  write('rt789_envelope_types.h', '#include <game/mapitems.h>\n' + between(renderTypes, 'class IEnvelopePointAccess', 'class IGraphics;') + '\nclass CRenderMap { public: static void RenderEvalEnvelope(const IEnvelopePointAccess *, std::chrono::nanoseconds, ColorRGBA &, size_t); };\n' + read('src/game/map/envelope_extrema.h').replace('#include <game/map/render_map.h>', '') + '\nenum {\n#define UUID(id, name) id,\n' + read('src/game/mapitems_ex_types.h') + '\n#undef UUID\n};\n');
  write('rt789_envelope_impl.h', between(renderMap, 'int IEnvelopePointAccess::FindPointIndex(', 'void CRenderMap::Init(') + freeFunction(renderMap, 'void CRenderMap::RenderEvalEnvelope(') + '\n' + read('src/game/map/envelope_extrema.cpp').replace('#include "envelope_extrema.h"', ''));
  write('rt789_clipping.h', freeFunction(read('src/game/map/render_layer.cpp'), 'bool CRenderLayerQuads::CalculateQuadClipping('));
  read('scripts/rt789_test.cpp');
  fs.copyFileSync(path.join(__dirname, 'rt789_test.cpp'), path.join(out, 'rt789_test.cpp'));
  write('production-seam.json', JSON.stringify({ root, inputs, stubs: ['record-only graphics (no GPU)', 'gradient game clock/config/client id', 'gradient mode selection: real custom split builder', 'text container allocation', 'memory-only IMap fixture'] }, null, 2));
  let binary;
  if (process.platform === 'win32') {
    const pf = process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)';
    const found = cp.spawnSync(path.join(pf, 'Microsoft Visual Studio/Installer/vswhere.exe'), ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' });
    if (found.status !== 0 || !found.stdout.trim()) throw new Error('MSVC Build Tools not found');
    const tools = path.join(found.stdout.trim(), 'VC/Tools/MSVC');
    const msvc = path.join(tools, fs.readdirSync(tools).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop());
    const kits = path.join(pf, 'Windows Kits/10');
    const version = fs.readdirSync(path.join(kits, 'Include')).filter(v => /^10\./.test(v)).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop();
    const env = { ...process.env, INCLUDE: [path.join(msvc, 'include'), ...['ucrt', 'shared', 'um', 'winrt'].map(n => path.join(kits, 'Include', version, n))].join(';'), LIB: [path.join(msvc, 'lib/x64'), ...['ucrt', 'um'].map(n => path.join(kits, 'Lib', version, n, 'x64'))].join(';'), VSLANG: '1033' };
    const ft = path.join(root, 'ddnet-libs/freetype/windows/lib64');
    binary = path.join(out, 'rt789_test.exe');
    fs.copyFileSync(path.join(ft, 'libfreetype.dll'), path.join(out, 'libfreetype.dll'));
    run(path.join(msvc, 'bin/Hostx64/x64/cl.exe'), ['/nologo', '/std:c++20', '/EHsc', '/W3', '/utf-8', `/I${path.join(root, 'src')}`, `/I${out}`, `/I${path.join(root, 'ddnet-libs/freetype/include')}`, path.join(out, 'rt789_test.cpp'), path.join(ft, 'freetype.lib'), `/Fe${binary}`], env);
  } else {
    binary = path.join(out, 'rt789_test');
    run(process.env.CXX || 'c++', ['-std=c++20', '-I', path.join(root, 'src'), '-I', out, '-I', path.join(root, 'ddnet-libs/freetype/include'), path.join(out, 'rt789_test.cpp'), '-lfreetype', '-o', binary]);
  }
  const output = run(binary, [root]);
  write('test-output.log', output);
  process.stdout.write(output);
  console.log(`Evidence: ${out}`);
  if (process.env.RT789_KEEP !== '1') fs.rmSync(out, { recursive: true, force: true });
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
