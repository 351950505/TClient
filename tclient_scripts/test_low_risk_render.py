"""提取实际 C++ 方法做隔离回归；不替代字体、GPU 或游戏内视觉验收。"""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASELINE = '48811c0a60bc9ad7de98283d0f94496fe92559e8'

def baseline_source(name):
    return subprocess.run(['git', '-C', str(ROOT), 'show', BASELINE + ':src/game/client/components/' + name], capture_output=True, check=True).stdout.decode('utf-8-sig')


def extract(text, signature):
    start = text.index(signature)
    pos = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]


COMMON = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
struct vec2 { float x,y; vec2(float X=0,float Y=0):x(X),y(Y){} };
struct ColorRGBA {
 float r,g,b,a; ColorRGBA(float R=1,float G=1,float B=1,float A=1):r(R),g(G),b(B),a(A){}
 ColorRGBA WithMultipliedAlpha(float A) const {return {r,g,b,a*A};}
 bool operator==(const ColorRGBA &o)const{return r==o.r&&g==o.g&&b==o.b&&a==o.a;}
};
struct STextBoundingBox {float m_X,m_Y,m_W,m_H;};
struct STextContainerIndex {int id=-1; bool Valid()const{return id>=0;}};
constexpr int TEXTFLAG_RENDER=1;
struct CTextCursor {
 float m_FontSize=0,m_LineWidth=-1,m_MaxCharacterHeight=0;int m_Flags=TEXTFLAG_RENDER;
 STextBoundingBox box{}; STextBoundingBox BoundingBox()const{return box;}
};
struct Draw { std::string text;float x,y,size;ColorRGBA color;
 bool operator==(const Draw &o)const{return text==o.text&&x==o.x&&y==o.y&&size==o.size&&color==o.color;}
};
struct ITextRender {
 struct Container {std::string text;float size;};
 std::map<int,Container> containers;std::vector<Draw> draws;ColorRGBA color;
 int next=0,layouts=0,creates=0,soft=0;
 ColorRGBA GetTextColor()const{return color;}
 ColorRGBA DefaultTextColor()const{return {};}
 ColorRGBA DefaultTextOutlineColor()const{return {0,0,0,0.3f};}
 void TextColor(ColorRGBA c){color=c;}
 void TextColor(float r,float g,float b,float a){color={r,g,b,a};}
 float TextWidth(float size,const char *s,int=-1,float=-1){++layouts;return std::string(s).size()*size/2;}
 void Text(float x,float y,float size,const char *s,float=-1){++layouts;draws.push_back({s,x,y,size,color});}
 void Store(STextContainerIndex &i,CTextCursor *c,const char *s){
  ++layouts;containers[i.id]={s,c->m_FontSize};c->box={0,0,float(std::string(s).size())*c->m_FontSize/2,c->m_FontSize};c->m_MaxCharacterHeight=c->m_FontSize;
 }
 void RecreateTextContainer(STextContainerIndex &i,CTextCursor *c,const char *s){++creates;i.id=next++;Store(i,c,s);}
 void RecreateTextContainerSoft(STextContainerIndex &i,CTextCursor *c,const char *s){assert(i.Valid());++soft;Store(i,c,s);}
 void DeleteTextContainer(STextContainerIndex &i){containers.erase(i.id);i.id=-1;}
 void RenderTextContainer(STextContainerIndex i,ColorRGBA c,ColorRGBA,float x,float y){
  assert(containers.count(i.id));auto &v=containers.at(i.id);draws.push_back({v.text,x,y,v.size,c});
 }
};
'''

HUD_STUB = r'''
struct Config {int m_ClDummy=0;};Config g_Config;
constexpr int GAMESTATEFLAG_SUDDENDEATH=1,GAMESTATEFLAG_GAMEOVER=2,GAMESTATEFLAG_RACETIME=4,GAMESTATEFLAG_PAUSED=8;
struct Info {int m_GameStateFlags=0,m_TimeLimit=0,m_WarmupTimer=0,m_RoundStartTick=0;};
struct Game {struct {Info *m_pGameInfoObj;} m_Snap;};
int g_Ticks[2]={500,900};int64_t g_Time=0;
int64_t time_get_placeholder(){return g_Time;}
int64_t time_freq(){return 1000;}
struct ClientStub {int GameTick(int d){return g_Ticks[d];}int GameTickSpeed(){return 50;}};
struct GraphicsStub {float ScreenAspect(){return 16.0f/9.0f;}};
std::string g_Language;
const char *Localize(const char *s){static std::string value;value=g_Language+s;return value.c_str();}
enum class ETimeFormat {DAYS};
void str_time(int64_t value,ETimeFormat,char *out,size_t n){std::snprintf(out,n,"%lld:%02lld",value/6000,(value/100)%60);}
struct CHud {
 float m_Width=300.0f*16.0f/9.0f;ITextRender renderer;Game game;ClientStub client;GraphicsStub graphics;
 CCachedText m_GameTimerText,m_PauseNotificationText,m_SuddenDeathText;
 ITextRender *TextRender(){return &renderer;}Game *GameClient(){return &game;}
 ClientStub *Client(){return &client;}GraphicsStub *Graphics(){return &graphics;}
 int64_t time(){return g_Time;}
 void RenderGameTimer();void RenderPauseNotification();void RenderSuddenDeath();
 void ResetCached(){m_GameTimerText.Reset(TextRender());m_PauseNotificationText.Reset(TextRender());m_SuddenDeathText.Reset(TextRender());}
};
'''

HUD_TEST = r'''
int main(){
 Info info;CHud hud;hud.game.m_Snap.m_pGameInfoObj=&info;
 for(int dummy=0;dummy<2;++dummy)for(int flags=0;flags<16;++flags)for(int limit: {0,1,30})for(int warmup: {-100,0,100})for(int sec: {0,1,9,10,59,60,61,3600,86400}){
  g_Config.m_ClDummy=dummy;info={flags,limit,warmup,0};g_Ticks[dummy]=sec*50;g_Time=(sec%2)*500;
  hud.renderer.draws.clear();hud.renderer.color={0.7f,0.8f,0.9f,0.6f};
  hud.RenderGameTimer();hud.RenderPauseNotification();hud.RenderSuddenDeath();
  for(const auto &d:hud.renderer.draws)std::printf("%s|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f|%.4f\n",d.text.c_str(),d.x,d.y,d.size,d.color.r,d.color.g,d.color.b,d.color.a);
 }
 g_Language="translated ";info={GAMESTATEFLAG_PAUSED,0,0,0};hud.ResetCached();
 hud.RenderPauseNotification();assert(hud.renderer.draws.back().text=="translated Game paused");
 hud.ResetCached();info={0,1,0,0};g_Ticks[g_Config.m_ClDummy]=50*51;
 hud.renderer.layouts=0;hud.renderer.creates=0;hud.renderer.soft=0;
 for(int i=0;i<200;++i){g_Time=i%2?500:0;hud.RenderGameTimer();}
 if(CANDIDATE){assert(hud.renderer.creates==1);assert(hud.renderer.soft==0);assert(hud.renderer.layouts==1);}
 else assert(hud.renderer.layouts==200);
 std::fprintf(stderr,"timer layouts/200 frames: %d\n",hud.renderer.layouts);
 auto last=hud.renderer.draws.back();hud.ResetCached();hud.RenderGameTimer();assert(hud.renderer.draws.back()==last);
 g_Ticks[g_Config.m_ClDummy]+=50;hud.RenderGameTimer();
 if(CANDIDATE)assert(hud.renderer.soft==1);
 return 0;
}
'''


def compile_run(source, label, env, compiler):
    with tempfile.TemporaryDirectory(prefix='tclient-low-risk-') as td:
        work = Path(td)
        cpp = work / 'check.cpp'
        exe = work / 'check.exe'
        cpp.write_text(source, encoding='utf-8')
        command = [str(compiler), '/nologo', '/std:c++17', '/EHsc', '/utf-8', '/Od', str(cpp), '/Fe:' + str(exe)]
        result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, errors='replace')
        if result.returncode:
            raise RuntimeError(label + ': ' + result.stdout + result.stderr)
        result = subprocess.run([str(exe)], cwd=work, capture_output=True)
        if result.returncode:
            raise RuntimeError(label + ': ' + result.stderr.decode(errors='replace'))
        print(label + ': ' + result.stderr.decode(errors='replace').strip())
        return result.stdout


def nameplate_test(text, candidate):
    begin = text.index('class CNamePlate\n')
    end = text.index('class CNamePlates::CNamePlatesData', begin)
    plate = text[begin:end]
    methods = extract(plate, 'void Reset(CGameClient &This)') + '\n' + extract(plate, 'void Update(CGameClient &This, const CNamePlateData &Data)')
    needs = extract(plate, 'bool NeedsInitialization() const') if candidate else 'bool NeedsInitialization() const {return false;}'
    tail = text[text.index('CNamePlate &NamePlate = m_pData->m_aNamePlates[pPlayerInfo->m_ClientId];'):text.index('\nvoid CNamePlates::RenderNamePlatePreview')]
    tail = tail.rsplit('}', 1)[0]
    prefix = 'm_InitializedThisFrame = 0;' if candidate else ''
    if candidate:
        assert prefix in extract(text, 'void CNamePlates::OnRender()')
    code = r'''
#include <cassert>
#include <cstdio>
#include <vector>
struct vec2 {float x=0,y=0;vec2(float X=0,float Y=0):x(X),y(Y){}vec2 operator-(vec2 b)const{return {x-b.x,y-b.y};}};
struct CGameClient {};
struct CNamePlateData {bool m_InGame=true;int value=0;};
struct Part {int value=0,updates=0,resets=0;void Reset(CGameClient &){++resets;}void Update(CGameClient &,const CNamePlateData &d){value=d.value;++updates;}};
struct CNamePlate {bool m_InGame=false,m_Inited=false;Part part;std::vector<Part *> m_vpParts{&part};int rendered=0;
 void Init(CGameClient &){m_Inited=true;}void Render(CGameClient &,vec2){++rendered;}
 METHODS
 NEEDS
};
struct {int m_ClNamePlatesOffset=0;}g_Config;
struct Player {int m_ClientId;};
struct CNamePlates {struct {CNamePlate m_aNamePlates[20];}data;decltype(data) *m_pData=&data;int m_InitializedThisFrame=0;CGameClient game;
 CGameClient *GameClient(){return &game;}
 void BeginFrame(){PREFIX}
 void Render(int id,int value){Player player{id};Player *pPlayerInfo=&player;CNamePlateData Data;Data.value=value;vec2 Position;
 TAIL
 }
};
int main(){CNamePlates plates;
 plates.BeginFrame();for(int id=0;id<20;++id)plates.Render(id,1);
 int count=0;for(auto &p:plates.data.m_aNamePlates)count+=p.rendered>0;assert(count==(CANDIDATE?4:20));
 if(CANDIDATE){
  plates.Render(0,2);assert(plates.data.m_aNamePlates[0].part.value==2);
  assert(plates.m_InitializedThisFrame==4);
  for(int frame=1;frame<5;++frame){plates.BeginFrame();for(int id=0;id<20;++id)plates.Render(id,frame+2);assert(plates.m_InitializedThisFrame==4);}
  for(auto &p:plates.data.m_aNamePlates)assert(!p.NeedsInitialization());
  plates.BeginFrame();plates.Render(0,91);plates.Render(0,92);assert(plates.m_InitializedThisFrame==0);assert(plates.data.m_aNamePlates[0].part.value==92);
  for(auto &p:plates.data.m_aNamePlates)p.Reset(plates.game);
  plates.BeginFrame();for(int id=0;id<20;++id)plates.Render(id,100);
  assert(plates.m_InitializedThisFrame==0);
  for(auto &p:plates.data.m_aNamePlates){assert(!p.NeedsInitialization());assert(p.part.value==100);}
 }
 std::fprintf(stderr,"nameplate initialization and immediate-update checks: PASS\n");}
'''
    return code.replace('METHODS', methods).replace('NEEDS', needs).replace('PREFIX', prefix).replace('TAIL', tail).replace('CANDIDATE', 'true' if candidate else 'false')


def main():
    global ROOT, BASELINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=ROOT)
    parser.add_argument('--baseline-ref', default=BASELINE)
    args = parser.parse_args()
    ROOT, BASELINE = args.repo.resolve(), args.baseline_ref
    tools = Path('C:/VS2026/BuildTools/VC/Tools/MSVC')
    msvc = max(tools.iterdir(), key=lambda p: tuple(map(int, p.name.split('.'))))
    kits = Path('C:/Program Files (x86)/Windows Kits/10')
    sdk = max((kits / 'Include').glob('10.*'), key=lambda p: tuple(map(int, p.name.split('.')))).name
    env = os.environ.copy()
    env['INCLUDE'] = os.pathsep.join(map(str, [msvc / 'include', kits / 'Include' / sdk / 'ucrt', kits / 'Include' / sdk / 'shared', kits / 'Include' / sdk / 'um']))
    env['LIB'] = os.pathsep.join(map(str, [msvc / 'lib/x64', kits / 'Lib' / sdk / 'ucrt/x64', kits / 'Lib' / sdk / 'um/x64']))
    compiler = msvc / 'bin/Hostx64/x64/cl.exe'
    env['PATH'] = str(compiler.parent) + os.pathsep + env['PATH']
    ui_h = (ROOT / 'src/game/client/ui.h').read_text(encoding='utf-8')
    cache_class = extract(ui_h, 'class CCachedText\n') + ';'
    ui_cpp = (ROOT / 'src/game/client/ui.cpp').read_text(encoding='utf-8')
    cache_methods = '\n'.join(extract(ui_cpp, signature) for signature in ['void CCachedText::Update(', 'void CCachedText::Render(', 'void CCachedText::Reset('])
    # 未测试彩色缓存路径，仅使真实类的声明可以编译。
    common = COMMON + '\nstruct STextColorSplit {};\n' + cache_class + cache_methods + HUD_STUB
    outputs = []
    for baseline in [True, False]:
        label = 'before' if baseline else 'current'
        hud = baseline_source('hud.cpp') if baseline else (ROOT / 'src/game/client/components/hud.cpp').read_text(encoding='utf-8')
        candidate = 'm_GameTimerText.Update' in hud
        methods = '\n'.join(extract(hud, 'void CHud::' + name + '()') for name in ['RenderGameTimer', 'RenderPauseNotification', 'RenderSuddenDeath'])
        outputs.append(compile_run(common + methods + HUD_TEST.replace('CANDIDATE', 'true' if candidate else 'false'), 'HUD ' + label, env, compiler))
        names = baseline_source('nameplates.cpp') if baseline else (ROOT / 'src/game/client/components/nameplates.cpp').read_text(encoding='utf-8')
        compile_run(nameplate_test(names, 'NeedsInitialization() const' in names), 'nameplates ' + label, env, compiler)
    assert outputs[0] == outputs[1], 'HUD draw records differ'
    print('PASS: HUD text, placement, color and blink draw records match baseline across 2592 state combinations.')
    print('LIMIT: mock renderer and extracted methods; no GPU, real glyph or online/Dummy integration coverage.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
