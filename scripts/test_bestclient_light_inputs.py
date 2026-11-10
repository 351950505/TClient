"""从当前生产源码提取快速输入方法，在隔离夹具中执行回归。

只模拟网络时钟、快照与图形数据，不启动客户端。不能替代真实双客户端验收。
Windows 运行：python -I scripts/test_bestclient_light_inputs.py
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.repo
    game = (root / "src/game/client/gameclient.cpp").read_text(encoding="utf-8")
    controls = (root / "src/game/client/components/controls.cpp").read_text(encoding="utf-8")
    on_predict = extract(game, "void CGameClient::OnPredict()")
    if "PredPos = GetFastInputPos(i);" in on_predict:
        raise RuntimeError("snapshot Improved must not call the final fast-input position path")
    for required in ("ApplyFastInputOffset(i, PredTick, PredIntra);", "SnapshotTryGetPredPos(i, PredTick, PredIntra, PredPos)"):
        if required not in on_predict:
            raise RuntimeError(f"missing snapshot Improved authority path: {required}")
    signatures = [
        "int CGameClient::FastInputMode()", "bool CGameClient::FastInputActive(",
        "bool CGameClient::IsSnapshotInputMode()", "bool CGameClient::FastInputImmediateOthers()",
        "int CGameClient::SnapshotInputMaxTick(", "bool CGameClient::SnapshotTryGetPredPos(",
        "int CGameClient::GetFastInputPredictionTicks()", "int CGameClient::GetFastInputOtherTicks()",
        "bool CGameClient::IsFastInputLocal(", "bool CGameClient::HasSnowHookInteraction(",
        "bool CGameClient::IsSnowHookActive()", "bool CGameClient::IsSnowHookAffected(",
        "int CGameClient::GetSnowHookTicks()", "void CGameClient::ApplyFastInputOffset(",
        "bool CGameClient::ReadFastInputPos(", "float CGameClient::GetAncientInputAmount(",
        "vec2 CGameClient::GetSmoothPos(", "vec2 CGameClient::GetFastInputPos(",
        "vec2 CGameClient::GetFreezePos(",
        "vec2 CGameClient::ApplyAncientInputPost(", "void CGameClient::UpdateRenderedCharacters()",
    ]
    methods = [extract(game, signature) for signature in signatures]
    methods = [
        method.replace("{", "{\n\t++m_SmoothCalls;", 1)
        if method.startswith("vec2 CGameClient::GetSmoothPos(") else method
        for method in methods
    ]
    control_methods = [extract(controls, s) for s in (
        "bool CControls::CheckNewInput()", "void CControls::OnReset()", "void CControls::ResetInput(", "void CControls::ResetFastInput(")]
    declarations = []
    for method in methods:
        decl = method[:method.index("{")].strip().replace("CGameClient::", "")
        decl = decl.replace("bool Salatik)", "bool Salatik = false)")
        decl = decl.replace("int InterpolationMode)", "int InterpolationMode = 0)")
        declarations.append(decl + ";")
    fields = sorted(set(re.findall(r"g_Config\.(m_\w+)", "\n".join(methods + control_methods))))
    config = "struct CConfig {\n" + "\n".join(
        ("float" if field == "m_TcMeowInput" else "int") + " " + field + " = 0;" for field in fields
    ) + "\n} g_Config;\n"
    header = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <game/client/fastinput.h>
constexpr int MAX_CLIENTS = 128, NUM_DUMMIES = 2, INPUT_STATE_MASK = 63;
struct vec2 {
    float x=0,y=0;
    vec2() = default;
    vec2(float X, float Y):x(X),y(Y){}
    float &operator[](int Axis) { return Axis ? y : x; }
    float operator[](int Axis) const { return Axis ? y : x; }
    vec2 operator+(vec2 Other) const { return {x+Other.x,y+Other.y}; }
    vec2 operator-(vec2 Other) const { return {x-Other.x,y-Other.y}; }
    vec2 operator*(float Factor) const { return {x*Factor,y*Factor}; }
    vec2 &operator*=(float Factor) { x*=Factor; y*=Factor; return *this; }
};
float length(vec2 Pos) { return std::sqrt(Pos.x*Pos.x+Pos.y*Pos.y); }
float distance(vec2 A,vec2 B) { return length(A-B); }
template<class T> T mix(T A,T B,float Factor) { return A+(B-A)*Factor; }
long long time_get(){return 100;}
long long time_freq(){return 1000;}
bool in_range(long long V,long long L,long long H){return V>=L && V<=H;}
struct CNetObj_PlayerInput {
    int m_Direction=0,m_Hook=0,m_Fire=0,m_Jump=0,m_NextWeapon=0,m_PrevWeapon=0,m_WantedWeapon=0,m_TargetX=0,m_TargetY=0;
};
struct CCore {
    vec2 m_Pos,m_Vel;
    int m_FreezeEnd=0,m_HookState=0,m_HookedPlayer=-1;
    int m_X=0,m_Y=0,m_Angle=0,m_Weapon=0,m_AttackTick=0,m_ActiveWeapon=0;
    int HookedPlayer() const {return m_HookedPlayer;}
    void Write(CCore *Other) const {*Other=*this;Other->m_X=int(m_Pos.x);Other->m_Y=int(m_Pos.y);}
};
constexpr int WEAPON_GUN=1, WEAPON_NINJA=5;
struct CCharacter {
    int m_FreezeAccumulation=0,m_AliveAccumulation=0,m_FreezeTime=0;
    bool m_NinjaJetpack=false;
    CCore m_Core;
    CCore *Core(){return &m_Core;}
    int GetAttackTick() const {return 0;}
    bool IsInterfering() const {return false;}
};
struct CWorld {
    CCharacter m_Character;
    CCharacter *GetCharacterById(int){return &m_Character;}
};
struct CClock {
    int m_Tick=100,m_Speed=50,m_Prev=90,m_FreezeTick=98;
    float m_Intra=0.25f,m_FreezeIntra=0.5f;
    bool m_IsLocalFrozen=false;
    int PredGameTick(int) const {return m_Tick;}
    int PrevGameTick(int) const {return m_Prev;}
    float PredIntraGameTick(int) const {return m_Intra;}
    float IntraGameTick(int) const {return 0.5f;}
    int GameTickSpeed() const {return m_Speed;}
    void GetSmoothTick(int *Tick,float *Intra,float){*Tick=m_Tick;*Intra=m_Intra;}
    void GetSmoothFreezeTick(int *Tick,float *Intra,float){*Tick=m_FreezeTick;*Intra=m_FreezeIntra;}
};
struct CGameClient;
struct CControls {
    static constexpr int NUM_FAST_INPUT_MODES=15;
    CGameClient *m_Game=nullptr;
    CGameClient *GameClient(){return m_Game;}
    CNetObj_PlayerInput m_aInputData[2]{},m_aLastData[2]{},m_aFastInput[2]{},m_aaModeFastInput[15][2]{};
    vec2 m_aMousePos[2];
    int m_aInputDirectionLeft[2]{},m_aInputDirectionRight[2]{},m_aAmmoCount[6]{},m_LastSendTime=0;
    bool m_FastInputHookAction[2]{},m_FastInputFireAction[2]{},m_aModeFastInputHookAction[15][2]{},m_aModeFastInputFireAction[15][2]{};
    int m_ActiveFastInputMode=0;
    CNetObj_PlayerInput &GetFastInput(int Slot){return m_ActiveFastInputMode==0 ? m_aFastInput[Slot] : m_aaModeFastInput[m_ActiveFastInputMode][Slot];}
    bool CheckNewInput(); void OnReset(); void ResetInput(int Dummy); void ResetFastInput(int Dummy);
};
struct CGameClient {
    CClock m_Clock;
    CClock *m_pClient=&m_Clock;
    CClock *Client() const {return const_cast<CClock *>(&m_Clock);}
    bool m_Dummy=true;
    bool PredictDummy() const {return m_Dummy;}
    bool Predict() const {return true;}
    int AntiPingPlayers() const {return 1;}
    bool AntiPingGunfire() const {return false;}
    bool IsOtherTeam(int) const {return false;}
    int m_aLocalIds[2]={0,1};
    struct CSnap { int m_LocalClientId=0; struct CChar {bool m_Active=false; CCore m_Cur,m_Prev;} m_aCharacters[MAX_CLIENTS];} m_Snap;
    struct CData {
        CCore m_Predicted,m_PrevPredicted,m_RegularPredicted,m_PrevRegularPredicted;
        vec2 m_aPredPos[200]; int m_aPredTick[200]{};
        long long m_aSmoothLen[2]{},m_aSmoothStart[2]{};
        CCore m_RenderCur,m_RenderPrev;
        vec2 m_RenderPos,m_PrevImprovedPredPos,m_ImprovedPredPos;
        int m_FreezeEnd=0;
        bool m_IsPredicted=false,m_IsPredictedLocal=false,m_ValidAntipingSmooth=false;
    } m_aClients[MAX_CLIENTS];
    CControls m_Controls;
    FastInput::CAncientState m_aAncientInputState[2],m_aLightAncientInputState[2];
    CWorld m_PredictedWorld,m_ExtraPredictedWorld;
    int m_SmoothTick=0; float m_SmoothIntraTick=0;
    vec2 m_LocalCharacterPos;
    bool m_aReceivedPreInput[NUM_DUMMIES]{};
    int m_SmoothCalls=0;
    vec2 GetFInputPos(int){return {};}
    CGameClient(){m_Controls.m_Game=this;}
'''
    tests = r'''
int g_Checks=0,g_Failures=0;
void Check(bool Pass,const char *What,int Mode=-1) {
    ++g_Checks;
    if(Pass)return;
    ++g_Failures;
    if(Mode>=0)std::printf("FAIL: mode %d %s\n",Mode,What);
    else std::printf("FAIL: %s\n",What);
}
void Near(float Actual,float Expected,const char *What="near value",int Mode=-1) {
    char Message[256];
    std::snprintf(Message,sizeof(Message),"%s (actual %.6f expected %.6f)",What,Actual,Expected);
    Check(std::fabs(Actual-Expected)<=0.0002f,Message,Mode);
}
#undef assert
#define assert(Condition) Check((Condition),#Condition)
void Ready(CGameClient &Game,int Mode) {
    g_Config={};g_Config.m_TcFastInput=1;g_Config.m_TcFastInputMode=Mode;g_Config.m_TcUnfreezeLagDelayTicks=25;
    for(int Id=0;Id<4;++Id) {
        Game.m_Snap.m_aCharacters[Id].m_Active=true;
        for(int Tick=90;Tick<120;++Tick){Game.m_aClients[Id].m_aPredTick[Tick%200]=Tick;Game.m_aClients[Id].m_aPredPos[Tick%200]=vec2(float(Tick),0);}
        Game.m_aClients[Id].m_RegularPredicted.m_Pos={100,0};
        Game.m_aClients[Id].m_Predicted.m_Pos={100,0};
        Game.m_aClients[Id].m_PrevPredicted.m_Pos={99,0};
    }
}
int main(){
    const int RenderStart=g_Checks;
    for(int Mode:{12,13,14}) {
        for(int Others:{0,1}) for(int Improved:{0,1}) for(int RemoveAnti:{0,1}) for(int Frozen:{0,1}) {
            CGameClient G;Ready(G,Mode);
            g_Config.m_TcLightInput=150;g_Config.m_TcAzureInput=150;g_Config.m_TcLightAncientInput=30;
            g_Config.m_TcLightInputOthers=Others;g_Config.m_TcAzureInputOthers=Others;
            g_Config.m_TcLightAncientInputOthers=Others;g_Config.m_TcLightAncientInputOthersAmount=30;
            g_Config.m_TcAntiPingImproved=Improved;g_Config.m_ClAntiPingSmooth=1;g_Config.m_TcRemoveAnti=RemoveAnti;
            G.m_PredictedWorld.m_Character.m_FreezeTime=Frozen;
            auto &Data=G.m_aClients[2];
            Data.m_ValidAntipingSmooth=true;
            Data.m_PrevImprovedPredPos=Data.m_ImprovedPredPos={500,0};
            Data.m_aSmoothStart[0]=Data.m_aSmoothStart[1]=99;
            // 上游快照模式只允许 Others 使用 Improved；远端冻结启用 Others 时仍走其自身时间线。
            float Expected=Others ? (Improved ? 500.0f : G.GetFastInputPos(2).x) : 99.25f;
            if(RemoveAnti && !Others) Expected=97.5f;
            if(RemoveAnti && Frozen && Others) Expected=G.GetFastInputPos(2).x;
            if(Mode==14 && Others && !(RemoveAnti && Frozen))
                Expected=G.ApplyAncientInputPost(2,{Improved ? 500.0f : (RemoveAnti ? G.GetFastInputPos(2).x : 99.25f),0}).x;
            G.UpdateRenderedCharacters();
            Check(std::fabs(Data.m_RenderPos.x-Expected)<0.0002f,"final remote render matches snapshot rules",Mode);
            Check(!Data.m_aSmoothStart[0] && !Data.m_aSmoothStart[1],"clears previous smoothing windows",Mode);
            Check(Others || !Data.m_ValidAntipingSmooth,"Others off invalidates Improved cache",Mode);
            Check(G.m_SmoothCalls==0,"does not consume legacy smooth position",Mode);
            if(Others) Check(std::fabs(G.GetFreezePos(2).x-G.GetFastInputPos(2).x)<0.0002f,"remote freeze uses its own fast-input timeline",Mode);
        }
        CGameClient G;Ready(G,Mode);
        g_Config.m_TcLightInput=100;g_Config.m_TcAzureInput=100;g_Config.m_TcLightAncientInput=20;
        g_Config.m_TcAntiPingImproved=1;g_Config.m_ClAntiPingSmooth=1;
        G.m_aClients[2].m_ValidAntipingSmooth=true;
        G.m_aClients[2].m_PrevImprovedPredPos=G.m_aClients[2].m_ImprovedPredPos={500,0};
        G.UpdateRenderedCharacters();
        Check(std::fabs(G.m_aClients[2].m_RenderPos.x-99.25f)<0.0002f,"mode switch discards prior Improved position",Mode);
        g_Config.m_TcFastInput=0;
        G.m_aClients[2].m_ValidAntipingSmooth=true;
        G.UpdateRenderedCharacters();
        Check(std::fabs(G.m_aClients[2].m_RenderPos.x-500.0f)<0.0002f,"disabled mode preserves legacy smoothing",Mode);
        g_Config.m_TcFastInput=1;g_Config.m_TcRemoveAnti=1;
        Check(std::fabs(G.GetFreezePos(0).x-98.5f)<0.0002f,"local freeze retains compensation",Mode);
        g_Config.m_ClDummy=1;G.m_Snap.m_LocalClientId=1;
        Check(std::fabs(G.GetFreezePos(0).x-98.5f)<0.0002f,"dummy freeze retains local compensation",Mode);
    }
    {
        CGameClient G;Ready(G,12);g_Config.m_TcLightInput=150;g_Config.m_TcLightInputHook=1;g_Config.m_TcLightInputHookAmount=37;
        G.m_Snap.m_aCharacters[0].m_Cur.m_HookState=5;G.m_Snap.m_aCharacters[0].m_Cur.m_HookedPlayer=2;
        g_Config.m_TcAntiPingImproved=1;g_Config.m_TcRemoveAnti=1;
        G.m_PredictedWorld.m_Character.m_FreezeTime=1;
        const float Expected=G.GetFastInputPos(2).x;
        G.UpdateRenderedCharacters();
        Check(std::fabs(G.m_aClients[2].m_RenderPos.x-Expected)<0.0002f,"hook-linked remote keeps its timeline with Others off",12);
    }
    const int RenderChecks=g_Checks-RenderStart;
    Check(RenderChecks==229,"snapshot render executed 229 checks");
    std::printf("CHECKED: %d snapshot render, freeze, cache and mode-switch checks\n",RenderChecks);
    // Authority: BestClient-Light c56daba94b2b0ff7b5590a16ca463d9ff5182b98.
    {
        const int Start=g_Checks;
        for(int Mode : {0,7}) {
            CGameClient G;Ready(G,Mode);g_Config.m_TcRemoveAnti=1;
            if(Mode==0){g_Config.m_TcFastInputAmount=25;g_Config.m_TcFastInputOthers=1;}
            else {g_Config.m_TcFInput=1250;g_Config.m_TcFInputOthers=1;}
            Check(G.GetFastInputPredictionTicks()==2 && G.GetFastInputOtherTicks()==2,"shared Fast/F prediction horizon",Mode);
            int T=100;float F=.25f;G.ApplyFastInputOffset(0,T,F);
            Check(T==101 && std::fabs(F-.5f)<.0002f,"shared Fast/F fractional offset",Mode);
            Near(G.GetFreezePos(0).x,98.75f,"local freeze keeps Fast/F offset",Mode);
            Near(G.GetFreezePos(2).x,98.75f,"remote freeze keeps Fast/F Others offset",Mode);
        }
        Check(g_Checks-Start==8,"Fast/F executed 8 authority checks");
        std::puts("CHECKED: modes 0/7 shared offset and freeze paths");
    }
    {
        const int Start=g_Checks;
        CGameClient G;Ready(G,8);g_Config.m_TcBestInput=200;g_Config.m_TcBestInputSmoothing=50;g_Config.m_TcBestInputLatencyComp=20;
        Check(G.GetFastInputPredictionTicks()==2,"Best smoothing/latency prediction horizon",8);
        int T=100;float F=.25f;G.ApplyFastInputOffset(0,T,F);
        Check(T==102,"Best smoothing/latency whole ticks",8);Near(F,.05f,"Best smoothing/latency fraction",8);
        for(int Curve : {1,2,3}) {
            CGameClient CurveGame;Ready(CurveGame,8);g_Config.m_TcBestInput=100;g_Config.m_TcBestInputOthers=1;g_Config.m_TcBestInputInterpolation=Curve;
            CurveGame.m_aClients[0].m_aPredPos[100]={0,0};CurveGame.m_aClients[0].m_aPredPos[101]={500,0};
            const float Expected=Curve==1 ? 125.0f : Curve==2 ? 78.125f : 101.5625f;
            Near(CurveGame.GetFastInputPos(0).x,Expected,"Best configured interpolation",Curve);
        }
        Check(g_Checks-Start==6,"Best executed 6 offset/interpolation checks");
        std::puts("CHECKED: mode 8 Best amount and interpolation configurations");
    }
    {
        const int Start=g_Checks;
        for(int Mode : {8,9,10}) {
            CGameClient G;Ready(G,Mode);
            if(Mode==8){g_Config.m_TcBestInput=100;g_Config.m_TcBestInputOthers=1;g_Config.m_TcBestInputInterpolation=2;}
            if(Mode==9){g_Config.m_TcSaikoInput=100;g_Config.m_TcSaikoInputOthers=1;}
            if(Mode==10){g_Config.m_TcDeltaInput=100;g_Config.m_TcDeltaInputOthers=1;}
            g_Config.m_ClAntiPingSmooth=1;g_Config.m_TcAntiPingImproved=1;
            G.m_aClients[2].m_ValidAntipingSmooth=true;
            G.m_aClients[2].m_PrevImprovedPredPos=G.m_aClients[2].m_ImprovedPredPos={500,0};
            const float Expected=G.GetFastInputPos(2).x;
            Check(G.FastInputImmediateOthers(),"Best/Saiko/Delta marks Others immediate",Mode);
            G.UpdateRenderedCharacters();
            Near(G.m_aClients[2].m_RenderPos.x,Expected,"ImmediateOthers overrides Improved smoothing",Mode);
        }
        Check(g_Checks-Start==6,"Best/Saiko/Delta executed 6 ImmediateOthers checks");
        std::puts("CHECKED: modes 8/9/10 ImmediateOthers scheduling");
    }
    {
        const int Start=g_Checks;
        for(int Mode : {0,8,9,10,11}) {
            CGameClient G;Ready(G,Mode);
            if(Mode==0){g_Config.m_TcFastInputAmount=20;g_Config.m_TcFastInputOthers=1;}
            if(Mode==8){g_Config.m_TcBestInput=100;g_Config.m_TcBestInputOthers=1;}
            if(Mode==9){g_Config.m_TcSaikoInput=100;g_Config.m_TcSaikoInputOthers=1;}
            if(Mode==10){g_Config.m_TcDeltaInput=100;g_Config.m_TcDeltaInputOthers=1;}
            if(Mode==11){g_Config.m_TcCloudInput=100;g_Config.m_TcCloudInputOthers=1;G.m_aReceivedPreInput[0]=true;}
            int T=100;float F=.25f;G.ApplyFastInputOffset(2,T,F);
            vec2 P;
            Check(G.ReadFastInputPos(2,T,F,100+G.GetFastInputOtherTicks(),P),"exact ring accepts matching slots",Mode);
            G.m_aClients[2].m_aPredTick[(T-1)%200]=T-2;
            Check(!G.ReadFastInputPos(2,T,F,100+G.GetFastInputOtherTicks(),P),"exact ring rejects stale slot",Mode);
        }
        CGameClient G;Ready(G,7);g_Config.m_TcFInput=1000;g_Config.m_TcFInputOthers=1;
        int T=100;float F=.25f;G.ApplyFastInputOffset(2,T,F);G.m_aClients[2].m_aPredTick[(T-1)%200]=T-2;vec2 P;
        Check(G.ReadFastInputPos(2,T,F,100+G.GetFastInputOtherTicks(),P),"F keeps authoritative range guard",7);
        Check(g_Checks-Start==11,"ring policy executed 11 authority checks");
        std::puts("CHECKED: exact ring policy for Fast/Best/Saiko/Delta/Cloud and range policy for F");
    }
    {
        const int Start=g_Checks;
        CGameClient G;Ready(G,11);g_Config.m_TcCloudInput=100;g_Config.m_TcCloudInputOthers=1;
        Check(G.IsSnapshotInputMode(),"Cloud is a snapshot mode",11);
        Check(G.GetFastInputOtherTicks()==0,"Cloud Others waits for PreInput",11);
        int T=100;float F=.25f;G.ApplyFastInputOffset(2,T,F);
        Check(T==100 && std::fabs(F-.25f)<.0002f,"Cloud remote offset waits for PreInput",11);
        g_Config.m_ClAntiPingSmooth=1;g_Config.m_TcAntiPingImproved=1;
        G.m_aClients[2].m_ValidAntipingSmooth=true;
        G.m_aClients[2].m_PrevImprovedPredPos=G.m_aClients[2].m_ImprovedPredPos={500,0};
        G.UpdateRenderedCharacters();
        Near(G.m_aClients[2].m_RenderPos.x,99.25f,"Cloud before PreInput stays on predicted snapshot",11);
        Check(G.m_SmoothCalls==0,"Cloud bypasses legacy smooth position",11);
        G.m_aReceivedPreInput[0]=true;
        Check(G.GetFastInputOtherTicks()==1,"Cloud Others starts after PreInput",11);
        T=100;F=.25f;G.ApplyFastInputOffset(2,T,F);
        Check(T==101 && std::fabs(F-.25f)<.0002f,"Cloud remote offset is exactly one tick",11);
        Near(G.GetFastInputPos(2).x,100.25f,"Cloud samples remote snapshot after PreInput",11);
        G.m_aClients[2].m_aPredTick[101]=100;
        Near(G.GetFastInputPos(2).x,99.25f,"Cloud keeps current-tick fallback when the future slot is stale",11);
        G.m_aClients[2].m_aPredTick[101]=101;G.m_aClients[2].m_ValidAntipingSmooth=true;
        G.UpdateRenderedCharacters();
        Near(G.m_aClients[2].m_RenderPos.x,500.0f,"Cloud remains non-immediate with Improved smoothing",11);
        Check(g_Checks-Start==10,"Cloud executed 10 snapshot/PreInput/ring checks");
        std::puts("CHECKED: mode 11 Cloud snapshot, PreInput gate and strict ring");
    }
    {
        CGameClient G;Ready(G,12);g_Config.m_TcLightInput=150;
        assert(G.GetFastInputPredictionTicks()==2 && G.GetFastInputOtherTicks()==0);
        int T=100;float F=.75f;G.ApplyFastInputOffset(0,T,F);assert(T==102);Near(F,.25f);
        T=100;F=.25f;G.ApplyFastInputOffset(2,T,F);assert(T==100);Near(F,.25f);
        g_Config.m_TcLightInputOthers=1;G.ApplyFastInputOffset(2,T,F);assert(T==102);Near(F,.25f);
        g_Config.m_TcLightInputOthers=0;G.m_Snap.m_aCharacters[0].m_Cur.m_HookState=5;G.m_Snap.m_aCharacters[0].m_Cur.m_HookedPlayer=2;
        g_Config.m_TcLightInputHook=1;g_Config.m_TcLightInputHookAmount=37;
        assert(G.IsSnowHookAffected(2) && !G.IsSnowHookAffected(3));
        assert(G.SnapshotInputMaxTick(2)==102);
        T=100;F=.25f;G.ApplyFastInputOffset(2,T,F);assert(T==102);Near(F,.10f);
        vec2 P;assert(G.SnapshotTryGetPredPos(2,102,.1f,P));
        G.m_aClients[2].m_aPredTick[102]=302;assert(!G.SnapshotTryGetPredPos(2,102,.1f,P));
        g_Config.m_TcLightInput=0;assert(!G.FastInputActive(0,false) && !G.IsSnowHookActive());
        std::puts("PASS: Light local, others, hook-linked and stale slots");
    }
    {
        CGameClient G;Ready(G,13);g_Config.m_TcAzureInput=150;g_Config.m_TcAzureInputOthers=1;
        assert(G.GetFastInputPredictionTicks()==3 && G.GetFastInputOtherTicks()==2);
        int T=100;float F=.75f;G.ApplyFastInputOffset(0,T,F);assert(T==102);Near(F,.25f);
        T=100;F=.75f;G.ApplyFastInputOffset(2,T,F);assert(T==102);Near(F,.25f);
        g_Config.m_TcAzureInput=0;assert(G.GetFastInputPredictionTicks()==0);
        std::puts("PASS: Azure self extra tick and fractional others offset");
    }
    {
        CGameClient G;Ready(G,14);g_Config.m_TcLightAncientInput=37;g_Config.m_TcLightAncientInputOthers=1;g_Config.m_TcLightAncientInputOthersAmount=12;
        for(int Speed : {25,50,100}) {
            G.m_Clock.m_Speed=Speed;
            int T=100;float F=.25f;G.ApplyFastInputOffset(2,T,F);
            const float Total=.25f+12.0f*Speed/1000.0f;
            assert(T==100+int(Total));Near(F,Total-int(Total));
        }
        G.m_Clock.m_Speed=50;assert(G.GetFastInputPredictionTicks()==2 && G.GetFastInputOtherTicks()==1);
        g_Config.m_TcLightAncientInputOthersAmount=100;Near(G.GetAncientInputAmount(2),37);
        g_Config.m_TcLightAncientInputOthersAmount=12;
        int SnapshotTick=100;float SnapshotIntra=.25f;G.ApplyFastInputOffset(2,SnapshotTick,SnapshotIntra);vec2 SnapshotPos;
        assert(G.SnapshotTryGetPredPos(2,SnapshotTick,SnapshotIntra,SnapshotPos));Near(SnapshotPos.x,99.85f,"Ancient snapshot layer samples before final safety guard",14);
        G.m_aClients[2].m_Predicted.m_FreezeEnd=10;
        Near(G.GetFastInputPos(2).x,100);
        g_Config.m_TcRemoveAnti=1;Near(G.GetFreezePos(2).x,100);
        G.m_aClients[2].m_Predicted.m_FreezeEnd=0;G.m_aClients[2].m_Predicted.m_HookState=5;Near(G.GetFastInputPos(2).x,100);
        G.m_aClients[2].m_Predicted.m_HookState=0;G.m_aClients[2].m_Predicted.m_HookedPlayer=3;Near(G.GetFastInputPos(2).x,100);
        G.m_aClients[2].m_Predicted.m_HookedPlayer=-1;
        for(auto &P:G.m_aClients[2].m_aPredPos)P={1000,0};
        Near(G.GetFastInputPos(2).x,124);
        Near(G.ApplyAncientInputPost(2,{105,0}).x,129);
        std::puts("PASS: Ancient tick rate, others clamp, freeze/hook guard and displacement cap");
    }
    {
        CGameClient G;Ready(G,12);g_Config.m_TcLightInput=100;
        g_Config.m_TcRemoveAnti=1;Near(G.GetFreezePos(0).x,98.5f);
        g_Config.m_TcFastInput=0;Near(G.GetFreezePos(0).x,97.5f);
        std::puts("PASS: Remove Anti preserves freeze timeline and disabled mode");
    }
    {
        CGameClient G;Ready(G,12);g_Config.m_TcLightInput=100;
        auto &C=G.m_Controls;C.m_aMousePos[0]={100,200};C.m_aInputData[0].m_Hook=1;
        assert(C.CheckNewInput());Near(float(C.GetFastInput(0).m_TargetX),100);
        C.m_aMousePos[0]={300,400};assert(!C.CheckNewInput());Near(float(C.GetFastInput(0).m_TargetX),100);
        g_Config.m_ClSubTickAiming=1;C.CheckNewInput();Near(float(C.GetFastInput(0).m_TargetX),300);
        g_Config.m_TcAzureInput=100;g_Config.m_TcFastInputMode=13;g_Config.m_ClSubTickAiming=0;
        C.m_aInputData[0].m_TargetX=71;assert(C.CheckNewInput());assert(C.GetFastInput(0).m_TargetX==71);
        g_Config.m_TcLightAncientInput=20;g_Config.m_TcFastInputMode=14;assert(C.CheckNewInput());
        assert(!C.CheckNewInput());C.m_aInputData[1].m_Jump=1;assert(C.CheckNewInput());
        g_Config.m_ClDummy=1;G.m_Snap.m_LocalClientId=1;C.m_aMousePos[1]={9,8};g_Config.m_ClSubTickAiming=1;C.CheckNewInput();assert(C.GetFastInput(1).m_TargetX==9);
        g_Config.m_TcFastInputMode=12;assert(C.CheckNewInput());
        C.OnReset();assert(!C.m_aModeFastInputHookAction[12][0] && !C.m_aModeFastInputHookAction[12][1] && C.m_aaModeFastInput[12][0].m_Hook==0);
        C.m_FastInputHookAction[0]=true;C.m_FastInputHookAction[1]=true;
        C.m_aModeFastInputFireAction[12][0]=true;C.m_aModeFastInputFireAction[12][1]=true;
        C.ResetFastInput(1);
        assert(C.m_FastInputHookAction[0] && !C.m_FastInputHookAction[1]);
        assert(C.m_aModeFastInputFireAction[12][0] && !C.m_aModeFastInputFireAction[12][1]);
        std::puts("PASS: Light aim latch, Azure/Ancient input, Dummy and mode reset");
    }
    {
        CGameClient G;Ready(G,0);g_Config.m_TcFastInputAmount=20;assert(G.GetFastInputPredictionTicks()==1);
        g_Config.m_TcFastInputMode=6;g_Config.m_TcAncientInput=20;assert(G.GetFastInputPredictionTicks()==1);
        g_Config.m_TcLightAncientInput=100;assert(G.GetFastInputPredictionTicks()==1);
        g_Config.m_TcFastInputMode=4;assert(!G.FastInputActive(0,false));
        std::puts("PASS: original Tater/Ancient settings and reserved mode remain independent");
    }
    {
        CGameClient G;Ready(G,0);auto &C=G.m_Controls;
        for(int Dummy=0;Dummy<NUM_DUMMIES;++Dummy) {
            C.m_aFastInput[Dummy]=C.m_aInputData[Dummy];
            C.m_aaModeFastInput[11][Dummy]=C.m_aInputData[Dummy];
            C.m_aaModeFastInput[8][Dummy]=C.m_aInputData[Dummy];
        }
        g_Config.m_TcCloudInput=100;g_Config.m_TcFastInputMode=11;
        Check(C.CheckNewInput(),"switching Fast to Cloud forces immediate reprediction",11);
        Check(!C.CheckNewInput(),"stable Cloud cache does not repredict again",11);
        g_Config.m_TcBestInput=100;g_Config.m_TcFastInputMode=8;
        Check(C.CheckNewInput(),"switching Cloud to Best forces immediate reprediction",8);
        Check(!C.CheckNewInput(),"stable Best cache does not repredict again",8);
        std::puts("CHECKED: every mode transition forces one immediate reprediction");
    }
    constexpr int ExpectedChecks=337;
    const int ExecutedChecks=g_Checks;
    Check(ExecutedChecks==ExpectedChecks,"executed exactly 337 authority checks");
    std::printf("executed_checks=%d expected_checks=%d count_assertion=%s failures=%d\n",ExecutedChecks,ExpectedChecks,ExecutedChecks==ExpectedChecks?"PASS":"FAIL",g_Failures);
    return g_Failures ? 1 : 0;
}
'''
    source = header + "\n".join(declarations) + "\n};\n" + config + "\n".join(methods + control_methods) + tests
    msvc = max(Path("C:/VS2026/BuildTools/VC/Tools/MSVC").iterdir(), key=lambda p: tuple(map(int, p.name.split('.'))))
    kits = Path("C:/Program Files (x86)/Windows Kits/10")
    sdk = max((kits / "Include").glob("10.*"), key=lambda p: tuple(map(int, p.name.split('.')))).name
    env = dict(os.environ, VSLANG="1033")
    env["PATH"] = str(msvc / "bin/Hostx64/x64") + os.pathsep + env.get("PATH", "")
    env["INCLUDE"] = os.pathsep.join(map(str, [msvc / "include", *[kits / "Include" / sdk / part for part in ("ucrt", "shared", "um")]]))
    env["LIB"] = os.pathsep.join(map(str, [msvc / "lib/x64", *[kits / "Lib" / sdk / part / "x64" for part in ("ucrt", "um")]]))
    with tempfile.TemporaryDirectory(prefix="light-input-regression-") as directory:
        work = Path(directory)
        (work / "test.cpp").write_text(source, encoding="utf-8")
        for cmd in [[str(msvc / "bin/Hostx64/x64/cl.exe"), "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/I" + str(root / "src"), "test.cpp", "/Fe:test.exe", "/link", "/STACK:8388608"], [str(work / "test.exe")]]:
            result = subprocess.run(cmd, cwd=work, env=env, capture_output=True, timeout=120)
            print((result.stdout + result.stderr).decode("utf-8", "replace"))
            if result.returncode:
                return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
