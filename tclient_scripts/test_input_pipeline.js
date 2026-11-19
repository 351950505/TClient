// Exercises current CGameClient function bodies with deterministic client/world stubs.
// Windows/MSVC only. Does not emulate networking, prediction physics, audio, or graphics.
// Usage: node test_input_pipeline.js --output <empty-build-dir> [--source-ref <git-ref>]
const fs=require('fs'),cp=require('child_process');
const path=require('path');
const root=path.resolve(__dirname,'..').split(path.sep).join('/');
const outputIndex=process.argv.indexOf('--output');
if(outputIndex<0||!process.argv[outputIndex+1])throw Error('--output is required');
const work=path.resolve(process.argv[outputIndex+1]).split(path.sep).join('/');
fs.mkdirSync(work,{recursive:true});
const refIndex=process.argv.indexOf('--source-ref');
let source;
if(refIndex>=0){const r=cp.spawnSync('git',['-C',root,'show',process.argv[refIndex+1]+':src/game/client/gameclient.cpp'],{encoding:'utf8'});if(r.status!==0)throw Error(r.stderr);source=r.stdout;}else source=fs.readFileSync(root+'/src/game/client/gameclient.cpp','utf8');
const names=['FastInputMode','FastInputActive','GetFastInputPredictionTicks','GetFastInputOtherTicks','IsFastInputLocal','IsSnapshotInputMode','SnapshotInputMaxTick','SnapshotTryGetPredPos','ApplyFastInputOffset','ReadFastInputPos','TcNovaPilotTickOffset','TcNovaPilotSamplePos','GetFastInputPos','GetFreezePos','UpdateRenderedCharacters'];
function extract(name){const m=new RegExp('(?:bool|int|float|void|vec2) CGameClient::'+name+'\\([^\\n]*\\n\\{').exec(source);if(!m)throw Error(name);const begin=m.index,open=source.indexOf('{',begin);let depth=1,end=open+1;while(depth&&end<source.length){if(source[end]==='{')depth++;if(source[end]==='}')depth--;end++;}return source.slice(begin,end);}
const bodies=names.map(extract);const fields=[...new Set(bodies.join('\n').match(/g_Config\.m_\w+/g))].map(s=>s.slice(9));
const declarations=bodies.map(b=>b.slice(0,b.indexOf('\n{')).replace('CGameClient::','').replace('int InterpolationMode) const','int InterpolationMode = 0) const')+';').join('\n');
const cpp=`#include <game/client/fastinput.h>
#include <iostream>
#include <string>
#include <iterator>
#include <algorithm>
constexpr int MAX_CLIENTS=4;
constexpr int HOOK_FLYING=1,HOOK_GRABBED=5;
struct { ${fields.map(f=>'int '+f+'=0;').join('\n')} } g_Config;
long long time_get(){return 100;} long long time_freq(){return 1000;}
template<class T> bool in_range(T v,T lo,T hi){return v>=lo&&v<=hi;}
constexpr int WEAPON_NINJA=5,WEAPON_GUN=1;
struct NetChar{int m_X=0,m_Y=0,m_Angle=0,m_Weapon=0,m_AttackTick=0,m_HookState=0;};
struct Core{void Write(NetChar*p)const{p->m_X=(int)m_Pos.x;p->m_Y=(int)m_Pos.y;}int m_ActiveWeapon=0;vec2 m_Pos=vec2(0,0),m_Vel=vec2(0,0);int m_FreezeEnd=0,m_HookState=0;int HookedPlayer()const{return -1;}};
struct CCharacter{bool m_NinjaJetpack=false;int m_FreezeAccumulation=0,m_AliveAccumulation=0,m_FreezeTime=0;Core core;Core*Core(){return &core;}bool IsInterfering()const{return true;}int GetAttackTick()const{return 0;}};
struct World{CCharacter character;CCharacter*GetCharacterById(int){return &character;}};
struct FakeClient{bool m_IsLocalFrozen=false;int pred=100,freeze=98;float intra=.25f;int PredGameTick(int)const{return pred;}float PredIntraGameTick(int)const{return intra;}float IntraGameTick(int)const{return intra;}int PrevGameTick(int)const{return 90;}int GameTickSpeed()const{return 50;}void GetSmoothFreezeTick(int*t,float*i,float){*t=freeze;*i=intra;}};
class CGameClient{public:
struct Data{bool m_IsPredicted=false,m_IsPredictedLocal=false,m_ValidAntipingSmooth=false;int m_FreezeEnd=0;NetChar m_RenderCur,m_RenderPrev;vec2 m_RenderPos=vec2(0,0),m_PrevImprovedPredPos=vec2(0,0),m_ImprovedPredPos=vec2(0,0);Core m_PrevPredicted,m_Predicted,m_RegularPredicted;vec2 m_aPredPos[200]{};int m_aPredTick[200]{};long long m_aSmoothLen[2]{},m_aSmoothStart[2]{};};
Data m_aClients[MAX_CLIENTS];struct{bool m_Active=false;NetChar m_Cur,m_Prev;}snapChars[MAX_CLIENTS];struct{int m_LocalClientId=0;decltype(snapChars) m_aCharacters;}m_Snap;vec2 m_LocalCharacterPos=vec2(0,0);int m_aLocalIds[2]{0,1};bool m_aReceivedPreInput[2]{};int m_aTcNovaPilotLatencySamples[16]{};int m_TcNovaPilotLatencySampleCount=0;int m_SmoothTick=0;float m_SmoothIntraTick=0;
bool Predict()const{return true;}int AntiPingPlayers()const{return 1;}bool AntiPingGunfire()const{return false;}bool IsOtherTeam(int)const{return false;}vec2 ApplyAncientInputPost(int,vec2 p){return p;}FakeClient client;FakeClient*m_pClient=&client;FakeClient*Client()const{return const_cast<FakeClient*>(&client);}World m_PredictedWorld,m_ExtraPredictedWorld;
struct Col{int game=0,front=0;int GetPureMapIndex(vec2)const{return 0;}int GetTileIndex(int)const{return game;}int GetFrontTileIndex(int)const{return front;}}collision;const Col*Collision()const{return &collision;}
bool PredictDummy()const{return false;}bool IsSnowHookActive()const{return false;}bool IsSnowHookAffected(int)const{return false;}bool TcLightUseHookLogic()const{return false;}bool TcLightIsHookLinkedClient(int)const{return false;}int TcLightHookTickOffset()const{return 1;}int GetSnowHookTicks()const{return 1;}bool FastInputImmediateOthers()const{return false;}float GetAncientInputAmount(int){return 1;}vec2 GetSmoothPos(int){return vec2(0,0);}vec2 GetFInputPos(int){return vec2(0,0);}vec2 TcBclAncientSamplePos(int,float,bool)const{return vec2(0,0);}
${declarations}
};
${bodies.join('\n')}
int failures=0,cases=0;void check(bool v,const char*name){++cases;if(!v){++failures;std::cout<<"FAIL "<<name<<"\\n";}}
void init(CGameClient &g,int mode){g_Config={};g_Config.m_TcFastInput=1;g_Config.m_TcFastInputMode=mode;g_Config.m_TcJukkiInput=1000;g_Config.m_TcNovapilotInput=100;g_Config.m_TcUnfreezeLagDelayTicks=25;for(auto&d:g.m_aClients){d.m_PrevPredicted.m_Pos=vec2(-10,0);d.m_Predicted.m_Pos=vec2(0,0);for(int t=90;t<=110;t++){d.m_aPredTick[t%200]=t;d.m_aPredPos[t%200]=vec2(float(t-100)*10,0);}}}
int main(){
for(int mode:{15,16})for(int others:{0,1})for(int enabled:{0,1}){
 CGameClient g;init(g,mode);g_Config.m_TcFastInput=enabled;g_Config.m_TcJukkiInputOthers=others;g_Config.m_TcNovapilotInputOthers=others;
 check(g.FastInputActive(0,false)==bool(enabled),"activation local");check(g.FastInputActive(2,true)==bool(enabled&&others),"activation remote");check(g.GetFastInputPredictionTicks()==enabled,"local horizon");check(g.GetFastInputOtherTicks()==(enabled&&others),"remote horizon");
 int tick=100;float intra=.25f;g.ApplyFastInputOffset(0,tick,intra);check(tick==100+enabled&&intra==.25f,"local offset exactly once");tick=100;g.ApplyFastInputOffset(2,tick,intra);check(tick==100+(enabled&&others),"remote offset gate");
 vec2 p=g.GetFastInputPos(0);check(p==vec2(enabled?2.5f:-7.5f,0),"actual local sample");
 p=g.GetFastInputPos(2);check(p==vec2(enabled&&others?2.5f:-7.5f,0),"actual remote sample");
}
{CGameClient g;init(g,15);g.m_aClients[0].m_aPredTick[101]=0;g.m_aClients[0].m_aPredPos[101]=vec2(999,0);check(g.GetFastInputPos(0)==vec2(-7.5f,0),"JuKKi stale current slot falls back");}
{CGameClient g;init(g,16);g_Config.m_TcNovapilotClamp=1;g.collision.game=TILE_FREEZE;check(g.GetFastInputPos(0)==vec2(-7.5f,0),"Nova local game freeze clamp");g.collision.game=0;g.collision.front=TILE_DFREEZE;check(g.GetFastInputPos(0)==vec2(-7.5f,0),"Nova local front freeze clamp");g_Config.m_TcRemoveAnti=1;check(g.GetFreezePos(0)==vec2(-7.5f,0),"Nova freeze compensation retains clamp");g_Config.m_TcNovapilotClamp=0;check(g.GetFreezePos(0)==vec2(-17.5f,0),"Nova freeze compensation retains adjusted clock");}
{CGameClient g;init(g,16);g.m_aClients[0].m_aPredTick[101]=0;check(g.GetFastInputPos(0)==vec2(-7.5f,0),"Nova stale current slot falls back");g_Config.m_TcFastInputMode=4;check(!g.FastInputActive(0,false)&&g.GetFastInputPredictionTicks()==0,"reserved mode stays inactive");}
for(int mode:{15,16})for(int amount:{0,1,50,100,300,1000,5000})for(int other:{0,1})for(float intra:{0.0f,0.25f,0.99f}){
 CGameClient g;init(g,mode);g.client.intra=intra;g_Config.m_TcJukkiInput=std::min(amount,5000);g_Config.m_TcNovapilotInput=std::min(amount,300);g_Config.m_TcJukkiInputOthers=other;g_Config.m_TcNovapilotInputOthers=other;
 for(int id:{0,2}){const bool active=g.FastInputActive(id,id!=0);int t=100;float f=intra;g.ApplyFastInputOffset(id,t,f);check(t<=100+(id==0?g.GetFastInputPredictionTicks():g.GetFastInputOtherTicks()),"sample offset never exceeds allocated horizon");check(active||t==100,"inactive offset stays unchanged");check(FastInput::FinitePos(g.GetFastInputPos(id)),"finite normal sample matrix");g_Config.m_TcRemoveAnti=1;check(FastInput::FinitePos(g.GetFreezePos(id)),"finite freeze sample matrix");}
}
{CGameClient g;init(g,16);g_Config.m_TcNovapilotCurved=1;g_Config.m_TcRemoveAnti=1;g_Config.m_TcNovapilotInputOthers=0;g.m_aClients[2].m_aPredPos[96]=vec2(-200,0);g.m_aClients[2].m_aPredPos[99]=vec2(-10,0);check(g.GetFreezePos(2)==vec2(-27.5f,0),"disabled Nova others retain linear freeze compensation");}
for(int mode:{15,16})for(int other:{0,1})for(int freeze:{0,1})for(int intensity:{50,100,200}){
 CGameClient g;init(g,mode);g_Config.m_TcJukkiInputOthers=other;g_Config.m_TcNovapilotInputOthers=other;g_Config.m_TcRemoveAnti=freeze;g_Config.m_TcHookPullVisual=intensity;for(int id:{0,2}){g.m_Snap.m_aCharacters[id].m_Active=true;g.m_Snap.m_aCharacters[id].m_Cur.m_HookState=HOOK_GRABBED;}
 g.UpdateRenderedCharacters();float expected=freeze?-17.5f:-7.5f+10.0f*(intensity/100.0f);if(std::fabs(g.m_aClients[0].m_RenderPos.x-expected)>.001f||g.m_aClients[2].m_RenderPos!=vec2(other?2.5f:(freeze?-27.5f:-7.5f),0))std::cout<<"matrix mode="<<mode<<" other="<<other<<" freeze="<<freeze<<" intensity="<<intensity<<" local="<<g.m_aClients[0].m_RenderPos.x<<" remote="<<g.m_aClients[2].m_RenderPos.x<<" expectedLocal="<<expected<<"\\n";check(std::fabs(g.m_aClients[0].m_RenderPos.x-expected)<.001f,"final local renderer applies intensity once");check(g.m_LocalCharacterPos==g.m_aClients[0].m_RenderPos,"local camera and tee position agree");check(g.m_aClients[2].m_RenderPos==vec2(other?2.5f:(freeze?-27.5f:-7.5f),0),"final remote renderer respects others gate");
}
std::cout<<"cases="<<cases<<" failures="<<failures<<"\\n";return failures?1:0;}
`;
fs.writeFileSync(work+'/input_actual_calls.cpp',cpp);
fs.writeFileSync(work+'/build.cmd','@echo off\r\ncall "C:\\VS2026\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\nif errorlevel 1 exit /b %errorlevel%\r\ncl /nologo /std:c++20 /EHsc /utf-8 /I"'+root+'/src" "'+work+'/input_actual_calls.cpp" /Fo:"'+work+'/input_actual_calls.obj" /Fe:"'+work+'/input_actual_calls.exe"\r\nif errorlevel 1 exit /b %errorlevel%\r\n"'+work+'/input_actual_calls.exe"\r\nexit /b %errorlevel%\r\n');
const fd=fs.openSync(work+'/integration.log','w');const r=cp.spawnSync('cmd.exe',['/d','/c',work+'/build.cmd'],{stdio:['ignore',fd,fd]});fs.closeSync(fd);console.log('exit='+r.status+'\n'+fs.readFileSync(work+'/integration.log','utf8').split('\n').slice(-22).join('\n'));process.exit(r.status||0);
