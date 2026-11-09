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
    signatures = [
        "int CGameClient::FastInputMode()", "bool CGameClient::FastInputActive(",
        "int CGameClient::GetFastInputPredictionTicks()", "int CGameClient::GetFastInputOtherTicks()",
        "bool CGameClient::IsFastInputLocal(", "bool CGameClient::HasSnowHookInteraction(",
        "bool CGameClient::IsSnowHookActive()", "bool CGameClient::IsSnowHookAffected(",
        "int CGameClient::GetSnowHookTicks()", "void CGameClient::ApplyFastInputOffset(",
        "bool CGameClient::ReadFastInputPos(", "float CGameClient::GetAncientInputAmount(",
        "vec2 CGameClient::GetFastInputPos(", "vec2 CGameClient::GetFreezePos(",
        "vec2 CGameClient::ApplyAncientInputPost(",
    ]
    methods = [extract(game, signature) for signature in signatures]
    control_methods = [extract(controls, s) for s in (
        "bool CControls::CheckNewInput()", "void CControls::OnReset()", "void CControls::ResetInput(")]
    declarations = []
    for method in methods:
        decl = method[:method.index("{")].strip().replace("CGameClient::", "")
        decl = decl.replace("bool Salatik)", "bool Salatik = false)")
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
    int HookedPlayer() const {return m_HookedPlayer;}
};
struct CCharacter {int m_FreezeAccumulation=0,m_AliveAccumulation=0,m_FreezeTime=0;};
struct CWorld {
    CCharacter m_Character;
    CCharacter *GetCharacterById(int){return &m_Character;}
};
struct CClock {
    int m_Tick=100,m_Speed=50,m_Prev=90,m_FreezeTick=98;
    float m_Intra=0.25f,m_FreezeIntra=0.5f;
    int PredGameTick(int) const {return m_Tick;}
    int PrevGameTick(int) const {return m_Prev;}
    float PredIntraGameTick(int) const {return m_Intra;}
    int GameTickSpeed() const {return m_Speed;}
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
    bool m_FastInputHookAction=false,m_FastInputFireAction=false,m_aModeFastInputHookAction[15]{},m_aModeFastInputFireAction[15]{};
    int m_ActiveFastInputMode=0;
    CNetObj_PlayerInput &GetFastInput(int Slot){return m_ActiveFastInputMode==0 ? m_aFastInput[Slot] : m_aaModeFastInput[m_ActiveFastInputMode][Slot];}
    bool CheckNewInput(); void OnReset(); void ResetInput(int Dummy);
};
struct CGameClient {
    CClock m_Clock;
    CClock *Client() const {return const_cast<CClock *>(&m_Clock);}
    bool m_Dummy=true;
    bool PredictDummy() const {return m_Dummy;}
    int m_aLocalIds[2]={0,1};
    struct CSnap { int m_LocalClientId=0; struct CChar {bool m_Active=false; CCore m_Cur,m_Prev;} m_aCharacters[MAX_CLIENTS];} m_Snap;
    struct CData {
        CCore m_Predicted,m_PrevPredicted,m_RegularPredicted,m_PrevRegularPredicted;
        vec2 m_aPredPos[200]; int m_aPredTick[200]{};
        long long m_aSmoothLen[2]{},m_aSmoothStart[2]{};
    } m_aClients[MAX_CLIENTS];
    CControls m_Controls;
    FastInput::CAncientState m_aAncientInputState[2],m_aLightAncientInputState[2];
    CWorld m_PredictedWorld,m_ExtraPredictedWorld;
    int m_SmoothTick=0; float m_SmoothIntraTick=0;
    vec2 GetSmoothPos(int){return {};}
    vec2 GetFInputPos(int){return {};}
    CGameClient(){m_Controls.m_Game=this;}
'''
    tests = r'''
void Near(float Actual,float Expected) { if(std::fabs(Actual-Expected)>0.0002f) {std::fprintf(stderr,"actual %f expected %f\n",Actual,Expected);assert(false);} }
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
    {
        CGameClient G;Ready(G,12);g_Config.m_TcLightInput=150;
        assert(G.GetFastInputPredictionTicks()==2 && G.GetFastInputOtherTicks()==0);
        int T=100;float F=.75f;G.ApplyFastInputOffset(0,T,F);assert(T==102);Near(F,.25f);
        T=100;F=.25f;G.ApplyFastInputOffset(2,T,F);assert(T==100);Near(F,.25f);
        g_Config.m_TcLightInputOthers=1;G.ApplyFastInputOffset(2,T,F);assert(T==102);Near(F,.25f);
        g_Config.m_TcLightInputOthers=0;G.m_Snap.m_aCharacters[0].m_Cur.m_HookState=5;G.m_Snap.m_aCharacters[0].m_Cur.m_HookedPlayer=2;
        g_Config.m_TcLightInputHook=1;g_Config.m_TcLightInputHookAmount=37;
        assert(G.IsSnowHookAffected(2) && !G.IsSnowHookAffected(3));
        T=100;F=.25f;G.ApplyFastInputOffset(2,T,F);assert(T==102);Near(F,.10f);
        vec2 P;assert(G.ReadFastInputPos(2,102,.1f,102,P,false));
        G.m_aClients[2].m_aPredTick[102]=302;assert(!G.ReadFastInputPos(2,102,.1f,102,P,false));
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
        G.m_aClients[2].m_Predicted.m_FreezeEnd=10;
        Near(G.GetFastInputPos(2).x,100);
        g_Config.m_TcRemoveAnti=1;Near(G.GetFreezePos(2).x,98.1f);
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
        C.OnReset();assert(!C.m_aModeFastInputHookAction[12] && C.m_aaModeFastInput[12][0].m_Hook==0);
        std::puts("PASS: Light aim latch, Azure/Ancient input, Dummy and mode reset");
    }
    {
        CGameClient G;Ready(G,0);g_Config.m_TcFastInputAmount=20;assert(G.GetFastInputPredictionTicks()==1);
        g_Config.m_TcFastInputMode=6;g_Config.m_TcAncientInput=20;assert(G.GetFastInputPredictionTicks()==1);
        g_Config.m_TcLightAncientInput=100;assert(G.GetFastInputPredictionTicks()==1);
        g_Config.m_TcFastInputMode=4;assert(!G.FastInputActive(0,false));
        std::puts("PASS: original Tater/Ancient settings and reserved mode remain independent");
    }
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
