// Run with: node scripts/test_island_link_status.js
// Compile the verbatim link-cell rendering block with deterministic drawing stubs.
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const cp = require('node:child_process');
const crypto = require('node:crypto');

const root = path.resolve(__dirname, '..');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'tclient-link-cell-'));
let success = false;
function run(executable, args, env = process.env) {
  const result = cp.spawnSync(executable, args, { cwd: work, env, encoding: 'utf8', timeout: 120000, maxBuffer: 4 * 1024 * 1024 });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    process.stdout.write(result.stdout || '');
    process.stderr.write(result.stderr || '');
    throw new Error(`${path.basename(executable)} exited with ${result.status}; evidence: ${work}`);
  }
  return result.stdout || '';
}
try {
  const relative = 'src/game/client/components/tclient/island_music.cpp';
  const bytes = fs.readFileSync(path.join(root, relative));
  const source = bytes.toString('utf8').replace(/\r\n/g, '\n');
  const start = source.indexOf('\t\t// ---- link status cell');
  const end = source.indexOf('\t\t// ---- right zone:', start);
  if (start < 0 || end <= start) throw new Error('Production link-cell rendering seam not found');
  const block = source.slice(start, end);
  const prefix = String.raw`
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
struct CConfig { int m_TcIslandLinkStatus=1, m_TcFontSize=100, m_ClDupUpstreamLowPct=1, m_ClDupUpstreamHighPct=5; } g_Config;
const char *TCLocalize(const char *pText) { return pText; }
template<typename... T> void str_format(char *p, size_t Size, const char *pFmt, T... Args) { std::snprintf(p, Size, pFmt, Args...); }
template<size_t N> void str_copy(char (&p)[N], const char *pText) { std::snprintf(p, N, "%s", pText); }
struct ColorRGBA { ColorRGBA(float, float, float, float) {} };
struct IGraphics { enum { CORNER_ALL=15 }; };
struct CLinkProbeTracker {
 bool m_Known=true, m_LatencyKnown=true;
 float m_Percent=0.0f, m_Latency=35.0f;
 bool HasRecentEstimate() const { return m_Known; }
 float RecentUnconfirmedPercent() const { return m_Percent; }
 bool HasLatency() const { return m_LatencyKnown; }
 float AverageLatencyMs() const { return m_Latency; }
};
struct IClient { enum { STATE_ONLINE=3 }; };
struct CClient {
 CLinkProbeTracker m_Probe;
 float m_Gap=0.0f;
 int m_State=IClient::STATE_ONLINE;
 int State() const { return m_State; }
 const CLinkProbeTracker *LinkProbe() const { return &m_Probe; }
 float DownlinkLossPercent() const { return m_Gap; }
};
struct CText {
 struct CDraw { float m_X, m_Y, m_Size; std::string m_Text; };
 std::vector<CDraw> m_vDraws;
 void TextColor(float, float, float, float) {}
 void Text(float X, float Y, float Size, const char *pText) { m_vDraws.push_back({X,Y,Size * g_Config.m_TcFontSize / 100.0f,pText}); }
};
struct CLinkCellHarness {
 CClient m_Client;
 CClient *m_pClient=&m_Client;
 CText m_Text;
 CText *m_pTextRender=&m_Text;
 float m_Width=0.0f;
 template<typename... T> void DrawShape(T...) {}
 float MeasureCached(CText *, float Size, const char *pText) { return std::strlen(pText) * Size * g_Config.m_TcFontSize / 200.0f; }
 void Render(float Scale=1.0f) {
  m_Text.m_vDraws.clear();
  const float PillY=40.0f * Scale, Height=72.0f * Scale;
`;
  const suffix = String.raw`
  m_Width=LinkCellW;
  if(ShowLink) DrawLinkCell(0.0f, 1.0f);
 }
};
int g_Checks=0, g_Failures=0;
void Check(bool Pass, const char *pWhat) { ++g_Checks; if(!Pass) { ++g_Failures; std::printf("FAIL: %s\n",pWhat); } }
void Lines(CLinkCellHarness &H, const std::vector<std::string> &Expected, const char *pWhat) {
 H.Render();
 std::vector<std::string> Actual;
 for(const auto &Draw : H.m_Text.m_vDraws) Actual.push_back(Draw.m_Text);
 Check(Actual==Expected,pWhat);
 if(Actual!=Expected) { for(const auto &Text : Actual) std::printf("  actual: %s\n",Text.c_str()); }
}
int main() {
 CLinkCellHarness H;
 Lines(H,{"35 ms"},"healthy link only shows latency");
 const float Width=H.m_Width;
 H.m_Client.m_Probe.m_Percent=2.0f;
 Lines(H,{"Probe 2.0%"},"probe anomaly only shows the probe rate");
 Check(H.m_Width==Width,"probe anomaly does not resize the cell");
 H.m_Client.m_Probe.m_Percent=0.0f; H.m_Client.m_Gap=6.0f;
 Lines(H,{"Snapshot gap 6.0%"},"snapshot anomaly only shows snapshot gaps");
 Check(H.m_Width==Width,"snapshot anomaly does not resize the cell");
 H.m_Client.m_Probe.m_Percent=2.0f;
 Lines(H,{"Probe 2.0%","Snapshot gap 6.0%"},"simultaneous anomalies stay separately labelled");
 Check(H.m_Width==Width,"two anomalies do not resize the cell");
 H.m_Client.m_Probe.m_Percent=0.2f; H.m_Client.m_Gap=0.3f;
 Lines(H,{"35 ms"},"readings below the existing warning threshold show latency");
 H.m_Client.m_Probe.m_Known=false; H.m_Client.m_Gap=-1.0f;
 Lines(H,{"35 ms"},"unknown loss observations do not hide a valid RTT");
 H.m_Client.m_Probe.m_LatencyKnown=false;
 Lines(H,{"--"},"unavailable measurements are not shown as zero");
 H.m_Client.m_Gap=7.0f;
 Lines(H,{"Snapshot gap 7.0%"},"known gaps remain visible when the probe is unknown");
 g_Config.m_TcIslandLinkStatus=0;
 Lines(H,{},"disabled cell draws nothing");
 g_Config.m_TcIslandLinkStatus=1;
 H.m_Client.m_State=0;
 Lines(H,{},"offline cell draws nothing");
 H.m_Client.m_State=IClient::STATE_ONLINE;
 H.m_Client.m_Probe.m_Known=true; H.m_Client.m_Probe.m_LatencyKnown=true;
 H.m_Client.m_Probe.m_Percent=2.0f; H.m_Client.m_Gap=6.0f;
 for(int FontScale : {50,100,150,200}) {
  g_Config.m_TcFontSize=FontScale;
  for(float Scale : {0.42f,1.0f,3.6f}) {
   H.Render(Scale);
   Check(H.m_Text.m_vDraws.size()==2,"scaled abnormal cell has exactly two readouts");
   const auto &Draws=H.m_Text.m_vDraws;
   if(Draws.size()==2) {
    Check(Draws[0].m_Y+Draws[0].m_Size<=Draws[1].m_Y,"scaled lines do not overlap");
    Check(Draws[0].m_Y>=40.0f*Scale && Draws[1].m_Y+Draws[1].m_Size<=112.0f*Scale,"scaled readouts fit inside the capsule");
   }
  }
 }
 std::printf("island_link_checks=%d failures=%d\n",g_Checks,g_Failures);
 return g_Failures ? 1 : 0;
}
`;
  const cpp = path.join(work, 'link_cell_test.cpp');
  fs.writeFileSync(cpp, prefix + block + suffix);
  fs.writeFileSync(path.join(work, 'production-seam.json'), JSON.stringify({ path: relative, sha256: crypto.createHash('sha256').update(bytes).digest('hex') }, null, 2));
  let binary;
  if (process.platform === 'win32') {
    const tools = 'C:/VS2026/BuildTools/VC/Tools/MSVC';
    const msvc = path.join(tools, fs.readdirSync(tools).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop());
    const kits = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Windows Kits/10');
    const sdk = fs.readdirSync(path.join(kits, 'Include')).filter(v => /^10\./.test(v)).sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop();
    const env = { ...process.env, INCLUDE: [path.join(msvc, 'include'), ...['ucrt','shared','um','winrt'].map(n => path.join(kits, 'Include', sdk, n))].join(';'), LIB: [path.join(msvc, 'lib/x64'), ...['ucrt','um'].map(n => path.join(kits, 'Lib', sdk, n, 'x64'))].join(';'), VSLANG: '1033' };
    binary = path.join(work, 'link_cell_test.exe');
    run(path.join(msvc, 'bin/Hostx64/x64/cl.exe'), ['/nologo','/std:c++17','/EHsc','/utf-8',cpp,'/Fe:' + binary], env);
  } else {
    binary = path.join(work, 'link_cell_test');
    run(process.env.CXX || 'c++', ['-std=c++17',cpp,'-o',binary]);
  }
  process.stdout.write(run(binary, []));
  success = true;
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
} finally {
  if (success && !process.env.ISLAND_LINK_TEST_KEEP) fs.rmSync(work, { recursive: true, force: true });
  else console.log('evidence=' + work);
}
