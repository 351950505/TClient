#!/usr/bin/env node
'use strict';

// Runs the unmodified GetServerInfo/State bodies from scripting.cpp with checked
// client stand-ins. CScriptingCtx::Any, CConfig, and MAX_CLIENTS are real headers.
// This does not exercise CGameClient construction, network traffic, or the VM.
// From a Visual Studio developer prompt:
//   node scripts/tclient_state_regression.js --output <directory>
// To compare a pre-fix revision (expected to fail) with the working tree:
//   node scripts/tclient_state_regression.js --output <directory> --baseline HEAD
// Outside a developer prompt, also supply --msvc <VC/Tools/MSVC/version>,
// --sdk <Windows Kits/10>, and --sdk-version <version>.

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');

const options = {};
const allowed = new Set(['output', 'baseline', 'compiler', 'msvc', 'sdk', 'sdk-version']);
for(let i = 2; i < process.argv.length; i += 2)
{
	const name = process.argv[i].replace(/^--/, '');
	if(!process.argv[i].startsWith('--') || !allowed.has(name) || !process.argv[i + 1])
		throw new Error('Expected --output, --baseline, --compiler, --msvc, --sdk, or --sdk-version followed by a value');
	options[name] = process.argv[i + 1];
}
const root = path.resolve(__dirname, '..');
const output = options.output ? path.resolve(options.output) : fs.mkdtempSync(path.join(os.tmpdir(), 'tclient-state-regression-'));
fs.mkdirSync(output, {recursive: true});
const env = {...process.env};
const msvc = options.msvc;
const sdk = options.sdk;
const sdkVersion = options['sdk-version'];
if((msvc || sdk || sdkVersion) && !(msvc && sdk && sdkVersion))
	throw new Error('--msvc, --sdk, and --sdk-version must be supplied together');
if(msvc)
{
	env.INCLUDE = [path.join(msvc, 'include'), ...['ucrt', 'shared', 'um', 'winrt'].map(part => path.join(sdk, 'Include', sdkVersion, part))].join(';');
	env.LIB = [path.join(msvc, 'lib/x64'), ...['ucrt', 'um'].map(part => path.join(sdk, 'Lib', sdkVersion, part, 'x64'))].join(';');
}
const compiler = options.compiler || (msvc ? path.join(msvc, 'bin/Hostx64/x64/cl.exe') : 'cl.exe');
const commands = [];
function run(command, args, log)
{
	commands.push({command, args, cwd: output});
	const result = cp.spawnSync(command, args, {cwd: output, env, encoding: 'utf8', timeout: 45000});
	fs.writeFileSync(path.join(output, log), (result.stdout || '') + (result.stderr || '') + (result.error ? '\n' + result.error.message : ''));
	fs.writeFileSync(path.join(output, 'commands.json'), JSON.stringify({commands, INCLUDE: env.INCLUDE, LIB: env.LIB}, null, 2));
	if(result.error)
		throw result.error;
	return result;
}
function method(text, name)
{
	const start = text.indexOf(name);
	if(start < 0)
		throw new Error('Missing production method: ' + name);
	const brace = text.indexOf('{', start);
	let depth = 1;
	let end = brace + 1;
	for(; depth && end < text.length; ++end)
	{
		if(text[end] === '{') ++depth;
		if(text[end] === '}') --depth;
	}
	if(depth)
		throw new Error('Unbalanced production method: ' + name);
	return text.slice(start, end);
}

const prefix = `#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <game/client/components/tclient/scripting/impl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
CConfig g_Config{};
int str_comp(const char *a,const char *b){return std::strcmp(a,b);}
int str_comp_nocase(const char *a,const char *b){return _stricmp(a,b);}
void net_addr_str(const NETADDR *,char *out,int,bool){out[0]='x';out[1]=0;}
struct CServerInfo { char m_aMap[32]{},m_aName[32]{},m_aCommunityId[32]{}; NETADDR m_aAddresses[1]; int m_MaxClients=16; };
class IClient { public:
 enum EClientState {STATE_OFFLINE,STATE_CONNECTING,STATE_LOADING,STATE_ONLINE,STATE_DEMOPLAYBACK,STATE_QUITTING,STATE_RESTARTING};
 EClientState m_State=STATE_OFFLINE; CServerInfo m_Info; NETADDR m_Address;
 EClientState State()const{return m_State;}const CServerInfo &ServerInfo()const{return m_Info;}
 bool DummyAllowed()const{return true;}bool DummyConnected()const{return false;}bool RconAuthed()const{return false;}
 const NETADDR &ServerAddress()const{return m_Address;}
};
template<class T,size_t Size> struct CheckedArray {
 std::array<T,Size> m_Data{};
 T &operator[](int Index){if(Index<0||Index>=int(Size))throw std::out_of_range("invalid client access");return m_Data[Index];}
 const T &operator[](int Index)const{if(Index<0||Index>=int(Size))throw std::out_of_range("invalid client access");return m_Data[Index];}
 auto begin()const{return m_Data.begin();}auto end()const{return m_Data.end();}
};
struct CMapItemLayerTilemap {int m_Width=100,m_Height=100;};
class CLayers {public:CMapItemLayerTilemap m_Layer;const CMapItemLayerTilemap *GameLayer()const{return &m_Layer;}};
struct FakeMap {const char *BaseName()const{return "map";}};
struct CGameClient {
 struct CClientData {bool m_Active=false;int m_Team=0,m_Id=0;char m_aName[16]{},m_aClan[12]{};int ClientId()const{return m_Id;}};
 struct {char m_aGameType[16]{};bool m_Pvp=false,m_Race=false,m_AllowEyeWheel=false,m_AllowZoom=false;}m_GameInfo;
 struct {int m_NumPlayers=0;}m_Snap;
 struct {CheckedArray<int,MAX_CLIENTS> m_Teams;int Team(int Id)const{return m_Teams[Id];}}m_Teams;
 struct {CLayers *m_pLayers=nullptr;}m_MapLayersForeground;
 struct {struct {float x=0,y=0;}m_Center;}m_Camera;
 CheckedArray<CClientData,MAX_CLIENTS> m_aClients;
 CheckedArray<int,2> m_aLocalIds;
 std::optional<CServerInfo> m_ConnectServerInfo;
 FakeMap m_Map;FakeMap *Map(){return &m_Map;}
};
class Runner {public:
 IClient m_Client; CGameClient m_Game;
 IClient *Client(){return &m_Client;}CGameClient *GameClient(){return &m_Game;}
`;
const tests = `
};
int main(){
 Runner Runner;int Checks=0,Failures=0;
 auto Null=[&](const char *Key,const CScriptingCtx::Any &Arg){Checks++;try{if(!std::holds_alternative<std::nullptr_t>(Runner.State(Key,Arg)))Failures++;}catch(...){Failures++;}};
 auto Number=[&](const char *Key,int Expected){Checks++;try{const auto Value=Runner.State(Key,nullptr);if(!std::holds_alternative<int>(Value)||std::get<int>(Value)!=Expected)Failures++;}catch(...){Failures++;}};
 auto Text=[&](const char *Key,int Id,const char *Expected){Checks++;try{const auto Value=Runner.State(Key,Id);if(!std::holds_alternative<std::string>(Value)||std::get<std::string>(Value)!=Expected)Failures++;}catch(...){Failures++;}};
 for(const char *Key: {"team","ddnet_team"}) {
  for(int Dummy: {0,1}){
   g_Config.m_ClDummy=Dummy;
   for(auto State:{IClient::STATE_ONLINE,IClient::STATE_DEMOPLAYBACK}){
    Runner.m_Client.m_State=State;
    for(int Id:std::array<int,2>{0,MAX_CLIENTS-1}){
     Runner.m_Game.m_aLocalIds[Dummy]=Id;Runner.m_Game.m_aClients[Id].m_Team=-1;Runner.m_Game.m_Teams.m_Teams[Id]=42;
     Number(Key,std::strcmp(Key,"team")==0?-1:42);
    }
    for(int Id:std::array<int,2>{-1,MAX_CLIENTS}){Runner.m_Game.m_aLocalIds[Dummy]=Id;Null(Key,nullptr);}
   }
   for(auto State:{IClient::STATE_OFFLINE,IClient::STATE_CONNECTING,IClient::STATE_LOADING}){
    Runner.m_Client.m_State=State;Runner.m_Game.m_aLocalIds[Dummy]=0;Null(Key,nullptr);
   }
  }
 }
 for(const char *Key:{"name","clan"}) {
  for(auto State:{IClient::STATE_ONLINE,IClient::STATE_DEMOPLAYBACK}){
   Runner.m_Client.m_State=State;
   for(int Id:std::array<int,3>{-1,MAX_CLIENTS,MAX_CLIENTS+1})Null(Key,Id);
   Null(Key,std::string("0"));Null(Key,nullptr);
   Runner.m_Game.m_aClients[1].m_Active=false;Null(Key,1);
   for(int Id:std::array<int,2>{0,MAX_CLIENTS-1}){
    auto &Data=Runner.m_Game.m_aClients[Id];Data.m_Active=true;std::strcpy(Data.m_aName,"player");std::strcpy(Data.m_aClan,"clan");
    Text(Key,Id,std::strcmp(Key,"name")==0?"player":"clan");
   }
  }
  Runner.m_Client.m_State=IClient::STATE_OFFLINE;Null(Key,0);
 }
 std::printf("State method checks=%d failures=%d\\n",Checks,Failures);return Failures?1:0;
}
`;

const versions = [['green', fs.readFileSync(path.join(root, 'src/game/client/components/tclient/scripting.cpp'), 'utf8')]];
if(options.baseline)
{
	const baseline = cp.spawnSync('git', ['-C', root, 'show', options.baseline + ':src/game/client/components/tclient/scripting.cpp'], {encoding: 'utf8', timeout: 15000});
	if(baseline.status !== 0)
		throw new Error(baseline.stderr || 'Could not read the baseline revision');
	versions.push(['red', baseline.stdout]);
}
let failed = false;
for(const [color, text] of versions)
{
	const source = path.join(output, 'state_' + color + '.cpp');
	fs.writeFileSync(source, prefix + method(text, 'const CServerInfo *GetServerInfo()') + '\n' + method(text, 'CScriptingCtx::Any State(') + tests);
	const exe = path.join(output, 'state_' + color + '.exe');
	const built = run(compiler, ['/nologo', '/std:c++20', '/EHsc', '/utf-8', '/MD', '/O2', '/I' + path.join(root, 'src'), '/Fo' + output + '/', source, '/Fe' + exe], 'state_' + color + '_compile.log');
	console.log('State ' + color + ' compile exit=' + built.status);
	if(built.status !== 0)
	{
		failed = true;
		continue;
	}
	const tested = run(exe, [], 'state_' + color + '_test.log');
	console.log('State ' + color + ' exit=' + tested.status + ' ' + (tested.stdout || '').trim());
	if(color === 'green' ? tested.status !== 0 : tested.status !== 1)
		failed = true;
}
console.log('Validation artifacts: ' + output);
process.exitCode = failed ? 1 : 0;
