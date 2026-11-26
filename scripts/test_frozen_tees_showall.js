// Local-only regression; no server connection, CMake/build-tree writes or HUD edits.
// node scripts/test_frozen_tees_showall.js [evidence-directory] [generated-directory]
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const os = require('node:os');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname, '..');
const out = process.argv[2] ? path.resolve(process.argv[2]) : fs.mkdtempSync(path.join(os.tmpdir(), 'frozen-tees-showall-'));
fs.mkdirSync(out, { recursive: true });
const generated = process.argv[3] ? path.resolve(process.argv[3]) : path.join(root, 'build-vs2026/src');
const read = file => fs.readFileSync(path.join(root, file), 'utf8').replace(/\r\n/g, '\n');
const source = read('src/engine/client/client.cpp');
function between(text, start, end) {
  const a = text.indexOf(start), b = text.indexOf(end, a + start.length);
  assert(a >= 0 && b > a, `Missing production seam ${start}`);
  return text.slice(a, b);
}
const postConnect = between(source, 'void CClient::OnPostConnect(int Conn)', 'void CClient::SendChatCommand(');
const send = between(source, 'void CClient::SendChatCommand(', 'void CClient::UpdateFrozenTeesShowAll(');
const update = between(source, 'void CClient::UpdateFrozenTeesShowAll(', 'static void GenerateTimeoutCode');
const enter = between(source, 'void CClient::EnterGame(int Conn)', 'void CClient::OnPostConnect(');
const disconnect = between(source, 'void CClient::DisconnectWithReason(', 'void CClient::Disconnect()');
const dummyDisconnect = between(source, 'void CClient::DummyDisconnect(', 'bool CClient::DummyAllowed()');
const frameUpdate = between(source, 'void CClient::Update()', 'void CClient::RegisterInterfaces()');
assert(!frameUpdate.includes('UpdateFrozenTeesShowAll('), 'No per-frame requester');
assert(!postConnect.includes('/showall'), 'Never append showall after timeout');
assert(postConnect.includes('SendChatCommand(Conn, aBufMsg);'));
assert(enter.includes('m_aFrozenTeesShowAll[Conn].Reset();'));
assert(disconnect.includes('m_aFrozenTeesShowAll[CONN_MAIN].Reset();'));
assert(dummyDisconnect.includes('m_aFrozenTeesShowAll[CONN_DUMMY].Reset();'));
assert(source.indexOf('UpdateFrozenTeesShowAll(Conn, AltSnapBuffer.AsSnapshot());') > source.indexOf('if(AltSnapSize < 0)'));
assert(!update.includes('m_ClDummy') && !update.includes('GameClient()->m_GameInfo'));
assert(!source.includes('SendChatCommand(Conn, "/showall 0")'));
const sender = between(source, 'int CClient::SendMsg(int Conn', 'int CClient::SendMsgActive');
assert(sender.indexOf('OnChatSent(') > sender.indexOf('if(!(Flags & MSGFLAG_NOSEND))'));
assert(sender.includes('Unpacker.Reset(Pack.Data(), Pack.Size())') && sender.includes('protocol7::NETMSGTYPE_CL_SAY'));
const config = read('src/engine/shared/config_variables_tclient.h');
assert.match(config, /MACRO_CONFIG_INT\(TcFrozenTeesShowAll\s*,\s*tc_frozen_tees_showall\s*,\s*0\s*,\s*0\s*,\s*1\s*,\s*CFGFLAG_CLIENT \| CFGFLAG_SAVE/, 'native /showall is opt-in and saved');
assert(update.includes('const bool Requested = g_Config.m_TcFrozenTeesShowAll != 0;'), 'gate the request on the player setting');
const menu = read('src/game/client/components/tclient/menus_tclient.cpp');
const section = between(menu, 'TCLocalize("Frozen Tee Display")', '// ***** Tile Outlines ***** //');
const label = 'Request full-distance tee status (native /showall)';
assert(section.includes(`DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcFrozenTeesShowAll, TCLocalize("${label}"), &g_Config.m_TcFrozenTeesShowAll`), 'showall checkbox belongs in the TC Tee status section');
for (const lang of ['simplified_chinese', 'traditional_chinese']) {
  const dictionary = read(`data/tclient/languages/${lang}.txt`);
  const matches = dictionary.split('\n').filter(line => line === label);
  assert.equal(matches.length, 1, `Unique ${lang} key`);
  const next = dictionary.slice(dictionary.indexOf(label) + label.length).split('\n')[1];
  assert(next.startsWith('== ') && next.trim() !== '==', `Translated ${lang} key`);
}
// Use real generated message structs and their Pack methods, with a recording
// transport. Suppress only engine/message.h to make its packer standalone.
fs.writeFileSync(path.join(out, 'test.cpp'), `
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define ENGINE_MESSAGE_H
class CMsgPacker {
public:
 int m_MsgId; bool m_NoTranslate = false; std::vector<int> m_Ints; std::string m_String;
 template<class T> CMsgPacker(const T *) : m_MsgId(T::ms_MsgId) {}
 CMsgPacker(int Id, bool, bool NoTranslate=false) : m_MsgId(Id), m_NoTranslate(NoTranslate) {}
 void AddInt(int Value) { m_Ints.push_back(Value); }
 void AddString(const char *pValue, int) { m_String = pValue; }
 int Error() const { return 0; }
};
#include <generated/protocol.h>
#include <generated/protocol7.h>
#include <engine/client/frozen_tees_showall.h>
constexpr int CONN_MAIN=0, CONN_DUMMY=1, NUM_DUMMIES=2, MSGFLAG_VITAL=1, NETSTATE_ONLINE=3;
static int64_t Now=0; int64_t time_get() { return Now; } int64_t time_freq() { return 1000; }
struct IClient { enum { STATE_OFFLINE=0, STATE_ONLINE=3, STATE_DEMOPLAYBACK=4 }; };
struct CNetClient { int m_State=NETSTATE_ONLINE; int State() const { return m_State; } };
struct Config { int m_TcShowFrozenHud=1, m_TcShowFrozenText=0, m_TcFrozenTeesShowAll=0;
 int m_ClDummyDefaultEyes=0, m_ClPlayerDefaultEyes=0, m_ClEyeDuration=5; char m_ClRunOnJoin[100]=""; } g_Config;
const char *str_find_nocase(const char *pHaystack, const char *pNeedle) {
 std::string H(pHaystack), N(pNeedle);
 for(char &C : H) C=(char)std::tolower((unsigned char)C);
 for(char &C : N) C=(char)std::tolower((unsigned char)C);
 auto Pos=H.find(N); return Pos==std::string::npos ? nullptr : pHaystack+Pos;
}
template<class... T> void str_format(char *pBuf, size_t Size, const char *pFmt, T... Args) { std::snprintf(pBuf, Size, pFmt, Args...); }
template<size_t N> void str_append(char (&aBuf)[N], const char *pText) { const size_t Length=std::strlen(aBuf); std::snprintf(aBuf+Length,N-Length,"%s",pText); }
void dbg_msg(const char *, const char *, int) {}
struct CSnapshotItem { CNetObj_GameInfoEx m_Info; const void *Data() const { return &m_Info; } };
class CSnapshot {
public:
 bool m_HasInfo=true; int m_Size=12; CSnapshotItem m_Item{};
 CSnapshot(int Version=2, int Flags=GAMEINFOFLAG_ENTITIES_DDRACE, int Size=12) : m_Size(Size) { m_Item.m_Info.m_Version=Version; m_Item.m_Info.m_Flags=Flags; }
 int NumItems() const { return m_HasInfo ? 1 : 0; } int GetItemType(int) const { return NETOBJTYPE_GAMEINFOEX; }
 int GetItemSize(int) const { return m_Size; } const CSnapshotItem *GetItem(int) const { return &m_Item; }
};
struct Attempt { int m_Conn, m_MsgId; bool m_NoTranslate; int64_t m_Time; std::vector<int> m_Ints; std::string m_Text; };
class CClient {
public:
 bool m_Sixup=false; int m_State=IClient::STATE_ONLINE;
 CNetClient m_aNetClient[2]; CFrozenTeesShowAll m_aFrozenTeesShowAll[2];
 struct { bool m_ChatTimeoutCode=true; } m_ServerCapabilities;
 struct { char m_aGameType[32]="DDRace"; } m_CurrentServerInfo;
 char m_aTimeoutCodes[2][32] = {"main-code", "dummy-code"};
 std::vector<Attempt> m_vAttempts;
 int State() const { return m_State; } bool IsSixup() const { return m_Sixup; }
 int SendMsg(int Conn, CMsgPacker *pMsg, int Flags) {
  if(Flags!=MSGFLAG_VITAL) std::abort();
  m_vAttempts.push_back({Conn,pMsg->m_MsgId,pMsg->m_NoTranslate,Now,pMsg->m_Ints,pMsg->m_String});
  m_aFrozenTeesShowAll[Conn].OnChatSent(time_get(), time_freq()); return 0;
 }
 template<class T> int SendPackMsg(int Conn, T *pMsg, int Flags, bool NoTranslate=false) {
  CMsgPacker Packer(T::ms_MsgId,false,NoTranslate); if(pMsg->Pack(&Packer)) return -1; return SendMsg(Conn,&Packer,Flags);
 }
 void OnPostConnect(int Conn); void SendChatCommand(int Conn,const char *pCommand);
 void UpdateFrozenTeesShowAll(int Conn,const CSnapshot *pSnapshot);
};
${postConnect}
${send}
${update}
void Check(bool Value, const char *pMessage) { if(!Value) { std::fprintf(stderr,"FAIL: %s\\n",pMessage); std::exit(1); } }
void ResetConfig() { g_Config = Config(); g_Config.m_TcFrozenTeesShowAll = 1; }
int main() {
 for(bool Sixup : {false,true}) {
  g_Config=Config(); CClient Client; Client.m_Sixup=Sixup; Client.m_ServerCapabilities.m_ChatTimeoutCode=false; CSnapshot DDRace;
  Now=0; Client.OnPostConnect(0); Client.OnPostConnect(1);
  for(int Tick=0;Tick<30;++Tick){Now=Tick*1000;Client.UpdateFrozenTeesShowAll(0,&DDRace);Client.UpdateFrozenTeesShowAll(1,&DDRace);}
  Check(Client.m_vAttempts.empty(),"default off sends nothing on either connection");
 }
 std::puts("PASS default-off /showall on 0.6/0.7 and on both connections");
 // Exact CGameClient applicability precedence, including truncated old objects.
 Check(TcFrozenTeesEntitiesDDRace(12,2,GAMEINFOFLAG_ENTITIES_DDRACE,false),"explicit positive entity flags");
 Check(!TcFrozenTeesEntitiesDDRace(12,2,GAMEINFOFLAG_GAMETYPE_DDRACE,true),"explicit negative entity flags override fallback");
 Check(TcFrozenTeesEntitiesDDRace(12,1,GAMEINFOFLAG_GAMETYPE_DDNET,false),"version one DDNet implies DDRace");
 Check(TcFrozenTeesEntitiesDDRace(4,99,0,true),"flags-only object uses old fallback");
 Check(!TcFrozenTeesEntitiesDDRace(8,99,0,true),"short object version capped to four");
 Check(!TcFrozenTeesEntitiesDDRace(12,0,GAMEINFOFLAG_ENTITIES_DDRACE,false),"legacy game type required");
 std::puts("PASS applicability: modern/legacy/truncated flags");
 for(bool Sixup : {false,true}) {
  ResetConfig(); CClient Client; Client.m_Sixup=Sixup; CSnapshot DDRace;
  Now=0; Client.UpdateFrozenTeesShowAll(0,&DDRace); Check(Client.m_vAttempts.empty(),"no attempt before post-connect");
  std::snprintf(g_Config.m_ClRunOnJoin,sizeof(g_Config.m_ClRunOnJoin),"say retained-command");
  Client.OnPostConnect(0); Client.OnPostConnect(1);
  Check(Client.m_vAttempts.size()==2,"timeout once on each connection");
  Check(Client.m_vAttempts[0].m_Text=="/mc;timeout main-code;say retained-command","run-on-join preserved");
  Check(Client.m_vAttempts[1].m_Text=="/mc;timeout dummy-code;say retained-command","dummy timeout preserved");
  Now=1999; Client.UpdateFrozenTeesShowAll(0,&DDRace); Client.UpdateFrozenTeesShowAll(1,&DDRace);
  Check(Client.m_vAttempts.size()==2,"second command waits beyond chat limiter");
  Now=2000; Client.UpdateFrozenTeesShowAll(1,&DDRace); Client.UpdateFrozenTeesShowAll(0,&DDRace);
  Check(Client.m_vAttempts.size()==4,"one independent showall per connection");
  for(int Index=2; Index<4; ++Index) {
   const auto &A=Client.m_vAttempts[Index];
   Check(A.m_Conn==(Index==2?1:0),"explicit destination not active connection");
   Check(A.m_Text=="/showall 1" && A.m_Time==2000,"standalone command after timeout recovery");
   Check(A.m_MsgId==(Sixup?(int)protocol7::NETMSGTYPE_CL_SAY:(int)NETMSGTYPE_CL_SAY),"native message ID");
   Check(A.m_NoTranslate==Sixup,"sixup not repacked as six");
   Check(A.m_Ints==(Sixup?std::vector<int>{protocol7::CHAT_ALL,-1}:std::vector<int>{0}),"native mode/team/target fields");
  }
  for(int Index=0;Index<1000;++Index) { Now+=100; Client.UpdateFrozenTeesShowAll(0,&DDRace); Client.UpdateFrozenTeesShowAll(1,&DDRace); }
  Check(Client.m_vAttempts.size()==4,"continuing snapshots cannot spam requests");
  std::printf("PASS native %s: timeout recovery separation, destination, fields, one-shot\\n",Sixup?"0.7":"0.6");
 }
 ResetConfig(); CSnapshot Yes, No(2,0); CClient C; Now=0; C.m_ServerCapabilities.m_ChatTimeoutCode=false;
 C.OnPostConnect(0); C.OnPostConnect(1); Now=2000;
 C.UpdateFrozenTeesShowAll(0,&Yes); C.UpdateFrozenTeesShowAll(1,&No);
 Check(C.m_vAttempts.size()==1 && C.m_vAttempts.back().m_Conn==0,"inactive negative gate independent of main");
 C.UpdateFrozenTeesShowAll(1,&Yes); Check(C.m_vAttempts.size()==2 && C.m_vAttempts.back().m_Conn==1,"dummy can become applicable");
 std::puts("PASS independent Conn applicability without timeout capability");
 g_Config=Config(); CClient Late; Now=0; Late.OnPostConnect(0); Now=3000;
 Late.UpdateFrozenTeesShowAll(0,&Yes); Check(Late.m_vAttempts.size()==1,"default off preserves the existing timeout command without showall");
 g_Config.m_TcShowFrozenHud=0; g_Config.m_TcShowFrozenText=0; g_Config.m_TcFrozenTeesShowAll=1;
 Late.UpdateFrozenTeesShowAll(0,&Yes); Check(Late.m_vAttempts.size()==2 && Late.m_vAttempts.back().m_Text=="/showall 1","enabling mid-connection works independently of the HUD");
 g_Config.m_TcFrozenTeesShowAll=0; Late.UpdateFrozenTeesShowAll(0,&Yes); Check(Late.m_vAttempts.size()==2,"disabling does not send showall zero");
 g_Config.m_TcFrozenTeesShowAll=1; Late.UpdateFrozenTeesShowAll(0,&Yes); Check(Late.m_vAttempts.size()==2,"re-enabling does not repeat the same connection request");
 Late.m_aFrozenTeesShowAll[0].Reset(); Late.OnPostConnect(0); Now=5000; Late.UpdateFrozenTeesShowAll(0,&Yes);
 Check(Late.m_vAttempts.size()==4,"new entry re-arms just this connection");
 std::puts("PASS live opt-in, turning off without a server command, and reconnect reset");
 ResetConfig(); CClient Busy; Now=0; Busy.OnPostConnect(0); Busy.UpdateFrozenTeesShowAll(0,&Yes);
 for(int Index=1;Index<=15;++Index) { Now=Index*1000; Busy.SendChatCommand(0,"/user-command"); Busy.UpdateFrozenTeesShowAll(0,&Yes); }
 Now=100000; Busy.UpdateFrozenTeesShowAll(0,&Yes); Check(Busy.m_vAttempts.size()==16,"bounded busy-chat window expires");
 ResetConfig(); CClient Quiet; Now=0; Quiet.OnPostConnect(0); Quiet.UpdateFrozenTeesShowAll(0,&Yes);
 Now=1500; Quiet.SendChatCommand(0,"/user-command"); Now=2000; Quiet.UpdateFrozenTeesShowAll(0,&Yes);
 Check(Quiet.m_vAttempts.size()==2,"user chat delays showall"); Now=3500; Quiet.UpdateFrozenTeesShowAll(0,&Yes);
 Check(Quiet.m_vAttempts.size()==3,"finite later quiet opportunity");
 std::puts("PASS outgoing chat quiet period and finite expiry, no retries");
 ResetConfig(); CClient Demo; Now=0; Demo.OnPostConnect(0); Now=5000;
 Demo.m_State=IClient::STATE_DEMOPLAYBACK; Demo.UpdateFrozenTeesShowAll(0,&Yes);
 Demo.m_State=IClient::STATE_OFFLINE; Demo.UpdateFrozenTeesShowAll(0,&Yes);
 Demo.m_State=IClient::STATE_ONLINE; Demo.m_aNetClient[0].m_State=0; Demo.UpdateFrozenTeesShowAll(0,&Yes);
 Check(Demo.m_vAttempts.size()==1,"demo/offline/transport offline never showall");
 std::puts("PASS demo, offline and transport gate");
 CFrozenTeesShowAll Toggle; Now=0; Toggle.OnPostConnect(Now,1000); Toggle.Update(Now,1000,true,true,true);
 for(int Index=1;Index<15;++Index) { Now=Index*1000; Toggle.OnChatSent(Now,1000); Toggle.Update(Now,1000,true,false,true); Toggle.Update(Now,1000,true,true,true); }
 Check(!Toggle.Update(20000,1000,true,true,true),"toggle cannot prolong original deadline");
 CFrozenTeesShowAll Pause; Pause.OnPostConnect(0,1000); Pause.Update(0,1000,true,true,true); Check(!Pause.Update(15000,1000,true,true,true),"no stale send after snapshot pause");
 CFrozenTeesShowAll Zero; Zero.OnPostConnect(0,0); Check(!Zero.Update(5000,0,true,true,true),"invalid clock fails safe");
 std::puts("PASS repeated-toggle deadline, stale snapshot pause, invalid clock");
}
`);
let transcript = 'PASS static lifecycle, snapshot seam, config, menu and simplified/traditional keys\n';
function run(exe, args, env=process.env) {
  const result = cp.spawnSync(exe,args,{cwd:out,env,encoding:'utf8',timeout:120000,maxBuffer:4*1024*1024});
  transcript += `$ ${exe} ${args.join(' ')}\n${result.stdout||''}${result.stderr||''}EXIT=${result.status}\n`;
  fs.writeFileSync(path.join(out,'test.log'),transcript);
  if(result.error) throw result.error;
  assert.equal(result.status,0,`Failed; see ${path.join(out,'test.log')}`);
}
const binary = path.join(out,process.platform==='win32'?'showall-test.exe':'showall-test');
if(process.platform==='win32') {
 const where=cp.spawnSync('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe',['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],{encoding:'utf8'});
 assert(where.status===0 && where.stdout.trim(),'MSVC Build Tools not found');
 const install=where.stdout.trim(), tools=path.join(install,'VC/Tools/MSVC');
 const msvc=path.join(tools,fs.readdirSync(tools).filter(x=>fs.statSync(path.join(tools,x)).isDirectory()).sort().at(-1));
 const kits='C:/Program Files (x86)/Windows Kits/10', sdk=fs.readdirSync(path.join(kits,'Include')).filter(x=>/^10\./.test(x)).sort().at(-1);
 const env={...process.env,INCLUDE:[path.join(msvc,'include'),...['ucrt','shared','um','winrt'].map(x=>path.join(kits,'Include',sdk,x))].join(';'),LIB:[path.join(msvc,'lib/x64'),path.join(kits,'Lib',sdk,'ucrt/x64'),path.join(kits,'Lib',sdk,'um/x64')].join(';'),PATH:path.join(msvc,'bin/Hostx64/x64')+';'+process.env.PATH};
 run(path.join(msvc,'bin/Hostx64/x64/cl.exe'),['/nologo','/std:c++20','/EHsc','/W4','/WX','/utf-8',`/I${generated}`,`/I${path.join(root,'src')}`,path.join(out,'test.cpp'),`/Fe${binary}`],env);
} else {
 run(process.env.CXX||'c++',['-std=c++20','-Wall','-Wextra','-Werror','-I',generated,'-I',path.join(root,'src'),path.join(out,'test.cpp'),'-o',binary]);
}
run(binary,[]);
console.log(`PASS local-only frozen tee showall regression; log: ${path.join(out,'test.log')}`);
