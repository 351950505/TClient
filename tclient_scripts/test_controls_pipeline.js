// Runs the actual input collection/reset/accessor bodies with checked array indexing.
// Client/network stubs are limited to mode selection; array sizes come from controls.h.
// Usage: node test_controls_pipeline.js --output <build-dir> [--source-ref <git-ref>]
const fs = require('fs');
const cp = require('child_process');
const path = require('path');
const root = path.resolve(__dirname, '..').replaceAll('\\', '/');
const index = process.argv.indexOf('--output');
if(index < 0 || !process.argv[index + 1]) throw Error('--output is required');
const work = path.resolve(process.argv[index + 1]).replaceAll('\\', '/');
fs.mkdirSync(work, {recursive: true});
const refIndex = process.argv.indexOf('--source-ref');
function readSource(file) {
    if(refIndex < 0) return fs.readFileSync(root + '/' + file, 'utf8');
    const result = cp.spawnSync('git', ['-C', root, 'show', process.argv[refIndex + 1] + ':' + file], {encoding: 'utf8'});
    if(result.status !== 0) throw Error(result.stderr);
    return result.stdout;
}
const header = readSource('src/game/client/components/controls.h');
const source = readSource('src/game/client/components/controls.cpp');
function body(name) {
    const start = source.indexOf('void CControls::' + name + '(') >= 0 ? source.indexOf('void CControls::' + name + '(') : source.indexOf('bool CControls::' + name + '(');
    if(start < 0) throw Error(name);
    const open = source.indexOf('{', start);
    let end = open + 1, depth = 1;
    while(depth && end < source.length) { if(source[end] === '{') ++depth; if(source[end] === '}') --depth; ++end; }
    return source.slice(start, end);
}
let fields = header.slice(header.indexOf('\tCNetObj_PlayerInput m_aFastInput'), header.indexOf('\n\tCControls();'));
fields = fields.replace(/(CNetObj_PlayerInput|bool) (m_\w+)\[([^\]]+)\]\[([^\]]+)\] = \{\};/g, 'Checked<Checked<$1, $4>, $3> $2{};');
fields = fields.replace(/(CNetObj_PlayerInput|bool) (m_\w+)\[([^\]]+)\] = \{\};/g, 'Checked<$1, $3> $2{};');
const accessors = header.split(/\r?\n/).filter(line => line.includes('GetFastInput(int Dummy)')).join('\n');
const bodies = [body('ResetFastInput'), body('CheckNewInput')].join('\n');
const fieldsUsed = [...new Set(bodies.match(/g_Config\.m_\w+/g))].map(s => s.slice(9));
const cpp = `#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <iterator>
constexpr int NUM_DUMMIES=2;
struct CNetObj_PlayerInput { int m_Direction=0,m_Hook=0,m_Fire=0,m_Jump=0,m_NextWeapon=0,m_PrevWeapon=0,m_WantedWeapon=0,m_TargetX=0,m_TargetY=0; bool operator==(const CNetObj_PlayerInput&)const=default; };
struct vec2 { float x=0,y=0; };
template<class T,size_t N> struct Checked { std::array<T,N> data{}; T&operator[](size_t i){return data.at(i);}const T&operator[](size_t i)const{return data.at(i);}operator T*(){return data.data();}auto begin(){return data.begin();}auto end(){return data.end();} };
struct {${fieldsUsed.map(f => 'int ' + f + '=0;').join('')}} g_Config;
struct FakeGame { int mode=0; bool active=true; struct{int m_LocalClientId=0;}m_Snap; int FastInputMode(){return mode;} bool FastInputActive(int,bool){return active;} };
class CControls { public:
${fields}
FakeGame game; FakeGame*GameClient(){return &game;}
CNetObj_PlayerInput m_aInputData[2]{},m_aLastData[2]{};
int m_aInputDirectionLeft[2]{},m_aInputDirectionRight[2]{};vec2 m_aMousePos[2]{};
void ResetFastInput(int Dummy); bool CheckNewInput();
${accessors}
};
${bodies}
int main(){int failed=0,cases=0; for(int mode: {0,1,2,3,5,6,7,8,9,10,11,12,13,14,15,16})for(int dummy:{0,1}) {
CControls c; c.game.mode=mode;g_Config={};g_Config.m_ClDummy=dummy;
c.m_aInputData[dummy].m_Hook=1;c.m_aInputData[dummy].m_Fire=1;c.m_aMousePos[dummy]={32,48};c.m_aInputDirectionLeft[dummy]=1;
try { ++cases;c.CheckNewInput();auto &input=c.GetFastInput(dummy); if(input.m_Direction!=-1||input.m_Hook!=1)throw std::runtime_error("wrong collected input");
const CControls &read=c;if(!(read.GetFastInput(dummy)==input))throw std::runtime_error("const accessor mismatch");
const auto other=c.GetFastInput(!dummy);c.ResetFastInput(dummy);if(!(c.GetFastInput(dummy)==CNetObj_PlayerInput{}))throw std::runtime_error("reset left input");if(!(c.GetFastInput(!dummy)==other))throw std::runtime_error("reset crossed dummy");
for(int next:{16,15,0,14,16}){c.game.mode=next;c.CheckNewInput();(void)c.GetFastInput(dummy);} }
catch(const std::exception&e){++failed;std::cout<<"FAIL mode="<<mode<<" dummy="<<dummy<<" "<<e.what()<<"\\n";}
}
for(int mode:{-1,17,999})for(int dummy:{0,1}){CControls c;g_Config={};g_Config.m_ClDummy=dummy;c.game.mode=mode;try{++cases;c.CheckNewInput();if(c.m_ActiveFastInputMode!=0)throw std::runtime_error("invalid mode not reset");c.m_ActiveFastInputMode=mode;(void)c.GetFastInput(dummy);const CControls&read=c;(void)read.GetFastInput(dummy);}catch(const std::exception&e){++failed;std::cout<<"FAIL invalid mode="<<mode<<" "<<e.what()<<"\\n";}}
std::cout<<"controls cases="<<cases<<" failures="<<failed<<"\\n";return failed?1:0;}
`;
fs.writeFileSync(work + '/controls_actual_calls.cpp', cpp);
fs.writeFileSync(work + '/build.cmd', '@echo off\r\ncall "C:\\VS2026\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\r\nif errorlevel 1 exit /b %errorlevel%\r\ncl /nologo /std:c++20 /EHsc /utf-8 "' + work + '/controls_actual_calls.cpp" /Fo:"' + work + '/controls_actual_calls.obj" /Fe:"' + work + '/controls_actual_calls.exe"\r\nif errorlevel 1 exit /b %errorlevel%\r\n"' + work + '/controls_actual_calls.exe"\r\nexit /b %errorlevel%\r\n');
const fd = fs.openSync(work + '/controls.log', 'w');
const result = cp.spawnSync('cmd.exe', ['/d', '/c', work + '/build.cmd'], {stdio: ['ignore', fd, fd]});
fs.closeSync(fd);
console.log('exit=' + result.status + '\n' + fs.readFileSync(work + '/controls.log', 'utf8').split('\n').slice(-38).join('\n'));
process.exit(result.status || 0);
