// Offline rendering-seam regression; no media/network/GPU or shared build outputs.
'use strict';
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),cp=require('node:child_process'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..'),source=fs.readFileSync(root+'/src/game/client/components/tclient/island_music.cpp','utf8').replace(/\r\n/g,'\n');
function between(s,a,b){const x=s.indexOf(a),y=s.indexOf(b,x+a.length);assert(x>=0&&y>x,'Missing seam '+a);return s.slice(x,y);}
const out=fs.mkdtempSync(path.join(os.tmpdir(),'island-two-lines-'));
try{
 const config=fs.readFileSync(root+'/src/engine/shared/config_variables_tclient.h','utf8');
 const menus=fs.readFileSync(root+'/src/game/client/components/tclient/menus_tclient.cpp','utf8');
 assert(config.includes('MACRO_CONFIG_INT(TcIslandDoubleLyrics, tc_island_double_lyrics, 0, 0, 1,'),'double lyrics must default off');
 assert(menus.includes('DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcIslandDoubleLyrics'),'two-line lyrics checkbox is wired');
 assert(source.includes('const bool DoubleLyrics = g_Config.m_TcIslandDoubleLyrics != 0;'),'rendering must honor the saved switch');
 // Limit extraction to the two cue-selection methods, excluding unrelated player controls.
 const current=between(source,'\tint FindLineIndex(','\tint FindNextLineIndex(');
 const next=between(source,'\tint FindNextLineIndex(', '\n\t// ---------------- track collection');
 const fit=between(source,'\tstatic void FitText(', '\n\t// 缓存 TextWidth');
 const sizes=between(source,'const float LyricFontScale =','const float TimerSize =');
 const positions=between(source,'const float LyricGap =','if(m_NextLyricKey !=');
 const selection=between(source,'const int NextLyricIndex = DoubleLyrics ?', 'const char *pLyricFace =');
 const nextDraw=between(source,'if(m_NextLyricKey !=','const ColorRGBA ColorUnsung');
 assert(!between(source,'if(UseLyric)\n\t\t{','const ColorRGBA ColorUnsung').includes('m_TitleFit'), 'song title must not replace current lyric');
 assert(source.includes('FindNextLineIndex(LyricIndex)'),'next cue selection wired');
 assert(source.includes('if(TransparentMode)')&&source.includes('TextOutlineColor'),'transparent outline preserved');
 const program=`
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <game/client/components/tclient/lyric_parser.h>
using TcLyric::SLyricLine;
int str_length(const char*p){return (int)std::strlen(p);}int str_utf8_rewind(const char*p,int i){while(i){--i;if(((unsigned char)p[i]&0xc0)!=0x80)break;}return i;}
template<size_t N>void str_copy(char(&dst)[N],const char*src){std::strncpy(dst,src,N-1);dst[N-1]=0;}
struct ITextRender{struct Draw{float x,y,size,alpha;std::string text;};std::vector<Draw> draws;float alpha=1;float font=1;int calls=0;float TextWidth(float size,const char*p){++calls;int chars=0;for(;*p;p++)if(((unsigned char)*p&0xc0)!=0x80)chars++;return chars*size*font*.5f;}void TextColor(float,float,float,float a){alpha=a;}void Text(float x,float y,float size,const char*p){draws.push_back({x,y,size,alpha,p});}};
struct Config{int m_TcFontSize=100;int m_TcIslandDoubleLyrics=0;}g_Config;
struct Layout{float currentY,nextY,currentSize,nextSize,currentHeight,nextHeight;};
class Test{public:std::vector<SLyricLine>m_vLines;std::string m_NextLyricKey,m_NextLyricFace,m_NextLyricText;float m_NextLyricSize=-1,m_NextLyricMaxW=-1;ITextRender*m_pTextRender;
${current}
${next}
${fit}
Layout RenderNext(int LyricIndex,float Scale,float PillY,float Height,float TextX,float MaxTextW,float Dim,const char*pLyricFace){
 const char *aSong="Test song";
 ${sizes}
 ${selection}
 ${positions}
 ${nextDraw}
 return {LyricY,NextLyricY,LyricSize,NextLyricSize,LyricGlyphSize,NextLyricGlyphSize};
}};
void Check(bool b,const char*s){if(!b)throw std::runtime_error(s);}
int main(){try{ITextRender text;Test t;t.m_pTextRender=&text;
 t.m_vLines={{1000,"current"},{1000,"translation"},{2000,""},{3000,"next"},{4000,"last"}};
 Check(t.FindLineIndex(0)==0&&t.FindLineIndex(1500)==1,"pre-roll/current lookup");
 Check(t.FindNextLineIndex(-1)==-1&&t.FindNextLineIndex(0)==3&&t.FindNextLineIndex(1)==3&&t.FindNextLineIndex(4)==-1&&t.FindNextLineIndex(5)==-1,"next selection skips same timestamp/blank and stays bounded");
 t.m_vLines={{1000,"current"},{3000,"original"},{3000,"translation"},{4000,""},{4000,""},{5000,"last"}};
 Check(t.FindNextLineIndex(0)==t.FindLineIndex(3000),"next timestamp preview differs from the actual current cue");
 Check(t.FindNextLineIndex(2)==5,"blank timestamp group must not be previewed");
 Check(TcLyric::IsSimplifiedChinese("风沿着山谷经过")&&!TcLyric::IsSimplifiedChinese("風沿著山谷經過"),"simplified/traditional classification");
 Check(!TcLyric::IsSimplifiedChinese("Hello world")&&!TcLyric::IsSimplifiedChinese("こんにちは世界")&&!TcLyric::IsSimplifiedChinese("안녕하세요")&&!TcLyric::IsSimplifiedChinese("123 🙂"),"non-Chinese classification");
 std::vector<SLyricLine> lines;
 Check(TcLyric::ParseSyncedLyrics("[00:01.00]Hello world\\n[00:03.00]Next line",lines),"parse original lyrics");
 const auto words=lines[0].m_vWords;
 TcLyric::AttachTranslatedLyrics(lines,"[00:01.00]你好世界\\n[00:03.12]下一句");
 Check(lines[0].m_Translation=="你好世界"&&lines[1].m_Translation=="下一句"&&lines[0].m_vWords.size()==words.size()&&lines[0].m_vWords[0].m_StartMs==words[0].m_StartMs,"translation alignment changes original word timing");
 std::vector<SLyricLine> ambiguous={{1000,"A"},{1200,"B"}};TcLyric::AttachTranslatedLyrics(ambiguous,"[00:01.10]不确定");
 Check(ambiguous[0].m_Translation.empty()&&ambiguous[1].m_Translation.empty(),"ambiguous translation must not attach");
 std::vector<SLyricLine> far={{1000,"A"}};TcLyric::AttachTranslatedLyrics(far,"[00:03.00]太远");Check(far[0].m_Translation.empty(),"far cue must not attach by line number");
 std::vector<SLyricLine> nearest={{1000,"A"}};TcLyric::AttachTranslatedLyrics(nearest,"[00:00.80]较远\\n[00:01.00]准确");Check(nearest[0].m_Translation=="准确","closest translation must win instead of the first nearby entry");
 std::vector<SLyricLine> mixed={{1000,"Baby"},{3000,"Next"}};TcLyric::AttachTranslatedLyrics(mixed,"[00:01.00]Baby，宝贝");
 Check(!TcLyric::IsSimplifiedChinese("Baby，宝贝")&&mixed[0].m_Translation=="Baby，宝贝","mixed provider translation must not relax original-language heuristics");
 t.m_vLines=mixed;g_Config.m_TcIslandDoubleLyrics=1;text.draws.clear();t.RenderNext(0,1,0,72,100,500,1,"face-mixed");
 Check(text.draws.size()==1&&text.draws[0].text=="Baby，宝贝","valid mixed translation fell back to the next cue");
 for(const char *bad:{"kimi no na wa","Baby","君の名前","안녕世界","123 🙂"}){std::vector<SLyricLine> rejected={{1000,"original"}};TcLyric::AttachTranslatedLyrics(rejected,(std::string("[00:01.00]")+bad).c_str());Check(rejected[0].m_Translation.empty(),"romaji/kana/Korean/non-language text masqueraded as Chinese translation");}
 std::vector<SLyricLine> boundary={{1000,"A"},{1000,"B"},{2000,"C"}};TcLyric::AttachTranslatedLyrics(boundary,"[00:00.75]边界\\n[00:02.251]超界");Check(boundary[0].m_Translation=="边界"&&boundary[1].m_Translation=="边界"&&boundary[2].m_Translation.empty(),"250ms inclusive window/duplicate cue propagation");
 std::vector<SLyricLine> groupedTie={{1000,"A"},{1000,"B"},{1200,"C"},{1200,"D"}};TcLyric::AttachTranslatedLyrics(groupedTie,"[00:01.10]不确定");for(const auto &line:groupedTie)Check(line.m_Translation.empty(),"equal-distance duplicate groups were misaligned");
 std::vector<SLyricLine> self={{1000,"你好"},{1000,"original"}};TcLyric::AttachTranslatedLyrics(self,"[00:01.00]你好\\n[00:01.00]其他译文");Check(self[0].m_Translation=="其他译文"&&self[1].m_Translation=="你好","group winner lost the per-line self-translation exclusion");
 std::vector<SLyricLine> prior={{1000,"original"}};TcLyric::AttachTranslatedLyrics(prior,"[00:01.20]已附译文");TcLyric::AttachTranslatedLyrics(prior,"[00:01.00]低优先级");Check(prior[0].m_Translation=="已附译文","second provider field replaced an already attached translation");
 t.m_vLines=lines;g_Config.m_TcIslandDoubleLyrics=1;text.draws.clear();t.RenderNext(0,1,0,72,100,500,1,"face-a");
 Check(text.draws.size()==1&&text.draws[0].text=="你好世界","non-simplified current cue uses available translation");
 t.m_vLines[0].m_Translation.clear();text.draws.clear();t.RenderNext(0,1,0,72,100,500,1,"face-a");
 Check(text.draws[0].text=="Next line","missing translation restores current/next mode");
 t.m_vLines[0].m_IsSimplifiedChinese=true;t.m_vLines[0].m_Translation="不应使用";text.draws.clear();t.RenderNext(0,1,0,72,100,500,1,"face-a");
 Check(text.draws[0].text=="Next line","simplified lyrics retain next cue even if source provides translation");
 g_Config.m_TcIslandDoubleLyrics=0;text.draws.clear();t.RenderNext(0,1,0,72,100,500,1,"face-a");
 Check(text.draws[0].text=="Test song","single mode must not use translation");
 t.m_vLines={{1000,"current"},{3000,"A你🙂B"}};
 Check(g_Config.m_TcIslandDoubleLyrics==0,"double lyrics default must be disabled");
 g_Config.m_TcIslandDoubleLyrics=1;
 int cases=0;for(int enabled:{0,1})for(int font:{50,100,150,200})for(float scale:{.6f,1.f,1.8f,2.f})for(float y:{0.f,40.f,120.f})for(bool transparent:{false,true}){
  (void)transparent;g_Config.m_TcIslandDoubleLyrics=enabled;g_Config.m_TcFontSize=font;text.font=font/100.f;text.draws.clear();const auto l=t.RenderNext(0,scale,y,72*scale,100,500,.85f,"face-a");
  if(enabled)Check(l.currentY>=y&&l.nextY>=l.currentY+l.currentHeight&&l.nextY+l.nextHeight<=y+72*scale,"current/next rows overlap or exceed capsule");
  else Check(l.nextY>=y&&l.currentY>=l.nextY+l.nextHeight&&l.currentY+l.currentHeight<=y+72*scale,"title/current rows overlap or exceed capsule");
  Check(l.currentSize*text.font<=l.currentHeight+.001f&&l.nextSize*text.font<=l.nextHeight+.001f,"global font scaling overflows row height");
  Check(text.draws.size()==1&&text.draws[0].text==(enabled?"A你🙂B":"Test song")&&text.draws[0].alpha<.5f,"secondary text must match the selected mode");++cases;
 }
 text.font=1;g_Config.m_TcFontSize=100;
 for(int Toggle:{0,1,0,1}){
  g_Config.m_TcIslandDoubleLyrics=Toggle;text.draws.clear();
  const auto l=t.RenderNext(0,1,0,72,100,500,1,"face-b");
  Check(text.draws.size()==1,"mode switch duplicates the secondary row");
  if(Toggle){Check(text.draws[0].text=="A你🙂B"&&l.currentY<l.nextY,"enabled must show current above next");}
  else{Check(text.draws[0].text=="Test song"&&l.nextY<l.currentY&&l.nextHeight==12,"disabled must show title above one lyric");}
 }
 g_Config.m_TcIslandDoubleLyrics=1;
 char b[512];str_copy(b,"A你🙂B");Test::FitText(&text,10,b,10);Check(std::string(b)=="A你","UTF8 fit splits a glyph");str_copy(b,"A你🙂B");Test::FitText(&text,10,b,0);Check(!b[0],"zero width must not draw text");
 t.RenderNext(0,1,0,72,100,500,1,"face-b");int calls=text.calls;t.RenderNext(0,1,0,72,100,500,1,"face-b");Check(text.calls==calls,"steady next line must not remeasure");
 text.draws.clear();t.RenderNext(1,1,0,72,100,500,1,"face-b");Check(text.draws.empty()&&t.m_NextLyricText.empty(),"last cue repeats stale next lyric");
 std::printf("PASS: %d dual-line layout cases plus cue selection, UTF8, cache and last-line clearing\\n",cases);return 0;}catch(const std::exception&e){std::printf("FAIL: %s\\n",e.what());return 1;}}
`;
 fs.writeFileSync(out+'/test.cpp',program);
 function run(exe,args,env=process.env){const r=cp.spawnSync(exe,args,{cwd:out,env,encoding:'utf8',windowsHide:true,timeout:120000});if(r.error||r.status!==0)throw new Error((r.stdout||'')+(r.stderr||'')+(r.error?.message||''));return r.stdout;}
 const {discoverCompiler,compileCommands}=require('./test_compiler');
 const {binary,commands}=compileCommands(discoverCompiler(),{out,binaryName:'test',warnings:4,includes:[path.join(root,'src')],sources:[{file:path.join(out,'test.cpp')},{file:path.join(root,'src/game/client/components/tclient/lyric_parser.cpp')}]});
 for(const command of commands)run(command.executable,command.args,command.env);
 process.stdout.write(run(binary,[]));
}catch(e){console.error(e.message);process.exitCode=1;}finally{fs.rmSync(out,{recursive:true,force:true});}
