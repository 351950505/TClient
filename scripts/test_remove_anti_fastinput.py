"""从当前生产源码提取快速输入方法，在隔离夹具中执行回归。

只模拟网络时钟、快照与图形数据，不启动客户端。不能替代真实双客户端验收。
Windows 运行：python -X utf8 -I scripts/test_remove_anti_fastinput.py
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
    parser.add_argument("--revision")
    args = parser.parse_args()
    root = args.repo
    game = (root / "src/game/client/gameclient.cpp").read_text(encoding="utf-8")
    controls = (root / "src/game/client/components/controls.cpp").read_text(encoding="utf-8")
    if args.revision:
        game = subprocess.check_output(["git", "-C", str(root), "show", args.revision + ":src/game/client/gameclient.cpp"]).decode("utf-8")
        controls = subprocess.check_output(["git", "-C", str(root), "show", args.revision + ":src/game/client/components/controls.cpp"]).decode("utf-8")
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
    signatures[1:1] = [signature for signature in (
        "bool CGameClient::IsSnapshotInputMode()",
        "bool CGameClient::FastInputImmediateOthers()",
        "int CGameClient::SnapshotInputMaxTick(",
        "bool CGameClient::SnapshotTryGetPredPos(",
    ) if signature in game]
    methods = [extract(game, signature) for signature in signatures]
    control_methods = [extract(controls, s) for s in (
        "bool CControls::CheckNewInput()", "void CControls::OnReset()", "void CControls::ResetInput(", "void CControls::ResetFastInput(")]
    declarations = []
    for method in methods:
        decl = method[:method.index("{")].strip().replace("CGameClient::", "")
        decl = decl.replace("bool Salatik) const", "bool Salatik = false) const")
        decl = decl.replace("int InterpolationMode) const", "int InterpolationMode = 0) const")
        declarations.append(decl + ";")
    clock_method = extract((root / "src/engine/client/client.cpp").read_text(encoding="utf-8"), "void CClient::GetSmoothFreezeTick(").replace("CClient::", "CClock::")
    fields = sorted(set(re.findall(r"g_Config\.(m_\w+)", "\n".join(methods + control_methods + [clock_method]))) | {
        "m_TcLightInput", "m_TcAzureInput", "m_TcLightAncientInput",
        "m_TcLightInputOthers", "m_TcAzureInputOthers", "m_TcLightAncientInputOthers",
        "m_TcLightAncientInputOthersAmount", "m_TcLightInputHook", "m_TcLightInputHookAmount",
    })
    config = "struct CConfig {\n" + "\n".join(
        ("float" if field == "m_TcMeowInput" else "int") + " " + field + " = 0;" for field in fields
    ) + "\n} g_Config;\n"
    header = r'''
#include <algorithm>
#include <cstdint>
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
    struct CTime { long long m_Value=0; long long Get(long long) const {return m_Value;} };
    CTime m_aGameTime[2]{{1700},{1700}},m_PredictedTime{1985};
    void GetSmoothFreezeTick(int *Tick,float *Intra,float MixAmount);
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
    bool m_aReceivedPreInput[NUM_DUMMIES]{};
    CWorld m_PredictedWorld,m_ExtraPredictedWorld;
    int m_SmoothTick=0; float m_SmoothIntraTick=0;
    vec2 GetSmoothPos(int){return {};}
    vec2 GetFInputPos(int){return {};}
    CGameClient(){m_Controls.m_Game=this;}
'''
    tests = r'''
int main(){
    int Cases=0,Checks=0,Failures=0;
    const char *Names[] = {"Tater","Meow","Snow","Next","unused","Salatik","Ancient","F","Best","Saiko","Delta","Cloud","Light","Azure","BCLAncient"};
    for(int Stage=0;Stage<3;++Stage) for(int Window=0;Window<2;++Window) for(int Mode : {0,1,2,3,5,6,7,8,9,10,11,12,13,14}) {
        CGameClient G;
        g_Config={};g_Config.m_TcFastInput=1;g_Config.m_TcFastInputMode=Mode;
        g_Config.m_TcFastInputAmount=20;g_Config.m_TcNextInput=100;g_Config.m_TcAncientInput=20;
        g_Config.m_TcSaikoInput=100;g_Config.m_TcDeltaInput=100;g_Config.m_TcCloudInput=100;
        g_Config.m_TcMeowInput=1.0f;g_Config.m_TcSnowInput=100;g_Config.m_TcSalatikInputOffset=100;g_Config.m_TcFInput=1000;g_Config.m_TcBestInput=100;
        g_Config.m_TcFastInputOthers=g_Config.m_TcMeowInputOthers=g_Config.m_TcSnowInputOthers=g_Config.m_TcNextInputOthers=1;
        g_Config.m_TcSalatikInputOthers=g_Config.m_TcFInputOthers=g_Config.m_TcBestInputOthers=g_Config.m_TcSaikoInputOthers=g_Config.m_TcDeltaInputOthers=g_Config.m_TcCloudInputOthers=1;
        g_Config.m_TcAncientInputOthers=1;g_Config.m_TcAncientInputOthersAmount=20;
        g_Config.m_TcLightInputOthers=g_Config.m_TcAzureInputOthers=g_Config.m_TcLightAncientInputOthers=1;g_Config.m_TcLightAncientInputOthersAmount=20;
        g_Config.m_TcSalatikInputInterpolation=1;g_Config.m_TcBestInputInterpolation=1;
        G.m_aReceivedPreInput[0]=G.m_aReceivedPreInput[1]=true;
        g_Config.m_TcUnfreezeLagTicks=5;g_Config.m_TcUnfreezeLagDelayTicks=25;
        NEW_CONFIG
        auto &D=G.m_aClients[0];D.m_RegularPredicted.m_Pos={100,0};D.m_Predicted.m_Pos={100,0};D.m_PrevPredicted.m_Pos={99,0};
        if(Window){D.m_aSmoothLen[0]=D.m_aSmoothLen[1]=1000;D.m_aSmoothStart[0]=D.m_aSmoothStart[1]=50;}
        G.m_Snap.m_aCharacters[0].m_Active=true;G.m_Snap.m_aCharacters[2].m_Active=true;
        auto &R=G.m_aClients[2];R.m_RegularPredicted.m_Pos={100,0};R.m_Predicted.m_Pos={100,0};R.m_PrevPredicted.m_Pos={99,0};
        for(int Tick=80;Tick<120;++Tick){D.m_aPredTick[Tick%200]=Tick;D.m_aPredPos[Tick%200]={float(Tick),0};R.m_aPredTick[Tick%200]=Tick;R.m_aPredPos[Tick%200]={float(Tick),0};}
        ++Cases;++Checks;
        const bool Active=G.FastInputActive(0,false);
        if(!Active){++Failures;std::printf("%s window=%d mode=%-10s configured_active=0 FAIL\n",Stage==0?"unfrozen":Stage==1?"frozen":"thawed",Window,Names[Mode]);}
        G.m_PredictedWorld.m_Character.m_FreezeAccumulation=Stage?25:0;
        G.m_PredictedWorld.m_Character.m_FreezeTime=Stage==1?150:0;
        G.m_ExtraPredictedWorld.m_Character.m_AliveAccumulation=Stage==2?5:0;
        g_Config.m_TcRemoveAnti=1;
        const float With=G.GetFreezePos(0).x;
        int T;float F;G.Client()->GetSmoothFreezeTick(&T,&F,Stage==1?0.0f:1.0f);
        const float Base=float(T-1)+F,Expected=Base+1;
        bool Good=std::fabs(With-Expected)<.0002f;
        ++Checks;
        std::printf("%s window=%d mode=%-10s baseline=%.2f extra=%.2f expected_extra=1.00 %s\n",Stage==0?"unfrozen":Stage==1?"frozen":"thawed",Window,Names[Mode],Base,With-Base,Good?"PASS":"FAIL");
        if(!Good)++Failures;
        const float Other=G.GetFreezePos(2).x;
        // 上游 Best/Saiko/Delta 立即使用 Others；Light/Azure/Ancient 直接使用远端快照时钟。
        // Cloud 保留冻结时间线，再在该时间线上施加快照偏移。
        const bool DirectOtherClock=(Mode>=8 && Mode<=10) || (Mode>=12 && Mode<=14);
        const float OtherExpected=DirectOtherClock ? float(G.m_Clock.m_Tick-1)+G.m_Clock.m_Intra+1.0f : Expected;
        const bool OtherGood=std::fabs(Other-OtherExpected)<.0002f;
        ++Checks;
        std::printf("%s window=%d mode=%-10s other=%.2f expected=%.2f %s\n",Stage==0?"unfrozen":Stage==1?"frozen":"thawed",Window,Names[Mode],Other,OtherExpected,OtherGood?"PASS":"FAIL");
        if(!OtherGood)++Failures;
    }
    constexpr int ExpectedCases=84,ExpectedChecks=252;
    const int ExecutedChecks=Checks;
    const bool CountGood=Cases==ExpectedCases && ExecutedChecks==ExpectedChecks;
    if(!CountGood)++Failures;
    std::printf("cases=%d expected_cases=%d executed_checks=%d expected_checks=%d count_assertion=%s failures=%d\n",Cases,ExpectedCases,ExecutedChecks,ExpectedChecks,CountGood?"PASS":"FAIL",Failures);
    return Failures?1:0;
}
'''
    tests = tests.replace('NEW_CONFIG', '' if args.revision else 'g_Config.m_TcLightInput=100;g_Config.m_TcAzureInput=100;g_Config.m_TcLightAncientInput=20;')
    source = header + "\n".join(declarations) + "\n};\n" + config + clock_method + "\n".join(methods + control_methods) + tests
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
