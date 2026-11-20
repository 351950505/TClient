// Offline regression using the production viewport, readback and screenshot methods.
// Usage: node scripts/test_gl_presented_readback.js [--baseline <git-ref>]
'use strict';
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');
const { discoverCompiler, compileCommands } = require('./test_compiler');
const root = path.resolve(__dirname, '..');
const sourcePath = 'src/engine/client/backend/opengl/backend_opengl.cpp';
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'gl-presented-readback-'));

function method(source, name, owner = 'CCommandProcessorFragment_OpenGL') {
  const signature = owner ? `${owner}::${name}(` : `${name}(`;
  const start = source.indexOf(signature);
  if (start < 0) throw new Error(`Missing production method: ${name}`);
  const line = source.lastIndexOf('\n', start) + 1;
  const open = source.indexOf('{', start);
  let depth = 1, end = open + 1;
  for (; end < source.length && depth; ++end) {
    if (source[end] === '{') ++depth;
    if (source[end] === '}') --depth;
  }
  if (depth) throw new Error(`Unbalanced production method: ${name}`);
  return source.slice(line, end);
}

const harness = `
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using GLint = int;
constexpr int GL_PACK_ALIGNMENT = 1, GL_FRONT = 2, GL_RGBA = 3, GL_UNSIGNED_BYTE = 4, GL_VIEWPORT = 5;
static int s_Width, s_Height, s_Alignment = 4, s_X, s_Y, s_ReadWidth, s_ReadHeight;
static int s_aViewport[4];
static void Require(bool Condition, const char *pMessage) {
  if (!Condition) throw std::runtime_error(pMessage);
}
static void glViewport(int X, int Y, int W, int H) {
  s_aViewport[0] = X; s_aViewport[1] = Y; s_aViewport[2] = W; s_aViewport[3] = H;
}
static void glReadBuffer(int) {}
static void glGetIntegerv(int Name, GLint *pOut) {
  if (Name == GL_VIEWPORT) std::memcpy(pOut, s_aViewport, sizeof(s_aViewport));
  else { Require(Name == GL_PACK_ALIGNMENT, "unexpected GL query"); *pOut = s_Alignment; }
}
static void glPixelStorei(int Name, int Value) {
  Require(Name == GL_PACK_ALIGNMENT, "unexpected pixel-store query"); s_Alignment = Value;
}
static void glReadPixels(int X, int Y, int W, int H, int, int, void *pData) {
  Require(X >= 0 && Y >= 0 && W >= 0 && H >= 0 && X + W <= s_Width && Y + H <= s_Height,
    "readback rectangle exceeds the framebuffer");
  s_X = X; s_Y = Y; s_ReadWidth = W; s_ReadHeight = H;
  auto *p = static_cast<uint8_t *>(pData);
  for (int Row = 0; Row < H; ++Row) for (int Col = 0; Col < W; ++Col) {
    const size_t I = (static_cast<size_t>(Row) * W + Col) * 4;
    p[I] = static_cast<uint8_t>(X + Col); p[I + 1] = static_cast<uint8_t>(Y + Row);
    p[I + 2] = 7; p[I + 3] = 255;
  }
}
static void mem_copy(void *pDst, const void *pSrc, size_t Size) { std::memcpy(pDst, pSrc, Size); }
struct CImageInfo {
  enum EImageFormat { FORMAT_RGBA };
  int m_Width = 0, m_Height = 0;
  EImageFormat m_Format = FORMAT_RGBA;
  uint8_t *m_pData = nullptr;
  void Allocate() { m_pData = static_cast<uint8_t *>(std::calloc(static_cast<size_t>(m_Width) * m_Height, 4)); }
  ~CImageInfo() { std::free(m_pData); }
};
struct CCommandBuffer {
  struct SCommand_Update_Viewport {
    int m_X, m_Y, m_Width, m_Height, m_DrawableWidth, m_DrawableHeight;
    bool m_ByResize;
  };
  struct SCommand_TrySwapAndScreenshot { CImageInfo *m_pImage; };
};
class CCommandProcessorFragment_OpenGL {
public:
  uint32_t m_CanvasWidth = 0, m_CanvasHeight = 0;
  int m_ViewportX = 0, m_ViewportY = 0, m_ViewportWidth = 0, m_ViewportHeight = 0;
  bool m_HasDisplayCutout = false;
  void Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *pCommand);
  bool GetPresentedImageData(uint32_t &Width, uint32_t &Height, CImageInfo::EImageFormat &Format, std::vector<uint8_t> &vData);
  void Cmd_Screenshot(const CCommandBuffer::SCommand_TrySwapAndScreenshot *pCommand);
};
#include "production.inc"
int main() {
  try {
    CCommandProcessorFragment_OpenGL Backend;
    uint32_t Width = 99, Height = 99;
    CImageInfo::EImageFormat Format = CImageInfo::FORMAT_RGBA;
    std::vector<uint8_t> Data;
    Require(!Backend.GetPresentedImageData(Width, Height, Format, Data), "uninitialized canvas must refuse capture");
    struct SCase { const char *m_pName; int m_W, m_H, m_X, m_Y, m_ViewW, m_ViewH; };
    const SCase aCases[] = {
      {"no bars", 192, 108, 0, 0, 192, 108},
      {"pillarbox after resize", 192, 108, 24, 0, 144, 108},
      {"letterbox after resize", 144, 108, 0, 13, 144, 82},
      {"HiDPI pillarbox", 384, 216, 48, 0, 288, 216},
      {"odd-height canvas", 91, 67, 7, 5, 77, 57},
      {"asymmetric display cutout", 240, 108, 11, 0, 224, 108},
    };
    for (const auto &Case : aCases) {
      s_Width = Case.m_W; s_Height = Case.m_H;
      const CCommandBuffer::SCommand_Update_Viewport Viewport{
        Case.m_X, Case.m_Y, Case.m_ViewW, Case.m_ViewH, Case.m_W, Case.m_H, true};
      Backend.Cmd_Update_Viewport(&Viewport);
      Require(Backend.GetPresentedImageData(Width, Height, Format, Data), "capture failed");
      Require(Width == static_cast<uint32_t>(Case.m_W) && Height == static_cast<uint32_t>(Case.m_H), "capture must contain the full drawable canvas");
      Require(s_X == 0 && s_Y == 0 && s_ReadWidth == Case.m_W && s_ReadHeight == Case.m_H, "full canvas origin must be zero");
      Require(s_Alignment == 4, "pack alignment was not restored");
      for (uint32_t Y = 0; Y < Height; ++Y) for (uint32_t X = 0; X < Width; ++X) {
        const size_t I = (static_cast<size_t>(Y) * Width + X) * 4;
        Require(Data[I] == static_cast<uint8_t>(X) && Data[I + 1] == static_cast<uint8_t>(Height - 1 - Y), "full canvas content or row flip is incorrect");
      }
      CImageInfo Image;
      const CCommandBuffer::SCommand_TrySwapAndScreenshot Screenshot{&Image};
      Backend.Cmd_Screenshot(&Screenshot);
      Require(Image.m_Width == Case.m_ViewW && Image.m_Height == Case.m_ViewH, "ordinary screenshot must remain viewport-sized");
      Require(s_X == Case.m_X && s_Y == Case.m_H - Case.m_Y - Case.m_ViewH, "ordinary screenshot viewport offset changed");
      Require(s_Alignment == 4, "screenshot pack alignment was not restored");
      std::cout << "PASS: " << Case.m_pName << " full canvas + cropped screenshot\\n";
    }
    std::cout << "RESULT 7/7 scenarios passed\\n";
    return 0;
  } catch (const std::exception &Error) { std::cerr << "FAIL: " << Error.what() << "\\n"; return 1; }
}
`;

function runSource(source, name, expectFailure) {
  const directory = path.join(out, name);
  fs.mkdirSync(directory);
  const methods = ['Cmd_Update_Viewport', 'GetPresentedImageData', 'Cmd_Screenshot'].map(n => method(source, n)).join('\n');
  fs.writeFileSync(path.join(directory, 'production.inc'), methods);
  const file = path.join(directory, 'fixture.cpp');
  fs.writeFileSync(file, harness);
  const { binary, commands } = compileCommands(discoverCompiler(), {
    out: directory, binaryName: 'gl_readback', sources: [{ file }], includes: [directory],
  });
  for (const command of commands) {
    const result = cp.spawnSync(command.executable, command.args, { cwd: directory, env: command.env, encoding: 'utf8', timeout: 120000 });
    if (result.error || result.status !== 0) throw new Error(`Compile failed: ${result.error?.message || result.stdout + result.stderr}`);
  }
  const result = cp.spawnSync(binary, [], { cwd: directory, encoding: 'utf8', timeout: 30000 });
  if (result.error) throw result.error;
  fs.writeFileSync(path.join(directory, 'test-output.log'), result.stdout + result.stderr);
  process.stdout.write(result.stdout + result.stderr);
  if (expectFailure ? result.status === 0 : result.status !== 0) throw new Error(`${name}: unexpected test exit ${result.status}`);
  if (expectFailure) console.log('PASS: baseline reproduces readback boundary failure');
}

// Exercise the production synchronous command, dispatch, backend size selection,
// StartVideo and CVideo constructor. Only external services and queue transport are stubs.
function runVideoContract() {
  const read = file => fs.readFileSync(path.join(root, file), 'utf8');
  const base = read('src/engine/client/backend/backend_base.h');
  const threaded = read('src/engine/client/graphics_threaded.h');
  const gl = read(sourcePath);
  const vk = read('src/engine/client/backend/vulkan/backend_vulkan.cpp');
  const client = read('src/engine/client/client.cpp');
  const video = read('src/engine/client/video.cpp');
  const body = (source, name, owner = '') => method(source, name, owner);
  const declaration = (source, start) => {
    const begin = source.indexOf(start);
    if (begin < 0) throw new Error(`Missing production declaration: ${start}`);
    const open = source.indexOf('{', begin);
    let end = open + 1, depth = 1;
    for (; depth && end < source.length; ++end) {
      if (source[end] === '{') ++depth;
      if (source[end] === '}') --depth;
    }
    return source.slice(begin, end) + ';';
  };
  for (const file of ['src/engine/client/backend_sdl.cpp', 'src/engine/client/backend_egl.cpp']) {
    if (!read(file).includes('m_pGLBackend->RunCommandWithReadback(pCommand)'))
      throw new Error(`${file}: production command loop does not dispatch the size query`);
  }
  const source = `
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <exception>
static void Require(bool Ok, const char *Message) { if(!Ok) throw std::runtime_error(Message); }
#define dbg_assert(Ok, ...) Require(Ok, "production assertion")
static std::thread::id s_MainThread = std::this_thread::get_id();
static int s_AudioDepth = 0, s_Constructed = 0, s_Started = 0;
static int64_t time_freq() { return 1000; }
static void log_error(const char *, const char *, ...) {}
static void log_info(const char *, const char *, ...) {}
static void str_timestamp(char *p, size_t n) { std::snprintf(p, n, "timestamp"); }
template<typename... T> static void str_format(char *p, size_t n, const char *f, T... Args) { std::snprintf(p,n,f,Args...); }
template<size_t N> static void str_copy(char (&p)[N], const char *s) { std::snprintf(p,N,"%s",s); }
constexpr int IO_MAX_PATH_LENGTH = 512;
struct { int m_ClVideoRecorderFPS = 60; int m_ClVideoSndEnable = 1; } g_Config;
struct CImageInfo { enum EImageFormat { FORMAT_RGBA }; };
struct IStorage {};
struct ISound {
  bool IsSoundEnabled() { return true; }
  void PauseAudioDevice() { ++s_AudioDepth; }
  void UnpauseAudioDevice() { --s_AudioDepth; }
};
struct CCommandBuffer {
  enum { CMD_READ_PRESENTED_IMAGE_SIZE = 97 };
  struct SCommand { int m_Cmd; explicit SCommand(int Cmd) : m_Cmd(Cmd) {} };
  ${declaration(threaded, 'struct SCommand_ReadPresentedImageSize :')}
  struct SCommand_Update_Viewport {
    int m_X, m_Y, m_Width, m_Height, m_DrawableWidth, m_DrawableHeight;
    bool m_ByResize;
  };
};
static void glViewport(int, int, int, int) {}
enum ERunCommandReturnTypes { RUN_COMMAND_COMMAND_HANDLED, RUN_COMMAND_COMMAND_UNHANDLED };
class CCommandProcessorFragment_GLBase {
protected:
  ${body(base, 'GetPresentedImageSize')}
public:
  virtual ~CCommandProcessorFragment_GLBase() = default;
  virtual ERunCommandReturnTypes RunCommand(const CCommandBuffer::SCommand *) { return RUN_COMMAND_COMMAND_UNHANDLED; }
  ${body(base, 'RunCommandWithReadback')}
};
class CCommandProcessorFragment_OpenGL : public CCommandProcessorFragment_GLBase {
public:
  uint32_t m_CanvasWidth = 0, m_CanvasHeight = 0;
  int m_ViewportX = 0, m_ViewportY = 0, m_ViewportWidth = 0, m_ViewportHeight = 0;
  bool m_HasDisplayCutout = false;
  void Cmd_Update_Viewport(const CCommandBuffer::SCommand_Update_Viewport *);
  bool GetPresentedImageSize(uint32_t &, uint32_t &) override;
};
${body(gl, 'Cmd_Update_Viewport', 'CCommandProcessorFragment_OpenGL')}
${body(gl, 'GetPresentedImageSize', 'CCommandProcessorFragment_OpenGL')}
struct VkExtent2D { uint32_t width, height; };
constexpr int VK_FORMAT_R8G8B8A8_UNORM = 1, VK_FORMAT_B8G8R8A8_UNORM = 2;
${declaration(vk, 'struct SSwapImgViewportExtent')}
class CVulkan : public CCommandProcessorFragment_GLBase {
public:
  struct { int format = VK_FORMAT_R8G8B8A8_UNORM; } m_VKSurfFormat;
  SSwapImgViewportExtent m_VKSwapImgAndViewportExtent{};
  ${body(vk, 'GetPresentedImageSize')}
};
class CNull : public CCommandProcessorFragment_GLBase {};
class CFail : public CCommandProcessorFragment_GLBase {
  bool GetPresentedImageSize(uint32_t &W, uint32_t &H) override { W = 123; H = 456; return false; }
};
class IGraphics {
public:
  virtual ~IGraphics() = default;
  virtual bool ReadPresentedImageSize(uint32_t &, uint32_t &) = 0;
};
class CGraphics_Threaded : public IGraphics {
public:
  CCommandProcessorFragment_GLBase *m_pBackend;
  bool m_pCommandBuffer = true;
  CCommandBuffer::SCommand_ReadPresentedImageSize m_Queued;
  std::thread m_Worker;
  std::exception_ptr m_Error;
  int m_ScreenW = 1, m_ScreenH = 1, m_Queries = 0;
  bool m_Waited = false;
  explicit CGraphics_Threaded(CCommandProcessorFragment_GLBase *p) : m_pBackend(p) {}
  ~CGraphics_Threaded() { if(m_Worker.joinable()) m_Worker.join(); }
  int ScreenWidth() { throw std::runtime_error("StartVideo used viewport width"); }
  int ScreenHeight() { throw std::runtime_error("StartVideo used viewport height"); }
  void AddCmd(const CCommandBuffer::SCommand_ReadPresentedImageSize &Cmd) { m_Queued = Cmd; ++m_Queries; }
  void KickCommandBuffer() {
    m_Waited = false;
    m_Worker = std::thread([this] {
      try {
        Require(std::this_thread::get_id() != s_MainThread, "query ran on main thread");
        Require(!*m_Queued.m_pSuccess, "success was not initialized");
        Require(m_pBackend->RunCommandWithReadback(&m_Queued) == RUN_COMMAND_COMMAND_HANDLED, "query not handled");
      } catch(...) { m_Error = std::current_exception(); }
    });
  }
  void WaitForIdle() {
    if(m_Worker.joinable()) m_Worker.join();
    m_Waited = true;
    if(m_Error) std::rethrow_exception(m_Error);
  }
  bool ReadPresentedImageSize(uint32_t &, uint32_t &) override;
};
${body(read('src/engine/client/graphics_threaded.cpp'), 'ReadPresentedImageSize', 'CGraphics_Threaded')}
class CVideo;
class IVideo { public: static CVideo *Current(); };
class CVideo {
public:
  static CVideo *ms_pCurrentVideo;
  IGraphics *m_pGraphics;
  IStorage *m_pStorage;
  ISound *m_pSound;
  void *m_pFormatContext, *m_pFormat, *m_pOptDict, *m_pVideoCodec, *m_pAudioCodec;
  int m_Width, m_Height, m_FPS, m_ProcessingVideoFrame, m_ProcessingAudioFrame;
  int64_t m_TickTime, m_LocalStartTime;
  bool m_Recording, m_Started, m_Stopped, m_HasAudio;
  char m_aName[512];
  CVideo(IGraphics *, ISound *, IStorage *, int, int, int64_t, const char *);
  bool Start() {
    Require(s_AudioDepth == 0, "sound remained paused after construction");
    Require(static_cast<CGraphics_Threaded *>(m_pGraphics)->m_Waited, "video started before query completed");
    ++s_Started; return true;
  }
  void Pause(bool) {}
};
CVideo *CVideo::ms_pCurrentVideo = nullptr;
CVideo *IVideo::Current() { return CVideo::ms_pCurrentVideo; }
${body(video, 'CVideo', 'CVideo')}
struct IClient { enum { STATE_DEMOPLAYBACK = 5 }; };
class CClient {
public:
  CGraphics_Threaded *m_pGraphics;
  ISound m_Sound;
  IStorage m_Storage;
  int64_t m_LocalStartTime = 123;
  struct CDemo {
    struct SInfo { struct { bool m_Paused = false; } m_Info; } m_Info;
    bool m_Stopped = false;
    void Stop(const char *) { m_Stopped = true; }
    SInfo *Info() { return &m_Info; }
  } m_DemoPlayer;
  explicit CClient(CGraphics_Threaded *p) : m_pGraphics(p) {}
  int State() { return IClient::STATE_DEMOPLAYBACK; }
  CGraphics_Threaded *Graphics() { return m_pGraphics; }
  ISound *Sound() { return &m_Sound; }
  IStorage *Storage() { return &m_Storage; }
  void StartVideo(const char *, bool);
};
${body(client, 'StartVideo', 'CClient')}
static void CheckStart(CCommandProcessorFragment_GLBase &Backend, uint32_t W, uint32_t H, const char *Name) {
  CGraphics_Threaded Graphics(&Backend);
  CClient Client(&Graphics);
  Client.StartVideo("test", false);
  auto *pVideo = IVideo::Current();
  Require(pVideo && !Client.m_DemoPlayer.m_Stopped, "video was not created");
  Require(pVideo->m_Width == static_cast<int>(W) && pVideo->m_Height == static_cast<int>(H), "constructor dimensions differ from readback");
  Require(Graphics.m_Queries == 1 && Graphics.m_Waited, "synchronous query was bypassed");
  delete pVideo; CVideo::ms_pCurrentVideo = nullptr;
  std::cout << "PASS: production StartVideo + constructor " << Name << "\\n";
}
static void CheckFailure(CCommandProcessorFragment_GLBase *pBackend, bool HaveBuffer, const char *Name) {
  CGraphics_Threaded Graphics(pBackend); Graphics.m_pCommandBuffer = HaveBuffer;
  uint32_t W = 99, H = 88;
  Require(!Graphics.ReadPresentedImageSize(W,H) && W == 0 && H == 0, "failed query kept stale dimensions");
  CClient Client(&Graphics); Client.StartVideo("test", false);
  Require(!IVideo::Current() && Client.m_DemoPlayer.m_Stopped && s_AudioDepth == 0, "failed query constructed a video");
  std::cout << "PASS: production failure contract " << Name << "\\n";
}
int main() { try {
  CCommandProcessorFragment_OpenGL Gl;
  struct SCase { const char *m_pName; int W, H, X, Y, ViewW, ViewH; };
  for(const auto &Case : std::vector<SCase>{
    {"GL no bars",192,108,0,0,192,108},
    {"GL cropped width",192,108,24,0,144,108},
    {"GL cropped height",144,108,0,13,144,82},
    {"GL HiDPI",384,216,48,0,288,216}}) {
    CCommandBuffer::SCommand_Update_Viewport Cmd{Case.X,Case.Y,Case.ViewW,Case.ViewH,Case.W,Case.H,true};
    Gl.Cmd_Update_Viewport(&Cmd);
    CheckStart(Gl, Case.W, Case.H, Case.m_pName);
  }
  CVulkan Vk;
  Vk.m_VKSwapImgAndViewportExtent.m_SwapImageViewport = {144,200};
  Vk.m_VKSwapImgAndViewportExtent.m_ForcedViewport = {144,115};
  Vk.m_VKSwapImgAndViewportExtent.m_HasForcedViewport = true;
  CheckStart(Vk, 144,115,"Vulkan forced viewport");
  Vk.m_VKSwapImgAndViewportExtent.m_HasForcedViewport = false;
  CheckStart(Vk,144,200,"Vulkan full swap image");
  CNull Null; CFail Fail;
  CheckFailure(&Null,true,"null backend"); CheckFailure(&Fail,true,"backend writes dimensions then fails");
  Gl.m_CanvasWidth = 0; CheckFailure(&Gl,true,"zero canvas");
  Vk.m_VKSurfFormat.format = 99; CheckFailure(&Vk,true,"Vulkan unsupported format");
  CheckFailure(nullptr,true,"no backend"); CheckFailure(&Null,false,"no command buffer");
  Require(s_Started == 6, "unexpected count of started videos");
  std::cout << "RESULT 12/12 production video-size contract scenarios passed\\n";
  return 0;
} catch(const std::exception &Error) { std::cerr << "FAIL: " << Error.what() << "\\n"; return 1; } }
`;
  const directory = path.join(out, 'video-contract');
  fs.mkdirSync(directory);
  const file = path.join(directory, 'fixture.cpp');
  fs.writeFileSync(file, source);
  const { binary, commands } = compileCommands(discoverCompiler(), {
    out: directory, binaryName: 'video_contract', sources: [{ file }],
  });
  for (const command of commands) {
    const result = cp.spawnSync(command.executable, command.args, { cwd: directory, env: command.env, encoding: 'utf8', timeout: 120000 });
    if (result.error || result.status !== 0) throw new Error(`Contract compile failed: ${result.error?.message || result.stdout + result.stderr}`);
  }
  const result = cp.spawnSync(binary, [], { cwd: directory, encoding: 'utf8', timeout: 30000 });
  fs.writeFileSync(path.join(directory, 'test-output.log'), (result.stdout || '') + (result.stderr || ''));
  process.stdout.write((result.stdout || '') + (result.stderr || ''));
  if (result.error || result.status !== 0) throw new Error(`Contract test failed: ${result.error?.message || result.status}`);
}

try {
  const args = process.argv.slice(2);
  if (args.length) {
    if (args.length !== 2 || args[0] !== '--baseline') throw new Error('usage: [--baseline <git-ref>]');
    runSource(cp.execFileSync('git', ['-C', root, 'show', `${args[1]}:${sourcePath}`], { encoding: 'utf8' }), 'baseline', true);
  }
  runSource(fs.readFileSync(path.join(root, sourcePath), 'utf8'), 'working', false);
  runVideoContract();
  console.log(`Artifacts: ${out}`);
} catch (error) {
  console.error(`${error.stack}\nArtifacts: ${out}`);
  process.exitCode = 1;
}
