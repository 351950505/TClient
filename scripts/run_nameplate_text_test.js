// Offline, isolated production-source differential regression.
// Run: node scripts/run_nameplate_text_test.js
// NAMEPLATE_TEXT_TEST_KEEP=1 preserves source hashes and compiler/test evidence.
// No shared CMake directory, network, or real client process is used.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '..');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'nameplate-text-test-'));
const inputs = [];
function read(relative) {
  const file = path.join(root, relative);
  const bytes = fs.readFileSync(file);
  inputs.push({ path: file, sha256: crypto.createHash('sha256').update(bytes).digest('hex') });
  return bytes.toString('utf8').replace(/\r\n/g, '\n');
}
function between(s, a, b) {
  const first = s.indexOf(a), last = s.indexOf(b, first + a.length);
  if (first < 0 || last < 0) throw new Error(`Production seam not found: ${a}`);
  return s.slice(first, last);
}
function method(s, signature) {
  const first = s.indexOf(signature);
  if (first < 0) throw new Error(`Production function not found: ${signature}`);
  const body = s.indexOf('\n\t{', first);
  const end = s.indexOf('\n\t}', body + 3);
  if (body < 0 || end < 0) throw new Error(`Production method boundary missing: ${signature}`);
  return s.slice(first, end + 3);
}
function run(executable, args, env = process.env) {
  const result = cp.spawnSync(executable, args, { cwd: out, env, encoding: 'utf8', timeout: 120000, maxBuffer: 4 * 1024 * 1024 });
  fs.appendFileSync(path.join(out, 'command-output.log'), `${path.basename(executable)}\n${result.stdout || ''}${result.stderr || ''}\n`);
  if (result.error) throw result.error;
  if (result.status !== 0) {
    process.stdout.write((result.stdout || '').split(/\r?\n/).slice(0, 45).join('\n') + '\n');
    process.stderr.write((result.stderr || '').split(/\r?\n/).slice(0, 15).join('\n') + '\n');
    throw new Error(`${path.basename(executable)} exited ${result.status}; evidence: ${out}`);
  }
  return result.stdout || '';
}
try {
  const source = read('src/engine/client/text.cpp');
  const types = between(read('src/engine/textrender.h'), 'enum\n{\n\tTEXTFLAG_RENDER', 'class ITextRender');
  fs.writeFileSync(path.join(out, 'nameplate_text_types.h'), types);
  const strings = read('src/base/str.cpp');
  function freeFunction(s, signature) {
    const first = s.indexOf(signature), body = s.indexOf('\n{', first), end = s.indexOf('\n}', body + 2);
    if (first < 0 || body < 0 || end < 0) throw new Error(`Production function missing: ${signature}`);
    return s.slice(first, end + 2);
  }
  fs.writeFileSync(path.join(out, 'nameplate_text_strings.h'), freeFunction(strings, 'static unsigned char str_byte_next(') + '\n' + freeFunction(strings, 'static void str_byte_rewind(') + '\n' + freeFunction(strings, 'int str_utf8_decode(') + '\n' + freeFunction(strings, 'int str_utf8_rewind('));
  fs.writeFileSync(path.join(out, 'nameplate_text_screen.h'), between(read('src/engine/graphics.h'), 'class CScreenRect', 'class IGraphics'));
  const glyphs = between(source, '// TClient\nstatic void ReplaceHyphensWithSpaces', '\tvoid UploadEntityLayerText(') +
    method(source, '\tsize_t TextureDimension() const') + '\n' + method(source, '\tIGraphics::CTextureHandle Texture(size_t TextureIndex) const') + '\n};\n';
  fs.writeFileSync(path.join(out, 'nameplate_text_glyphs.h'), glyphs);
  const containers = between(source, 'typedef vector4_base<unsigned char> STextCharQuadVertexColor;', 'struct SFontLanguageVariant');
  const methods = between(source, '\tvoid TextEx(', '\tvoid UploadEntityLayerText(').replace(/ override\b/g, '');
  fs.writeFileSync(path.join(out, 'nameplate_text_impl.h'), containers + `
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
${methods}
};
`);
  const vk = read('src/engine/client/backend/vulkan/backend_vulkan.cpp');
  const vkStructs = between(vk, '\tstruct SBufferObject\n', '\tstruct SFrameUniformBuffers') + between(vk, '\ttemplate<typename TName>\n\tstruct SStreamMemory', '\tstruct SShaderModule');
  const vkMethods = method(vk, '\t[[nodiscard]] bool CreateBufferObject(size_t BufferIndex,') + '\n' + method(vk, '\tvoid DeleteBufferObject(size_t BufferIndex)') + '\n' + between(vk, '\ttemplate<bool FlushForRendering, typename TName>', '\tvoid CleanBufferPair(') + '\n' + between(vk, '\ttemplate<typename TStreamMemName, typename TInstanceTypeName', '\ttemplate<typename TName, size_t InstanceMaxParticleCount') + '\n' + method(vk, '\tvoid BufferContainer_FillExecuteBuffer(');
  fs.writeFileSync(path.join(out, 'nameplate_text_vulkan.h'), vkStructs + '\nusing TNewMemFunc = std::function<bool(SFrameBuffers &, VkBuffer, VkDeviceSize)>;\n' + vkMethods);
  const gradient = read('src/game/client/components/tclient/gradient_math.cpp');
  const gradientDeclarations = between(read('src/game/client/components/tclient/gradient_math.h'), 'namespace TClientGradient', '#endif');
  fs.writeFileSync(path.join(out, 'nameplate_text_gradient.h'), gradientDeclarations + '\n' + between(gradient, 'namespace\n{', 'void TClientGradient::ApplySixupColors'));
  const names = read('src/game/client/components/nameplates.cpp');
  const plateClass = between(names, 'class CNamePlate\n', 'class CNamePlates::CNamePlatesData');
  const realInit = method(plateClass, '\tvoid Init(');
  const fixtureInit = '\tvoid Init(CGameClient &This)\n\t{\n\t\tif(m_Inited) return;\n\t\tm_Inited = true;\n\t\tAddPart<CNamePlatePartFriendMark>(This);\n\t\tAddPart<CNamePlatePartClientId>(This, false);\n\t\tAddPart<CNamePlatePartName>(This);\n\t\tAddPart<CNamePlatePartNewLine>(This);\n\t\tAddPart<CNamePlatePartClan>(This);\n\t\tAddPart<CNamePlatePartNewLine>(This);\n\t}';
  const newline = between(names, 'class CNamePlatePartNewLine', '// ***** Name Plates *****');
  // Only this part's class is required; later declaration ordering is preserved.
  const newlineEnd = newline.indexOf('\n};') + 4;
  if (newlineEnd < 4) throw new Error('Newline seam missing');
  fs.writeFileSync(path.join(out, 'nameplate_text_nameplates.h'),
    between(names, 'enum class EHookStrongWeakState', 'class CNamePlatePartIcon') +
    between(names, 'class CNamePlatePartClientId', 'class CNamePlatePartHookStrongWeak') +
    newline.slice(0, newlineEnd) + '\n' + plateClass.replace(realInit, fixtureInit));
  read('scripts/nameplate_text_test.cpp');
  for (const f of ['data/fonts/DejaVuSans.ttf', 'data/fonts/GlowSansJ-Compressed-Book.otf', 'data/fonts/SourceHanSans.ttc']) read(f);
  fs.writeFileSync(path.join(out, 'production-seam.json'), JSON.stringify({ inputs, stubs: ['graphics GPU command recording', 'game time/config/war list', 'CNamePlate::Init unrelated parts omitted', 'container allocation', 'string wrappers'] }, null, 2));
  let binary;
  const args = ['-std=c++20', '-I', path.join(root, 'src'), '-I', out, '-I', path.join(root, 'ddnet-libs/freetype/include'), path.join(__dirname, 'nameplate_text_test.cpp')];
  if (process.platform === 'win32') {
    const vswhere = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Microsoft Visual Studio/Installer/vswhere.exe');
    const found = cp.spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' });
    if (found.status !== 0 || !found.stdout.trim()) throw new Error('MSVC Build Tools not found');
    const tools = path.join(found.stdout.trim(), 'VC/Tools/MSVC');
    const msvc = path.join(tools, fs.readdirSync(tools).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop());
    const kits = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Windows Kits/10');
    const version = fs.readdirSync(path.join(kits, 'Include')).filter(v => /^10\./.test(v)).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop();
    const env = { ...process.env, INCLUDE: [path.join(msvc, 'include'), ...['ucrt', 'shared', 'um', 'winrt'].map(n => path.join(kits, 'Include', version, n))].join(';'), LIB: [path.join(msvc, 'lib/x64'), ...['ucrt', 'um'].map(n => path.join(kits, 'Lib', version, n, 'x64'))].join(';'), VSLANG: '1033' };
    binary = path.join(out, 'nameplate_text_test.exe');
    const ft = path.join(root, 'ddnet-libs/freetype/windows/lib64');
    fs.copyFileSync(path.join(ft, 'libfreetype.dll'), path.join(out, 'libfreetype.dll'));
    run(path.join(msvc, 'bin/Hostx64/x64/cl.exe'), ['/nologo', '/std:c++20', '/EHsc', '/W3', '/utf-8', `/I${path.join(root, 'src')}`, `/I${out}`, `/I${path.join(root, 'ddnet-libs/freetype/include')}`, path.join(__dirname, 'nameplate_text_test.cpp'), path.join(ft, 'freetype.lib'), `/Fe${binary}`], env);
  } else {
    binary = path.join(out, 'nameplate_text_test');
    run(process.env.CXX || 'c++', [...args, '-lfreetype', '-o', binary]);
  }
  const output = run(binary, [root]);
  fs.writeFileSync(path.join(out, 'test-output.log'), output);
  process.stdout.write(output);
  console.log(`Production text SHA-256: ${inputs[0].sha256}`);
  if (process.env.NAMEPLATE_TEXT_TEST_KEEP === '1') console.log(`Evidence: ${out}`);
  else fs.rmSync(out, { recursive: true, force: true });
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
