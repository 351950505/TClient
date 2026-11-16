// Offline RT-2 regression: compile current production function bodies, real
// JSON/Ed25519/SHA-256, and deterministic transport callbacks. Only TEMP files;
// no network, client, updater process, shared build, or personal configuration.
// node scripts/test_tclient_update_limits.js [absolute-source-root]
'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const root = path.resolve(process.argv[2] || path.join(__dirname, '..'));
const read = f => fs.readFileSync(path.join(root, f), 'utf8');
function body(text, signature) {
    const begin = text.indexOf(signature);
    assert(begin >= 0, `Missing ${signature}`);
    const start = text.indexOf('{', begin);
    let depth = 0, quote = '', comment = '';
    for (let i = start; i < text.length; ++i) {
        const c = text[i], next = text[i + 1];
        if (comment === '//') { if (c === '\n') comment = ''; continue; }
        if (comment === '/*') { if (c === '*' && next === '/') { comment = ''; ++i; } continue; }
        if (quote) { if (c === '\\') ++i; else if (c === quote) quote = ''; continue; }
        if (c === '/' && (next === '/' || next === '*')) { comment = c + next; ++i; continue; }
        if (c === '"' || c === "'") { quote = c; continue; }
        if (c === '{') ++depth;
        if (c === '}' && --depth === 0) return text.slice(begin, i + 1);
    }
    throw new Error(`Unbalanced ${signature}`);
}
const tc = read('src/game/client/components/tclient/tclient.cpp');
const th = read('src/game/client/components/tclient/tclient.h');
const http = read('src/engine/http.cpp');
const hh = read('src/engine/http.h');
const curl = read('src/engine/shared/http_curl.cpp');
const ch = read('src/engine/shared/http_curl.h');
const defs = (text, signatures) => signatures.map(s => body(text, s)).join('\n');
const httpDefs = defs(http, ['IHttpRequest::IHttpRequest(', 'IHttpRequest::~IHttpRequest(',
    'void IHttpRequest::WriteToFile(', 'void IHttpRequest::OnValidation(',
    'bool IHttpRequest::BeforeInit(', 'size_t IHttpRequest::OnData(',
    'void IHttpRequest::OnCompletionInternal(', 'void IHttpRequest::HeaderString(']);
const tcDefs = defs(tc, ['bool CTClient::VerifyUpdateManifestSignature(',
    'void CTClient::FinishUpdateManifest(', 'void CTClient::UpdateStartDownload(',
    'void CTClient::StartDownloadTask(', 'void CTClient::StartUpdateHelperTask(']);
const helpers = ['static int ParseAppVersion(', 'static int CompareAppVersion(',
    'static const json_value &JsonObj(', 'static const char *JsonStr(',
    'static int64_t JsonGetInt(', 'static bool IsHexSha256('];
if (tc.includes('static int64_t JsonGetUpdateSize(')) helpers.push('static int64_t JsonGetUpdateSize(');
const key = crypto.generateKeyPairSync('ed25519'); // ephemeral fixture key, never production signing key
const publicKey = key.publicKey.export({type:'spki', format:'der'}).subarray(-32);
const zip = Buffer.from('fixture ZIP signed bytes');
const helper = Buffer.from('fixture helper bytes');
const digest = b => crypto.createHash('sha256').update(b).digest('hex');
const base = JSON.stringify({product:'TClient', channel:'stable', platform:'win64', version:'9.0.0',
    asset:'TClient-update-win64.zip', sha256:digest(zip), size:zip.length,
    helper:{sha256:digest(helper), size:helper.length}});
const cases = [{name:'valid', json:base, valid:true},
    {name:'minimum', json:base.replace(`"size":${zip.length}`, '"size":1').replace(`"size":${helper.length}`, '"size":1'), valid:true},
    {name:'int64-max', json:base.replace(`"size":${zip.length}`, '"size":9223372036854775807').replace(`"size":${helper.length}`, '"size":9223372036854775807'), valid:true}];
for (const value of ['0', '-1', '9223372036854775808', '18446744073709551615', '1e300', '1.5', '"24"', 'null']) {
    for (const [kind, n] of [['ZIP',zip.length],['helper',helper.length]])
        cases.push({name:`${kind}-${value}`, json:base.replace(`"size":${n}`, `"size":${value}`), valid:false});
}
const bytes = b => '{' + [...b].join(',') + '}';
const caseDefs = cases.map((c,i) => {
    const msg = Buffer.from(c.json), sig = crypto.sign(null, msg, key.privateKey);
    return `const unsigned char msg${i}[] = ${bytes(msg)}, sig${i}[] = ${bytes(sig)}; ManifestCase("${c.name.replace(/"/g,'\\"')}", msg${i}, sizeof(msg${i}), sig${i}, ${c.valid});`;
}).join('\n');
const fixture = String.raw`
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <base/hash_ctxt.h>
#include <engine/external/json-parser/json.h>
#include <engine/external/ed25519/ed25519.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <curl/curl.h>
#undef ERROR
using IOHANDLE = FILE *;
constexpr int IO_MAX_PATH_LENGTH = 1024, IOFLAG_READ = 1, IOFLAG_WRITE = 2, IOFLAG_APPEND = 4;
static uint64_t s_Writes = 0, s_Reads = 0;
static int64_t s_ReportedSize = -1;
IOHANDLE io_open(const char *s, int flags) { return std::fopen(s, flags == IOFLAG_READ ? "rb" : flags == IOFLAG_APPEND ? "ab" : "wb"); }
int io_close(IOHANDLE f) { return std::fclose(f); }
unsigned io_read(IOHANDLE f, void *d, unsigned n) { ++s_Reads; return (unsigned)std::fread(d,1,n,f); }
unsigned io_write(IOHANDLE f, const void *d, unsigned n) { s_Writes += n; return (unsigned)std::fwrite(d,1,n,f); }
int64_t io_length(IOHANDLE f) { if(s_ReportedSize >= 0) return s_ReportedSize; auto old = std::ftell(f); std::fseek(f,0,SEEK_END); auto n=std::ftell(f); std::fseek(f,old,SEEK_SET); return n; }
bool io_read_all(IOHANDLE f, void **out, unsigned *n) { auto size=io_length(f); if(size>1024LL*1024*1024) return false; *out=std::malloc((size_t)size+1); *n=io_read(f,*out,(unsigned)size+1); return true; }
int fs_is_file(const char *s) { return std::filesystem::is_regular_file(s); }
int fs_makedir_rec_for(const char *) { return 0; }
int fs_file_time(const char *, time_t *, time_t *) { return -1; }
int fs_remove(const char *s) { return std::remove(s); }
int fs_rename(const char *a, const char *b) { return std::rename(a,b); }
void mem_copy(void *a, const void *b, size_t n) { std::memcpy(a,b,n); }
int mem_comp(const void *a, const void *b, size_t n) { return std::memcmp(a,b,n); }
void str_copy(char *d, const char *s, size_t n) { std::snprintf(d,n,"%s",s); }
template<size_t N> void str_copy(char (&d)[N], const char *s) { str_copy(d,s,N); }
void str_format(char *d,size_t n,const char *f,...) { va_list a; va_start(a,f); std::vsnprintf(d,n,f,a); va_end(a); }
int str_length(const char *s) { return (int)std::strlen(s); }
int str_comp(const char *a,const char *b) { return std::strcmp(a,b); }
void dbg_assert(bool b,const char *s) { if(!b) throw std::runtime_error(s); }
template<typename... T> void log_info(T...) {}
template<typename... T> void log_error(T...) {}
template<typename... T> void log_warn(T...) {}
struct { int m_DbgHttp=0,m_HttpAllowInsecure=0; char m_Bindaddr[64]=""; } g_Config;
struct IStorage {
    enum { TYPE_SAVE=0 };
    void GetBinaryPath(const char *s,char *d,size_t n) { str_copy(d,s,n); }
    void GetCompletePath(int,const char *s,char *d,size_t n) { str_copy(d,s,n); }
    static void FormatTmpPath(char *d,size_t n,const char *s) { str_format(d,n,"%s.tmp",s); }
};
${hh.slice(hh.indexOf('enum class EHttpState'), hh.indexOf('class IHttpRequest'))}
${body(hh,'class IHttpRequest').replace('protected:', 'public:')};
const char *const IHttpRequest::USER_AGENT_STRING = "RT-2 fixture";
${httpDefs}
void IHttpRequest::Result(unsigned char **p,size_t *n) const { *p=m_pBuffer; *n=(size_t)m_ResponseLength; }
static int s_JsonCalls=0;
json_value *IHttpRequest::ResultJson() const { ++s_JsonCalls; return json_parse((const char*)m_pBuffer,(size_t)m_ResponseLength); }
struct SOptions { curl_off_t max=-1,resume=0; long status=206; };
template<typename T> CURLcode TestSetOpt(CURL *h,CURLoption o,T v) { if constexpr(std::is_integral_v<T>) { auto &s=*(SOptions*)h; if(o==CURLOPT_MAXFILESIZE_LARGE) s.max=v; if(o==CURLOPT_RESUME_FROM_LARGE) s.resume=v; } return CURLE_OK; }
#define curl_easy_setopt TestSetOpt
curl_version_info_data *TestVersion(CURLversion) { static curl_version_info_data v{}; v.version_num=0x080000; return &v; }
#define curl_version_info TestVersion
CURLcode TestGetInfo(CURL *h,CURLINFO,long *d) { *d=((SOptions*)h)->status; return CURLE_OK; }
#define curl_easy_getinfo TestGetInfo
const char *TestCurlError(CURLcode) { return "fixture transport failure"; }
#define curl_easy_strerror TestCurlError
void CurlDebug() {}
${body(ch,'class CHttpRequestCurl').replace('private:', 'public:')};
CHttpRequestCurl::CHttpRequestCurl(const char *s):IHttpRequest(s) { m_aErr[0]=0; }
// Fixture exception cleanup only; production completion/destructor bodies remain tested.
CHttpRequestCurl::~CHttpRequestCurl() { if(m_File) { io_close(m_File); m_File=nullptr; } }
void CHttpRequestCurl::Header(const char *) {}
int CHttpRequestCurl::ProgressCallback(void *,double,double,double,double) { return 0; }
size_t CHttpRequestCurl::HeaderCallback(char *,size_t,size_t n,void *) { return n; }
${defs(curl,['bool CHttpRequestCurl::ConfigureHandle(','size_t CHttpRequestCurl::WriteCallback(','void CHttpRequestCurl::OnCompletionInternal('])}
std::unique_ptr<IHttpRequest> HttpGetFile(const char *u,IStorage *s,const char *d,int t) { auto p=std::make_unique<CHttpRequestCurl>(u); p->WriteToFile(s,d,t); return p; }
const char *TCLocalize(const char *s) { return s; }
constexpr int APP_VERSION_MAX_SEGMENTS=8;
constexpr const char *TCLIENT_UPDATE_PRODUCT="TClient", *TCLIENT_UPDATE_CHANNEL="stable", *TCLIENT_UPDATE_PLATFORM="win64", *TCLIENT_APP_VERSION="1.0.0";
constexpr const char *TCLIENT_UPDATE_ZIP_ASSET="TClient-update-win64.zip", *TCLIENT_UPDATE_HELPER_ASSET="TClientUpdater.exe";
constexpr const char *TCLIENT_UPDATE_ZIP_PATH="zip.bin", *TCLIENT_UPDATE_BOOTSTRAP_PATH="helper.bin";
constexpr const char *TCLIENT_UPDATE_MANIFEST_SOURCES[]={"https://fixture/manifest"};
constexpr const char *TCLIENT_UPDATE_SOURCE_SIBLING_BASES[]={"https://fixture/"};
constexpr int TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT=1;
const unsigned char s_aUpdateEd25519PublicKey[]=${bytes(publicKey)};
bool UpdateSourceIsApi(const char *) { return false; }
void FormatUpdateAssetUrl(char *d,size_t n,const char *b,const char *t,const char *a) { str_format(d,n,"%s%s/%s",b,t,a); }
${defs(tc, helpers)}
class CTClient {
public:
${th.slice(th.indexOf('\tenum class EUpdateState'),th.indexOf('\nprivate:'))}
    EUpdateState m_UpdateState=EUpdateState::IDLE;
    SUpdateRelease m_UpdateRelease;
    int m_UpdateManifestSource=0,m_UpdateDownloadAttempt=0,m_UpdateDownloadSource=0;
    bool m_UpdateFoundCurrentRelease=false;
    char m_aUpdateApiHelperUrl[512]="",m_aUpdateHelperUrl[512]="",m_aUpdateBootstrapPath[1024]="";
    std::shared_ptr<IHttpRequest> m_pUpdateDownloadTask,m_pUpdateHelperTask;
    struct SFakeHttp { int runs=0; void Run(std::shared_ptr<IHttpRequest>) { ++runs; } } http;
    IStorage storage;
    IStorage *Storage() { return &storage; }
    SFakeHttp *Http() { return &http; }
    void ResetUpdateTasks() { m_pUpdateDownloadTask.reset(); m_pUpdateHelperTask.reset(); }
    void SetUpdateStatus(const char *) {}
    void UpdateFail(const char *) { ResetUpdateTasks(); m_UpdateRelease.m_Valid=false; m_UpdateState=EUpdateState::FAILED; }
    void NextUpdateManifestSource(const char *s) { UpdateFail(s); }
    void StartApiResolveTask(bool) { throw std::runtime_error("unexpected network API resolution"); }
    bool VerifyUpdateManifestSignature(const unsigned char *,size_t,const unsigned char *,size_t);
    void FinishUpdateManifest(std::shared_ptr<IHttpRequest>,std::shared_ptr<IHttpRequest>);
    void UpdateStartDownload(); void StartDownloadTask(); void StartUpdateHelperTask();
};
${tcDefs}
static int s_Failures=0,s_Passes=0;
void Check(bool b,const char *s) { if(!b) throw std::runtime_error(s); }
template<typename F> void Test(const char *name,F f) { try { f(); ++s_Passes; std::printf("PASS %s\n",name); } catch(const std::exception &e) { ++s_Failures; std::printf("FAIL %s: %s\n",name,e.what()); } }
std::shared_ptr<CHttpRequestCurl> Memory(const unsigned char *b,size_t n) { auto r=std::make_shared<CHttpRequestCurl>("https://fixture"); r->m_pBuffer=(unsigned char*)std::malloc(n); std::memcpy(r->m_pBuffer,b,n); r->m_ResponseLength=n; return r; }
void ManifestCase(const char *name,const unsigned char *msg,size_t n,const unsigned char *sig,bool valid) { Test(name,[&] { CTClient t; t.FinishUpdateManifest(Memory(sig,64),Memory(msg,n)); Check(t.m_UpdateRelease.m_Valid==valid,"signed size boundary validation"); Check(t.http.runs==0,"manifest must not auto-download"); if(valid) { t.UpdateStartDownload(); Check(t.m_pUpdateDownloadTask && t.m_pUpdateDownloadTask->m_MaxResponseSize==t.m_UpdateRelease.m_Size,"ZIP budget must equal signed size"); Check(t.m_pUpdateDownloadTask->m_ExpectedSha256.has_value() && t.m_pUpdateDownloadTask->m_AllowResume,"ZIP hash/resume retained"); t.StartUpdateHelperTask(); Check(t.m_pUpdateHelperTask && t.m_pUpdateHelperTask->m_MaxResponseSize==t.m_UpdateRelease.m_HelperSize,"helper budget must equal signed size"); Check(t.m_pUpdateHelperTask->m_ExpectedSha256.has_value() && t.m_pUpdateHelperTask->m_AllowResume,"helper hash/resume retained"); } }); }
void Put(const char *s,const std::string &data) { auto f=io_open(s,IOFLAG_WRITE); Check(f!=nullptr,"fixture open"); std::fwrite(data.data(),1,data.size(),f); io_close(f); }
std::string Get(const char *s) { auto f=io_open(s,IOFLAG_READ); if(!f) return ""; char b[256]; size_t n=std::fread(b,1,sizeof(b),f); io_close(f); return std::string(b,n); }
const std::string ZIP=${JSON.stringify(zip.toString())}, HELPER=${JSON.stringify(helper.toString())};
SHA256_DIGEST Digest(const std::string &s) { SHA256_CTX c; sha256_init(&c); sha256_update(&c,s.data(),s.size()); return sha256_finish(&c); }
void Stream(const char *name,const std::string &data,bool resume) { Test(name,[&] { const char *dest="stream.bin"; fs_remove(dest); fs_remove("stream.bin.tmp"); IStorage storage; CHttpRequestCurl r("https://fixture"); r.WriteToFile(&storage,dest,0); r.MaxResponseSize((int64_t)data.size()); r.AllowResume(true); r.ExpectSha256(Digest(data)); const size_t prefix=resume?7:0; if(prefix) Put("stream.bin.tmp",data.substr(0,prefix)); SOptions options; Check(r.ConfigureHandle((CURL*)&options),"configure transfer"); Check(options.max==(curl_off_t)data.size() && options.resume==(curl_off_t)prefix,"curl max is complete entity budget, not Range body budget"); Check(r.m_ResponseLength==0,"response length excludes saved prefix"); auto bytes=data.size()-prefix; Check(CHttpRequestCurl::WriteCallback((char*)data.data()+prefix,1,bytes,&r)==bytes,"valid suffix accepted"); Check(r.m_ResponseLength==bytes,"Range response length stays suffix-only"); auto writes=s_Writes; auto hash=r.m_ActualSha256Ctx; Check(CHttpRequestCurl::WriteCallback((char*)"!",1,1,&r)==0,"extra byte must reject before transport EOF"); Check(s_Writes==writes && std::memcmp(&hash,&r.m_ActualSha256Ctx,sizeof(hash))==0,"rejection before disk write and hash mutation"); r.OnCompletionInternal((CURL*)&options,CURLE_WRITE_ERROR); Check(r.State()==EHttpState::ERROR && !fs_is_file(dest) && !fs_is_file("stream.bin.tmp"),"rejected temp removed, never installed"); }); }
void Complete(const char *name,const std::string &data) { Test(name,[&] { fs_remove("complete.bin"); Put("complete.bin.tmp",data.substr(0,7)); IStorage s; CHttpRequestCurl r("https://fixture"); r.WriteToFile(&s,"complete.bin",0); r.MaxResponseSize((int64_t)data.size()); r.AllowResume(true); r.ExpectSha256(Digest(data)); SOptions o; Check(r.ConfigureHandle((CURL*)&o),"resume setup"); Check(r.OnData(data.data()+7,data.size()-7)==data.size()-7,"legal suffix"); r.OnCompletionInternal((CURL*)&o,CURLE_OK); Check(r.State()==EHttpState::DONE && Get("complete.bin")==data,"partial prefix included in full hash and committed file"); }); }
int main() {
${caseDefs}
    Test("signature-before-JSON/tamper/short-signature",[&] { const unsigned char msg[]=${bytes(Buffer.from(base))}, sig[]=${bytes(crypto.sign(null,Buffer.from(base),key.privateKey))}; CTClient t; auto m=Memory(msg,sizeof(msg)); m->m_pBuffer[0]^=1; auto calls=s_JsonCalls; t.FinishUpdateManifest(Memory(sig,64),m); Check(!t.m_UpdateRelease.m_Valid && s_JsonCalls==calls,"tamper rejected before parse"); Check(!t.VerifyUpdateManifestSignature(msg,sizeof(msg),sig,63),"short Ed25519 signature rejected"); });
    Stream("ZIP unknown-length excess",ZIP,false); Stream("helper unknown-length excess",HELPER,false);
    Stream("ZIP Range excess counts prefix",ZIP,true); Stream("helper Range excess counts prefix",HELPER,true);
    Complete("ZIP legal 206 resume/full SHA256",ZIP); Complete("helper legal 206 resume/full SHA256",HELPER);
    Test("0 budget rejects first byte",[] { CHttpRequestCurl r("https://fixture"); r.MaxResponseSize(0); Check(r.OnData("!",1)==0 && r.m_ResponseLength==0,"zero cap"); });
    Test("uint64 accumulation overflow rejected",[] { CHttpRequestCurl r("https://fixture"); r.m_WriteToMemory=false; r.MaxResponseSize(INT64_MAX); r.m_ResponseLength=UINT64_MAX; Check(r.OnData("!",1)==0 && r.m_ResponseLength==UINT64_MAX,"addition must not wrap"); });
    Test("oversized prefix discarded before read",[] { Put("prefix.bin.tmp","oversize"); IStorage s; CHttpRequestCurl r("https://fixture"); r.WriteToFile(&s,"prefix.bin",0); r.MaxResponseSize(4); r.AllowResume(true); auto reads=s_Reads; SOptions o; Check(r.ConfigureHandle((CURL*)&o),"restart setup"); Check(r.m_ResumeFrom==0 && o.resume==0 && s_Reads==reads,"oversized prefix not hashed/read/resumed"); r.OnCompletionInternal((CURL*)&o,CURLE_WRITE_ERROR); });
    Test("prefix length mismatch cannot poison resume",[] { Put("mismatch.bin.tmp","abc"); s_ReportedSize=7; IStorage s; CHttpRequestCurl r("https://fixture"); r.WriteToFile(&s,"mismatch.bin",0); r.MaxResponseSize(10); r.AllowResume(true); SOptions o; Check(r.ConfigureHandle((CURL*)&o),"setup mismatch prefix"); s_ReportedSize=-1; Check(r.m_ResumeFrom==0,"short read must restart without hashing partial prefix"); Check(r.OnData("abcdefghij",10)==10,"restart full data"); r.ExpectSha256(Digest("abcdefghij")); r.OnCompletionInternal((CURL*)&o,CURLE_OK); Check(r.State()==EHttpState::DONE,"restart hash context clean"); });
    Test("bad Range drops partial; next attempt restarts",[] { Put("range.bin.tmp","abc"); IStorage s; CHttpRequestCurl r("https://fixture"); r.WriteToFile(&s,"range.bin",0); r.MaxResponseSize(10); r.AllowResume(true); SOptions o; Check(r.ConfigureHandle((CURL*)&o),"range setup"); r.OnCompletionInternal((CURL*)&o,CURLE_RANGE_ERROR); Check(!fs_is_file("range.bin.tmp") && r.State()==EHttpState::ERROR,"ignored/invalid Range cannot append full body"); });
    Test("curl declared oversized entity rejection",[] { Put("declared.bin.tmp","abc"); IStorage s; CHttpRequestCurl r("https://fixture"); r.WriteToFile(&s,"declared.bin",0); r.MaxResponseSize(10); r.AllowResume(true); SOptions o; Check(r.ConfigureHandle((CURL*)&o),"declared length setup"); auto writes=s_Writes; r.OnCompletionInternal((CURL*)&o,CURLE_FILESIZE_EXCEEDED); Check(s_Writes==writes && !fs_is_file("declared.bin.tmp"),"curl rejected entity never retains rejected partial"); });
    Test("default unlimited remains unchanged",[] { CHttpRequestCurl r("https://fixture"); Check(r.m_MaxResponseSize==-1,"global default unchanged"); Check(r.OnData("abc",3)==3,"unconfigured ordinary HTTP remains unchanged"); });
    std::printf("RESULT %d passed, %d failed\n",s_Passes,s_Failures); return s_Failures?1:0;
}
`;
function run() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(),'tclient-update-limits-'));
    try {
        fs.writeFileSync(path.join(dir,'fixture.cpp'),fixture);
        const sources = ['src/base/hash_libtomcrypt.cpp', 'src/engine/external/json-parser/json.c',
            ...['verify','fe','ge','sc','sha512'].map(n=>`src/engine/external/ed25519/${n}.c`)];
        // Only needed digest utilities, avoiding unrelated hash/MD5 link targets.
        fs.writeFileSync(path.join(dir,'digest.cpp'), '#include <base/hash_ctxt.h>\n#include <cstring>\n' +
            defs(read('src/base/hash.cpp'), ['static void digest_str(', 'void sha256_str(', 'int sha256_from_str(', 'int sha256_comp('])
                .replace('return str_hex_decode(out->data, sizeof(out->data), str);', 'if(std::strlen(str)!=64) return -1; for(int i=0;i<32;++i) { unsigned x; if(std::sscanf(str+i*2,"%2x",&x)!=1) return -1; out->data[i]=(unsigned char)x; } return 0;')
                .replace('mem_comp(', 'std::memcmp(').replace('#include <cstring>', '#include <cstring>')
                .replace('static void digest_str(', 'static void digest_str('));
        // sscanf is fixture glue; SHA256 itself and comparisons are production code.
        fs.writeFileSync(path.join(dir,'digest.cpp'),'#include <cstdio>\n'+fs.readFileSync(path.join(dir,'digest.cpp'),'utf8'));
        const include = [path.join(root,'src'),path.join(root,'ddnet-libs/curl/include')];
        if (process.platform==='win32') {
            const vswhere=path.join(process.env['ProgramFiles(x86)']||'C:/Program Files (x86)','Microsoft Visual Studio/Installer/vswhere.exe');
            const vs=cp.execFileSync(vswhere,['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],{encoding:'utf8'}).trim();
            assert(vs,'MSVC tools not found');
            const args=['/nologo','/std:c++17','/EHsc','/W3','/utf-8',...include.map(p=>`/I"${p}"`),'fixture.cpp','digest.cpp',...sources.map(s=>`"${path.join(root,s)}"`),'/Fe:fixture.exe'];
            cp.execSync(`call "${path.join(vs,'VC/Auxiliary/Build/vcvars64.bat')}" >nul && cl ${args.join(' ')}`,{cwd:dir,encoding:'utf8',shell:process.env.ComSpec||'cmd.exe',stdio:'pipe'});
        } else {
            cp.execFileSync(process.env.CXX||'c++',['-std=c++17',...include.flatMap(p=>['-I',p]),'fixture.cpp','digest.cpp',...sources.map(s=>path.join(root,s)),'-o','fixture'],{cwd:dir,encoding:'utf8',stdio:'pipe'});
        }
        const result=cp.spawnSync(path.join(dir,process.platform==='win32'?'fixture.exe':'fixture'),[],{cwd:dir,encoding:'utf8',timeout:15000});
        process.stdout.write(result.stdout||'');
        assert.strictEqual(result.status,0,result.stderr||'real-function fixture failed');
        console.log(`PASS RT-2 isolated real-function fixture: ${root}`);
    } finally { fs.rmSync(dir,{recursive:true,force:true}); }
}
try { run(); } catch(e) { console.error(String(e.stdout||'').trim()); console.error(String(e.stderr||e.message).trim()); process.exitCode=1; }
