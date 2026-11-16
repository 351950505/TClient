// Offline regression seam: compile the current real updater/lifecycle bodies with
// fake HTTP, filesystem, buttons and process APIs. Never starts the client or helper.
// Usage: node scripts/test_tclient_manual_update.js [absolute-source-root]
// Requires MSVC Build Tools (Windows), or CXX/clang++/g++ elsewhere.
'use strict';
const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const cp = require('child_process');
const root = path.resolve(process.argv[2] || path.join(__dirname, '..'));
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const source = read('src/game/client/components/tclient/tclient.cpp');
const header = read('src/game/client/components/tclient/tclient.h');
const menu = read('src/game/client/components/tclient/menus_tclient.cpp');
const engine = read('src/engine/client/client.cpp');

// Scan C++ braces while ignoring comments and quoted literals, not a copied model.
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
const definitions = [
    'void CTClient::SetUpdateStatus(', 'void CTClient::UpdateFail(',
    'void CTClient::ResetUpdateTasks(', 'void CTClient::UpdateCheck(',
    'void CTClient::FetchUpdateManifest(', 'void CTClient::NextUpdateManifestSource(',
    'void CTClient::UpdateStartDownload(', 'void CTClient::StartDownloadTask(',
    'void CTClient::FinishUpdateDownload(', 'void CTClient::StartUpdateHelperTask(',
    'void CTClient::RetryUpdateDownload(', 'void CTClient::FinishUpdateBootstrap(',
    'void CTClient::UpdateInstallAndRestart(', 'void CTClient::PollUpdateTasks(',
    'void CTClient::OnRender(', 'void CTClient::OnShutdown(',
    'void CTClient::ConUpdateCheck(',
];
if (source.includes('bool CTClient::UpdateShouldAutoCheck('))
    definitions.push('bool CTClient::UpdateShouldAutoCheck(');
const bodies = definitions.map(signature => body(source, signature)).join('\n\n');
const enumAndRelease = header.slice(header.indexOf('\tenum class EUpdateState'), header.indexOf('\nprivate:'));
const internals = header.slice(header.indexOf('\tEUpdateState m_UpdateState'), header.indexOf('\n\t// 游戏内中键玩家菜单'))
    .replace(/\[\[gnu::format\(printf, 2, 3\)\]\]/g, '');
const buttonFlow = menu.match(/\t*if\(DoButton_Menu\(&s_CheckButton[^\n]*\n[^\n]*TC\.UpdateCheck\([^\n]*\n\t*if\(DoButton_Menu\(&s_DownloadButton[^\n]*\n[^\n]*TC\.UpdateStartDownload\([^\n]*\n\t*if\(DoButton_Menu\(&s_InstallButton[^\n]*\n[^\n]*TC\.UpdateInstallAndRestart\([^\n]*/);
assert(buttonFlow, 'Missing actual three-step update button block');
const legacyAutomatic = /FetchTClientInfo\s*\(/.test(body(source, 'void CTClient::OnInit('));
const oldTiming = source.match(/^static constexpr (?:float|int64_t) TCLIENT_UPDATE_(?:FIRST_CHECK_DELAY|CHECK_INTERVAL)[^\n]*/gm) || [];

const fixture = String.raw`
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#define CONF_FAMILY_WINDOWS 1
#define CLIENT_EXEC "DDNet"
#define PLAT_EXT ".exe"
#define MB_ERR_INVALID_CHARS 0
#define CP_UTF8 0
constexpr int IO_MAX_PATH_LENGTH = 1024, IOFLAG_READ = 1;
constexpr const char *TCLIENT_APP_VERSION = "1.7.8.84";
constexpr const char *TCLIENT_UPDATE_MANIFEST_SOURCES[] = {"https://one/manifest", "https://two/manifest"};
constexpr const char *TCLIENT_UPDATE_SOURCE_SIBLING_BASES[] = {"https://one/", "https://two/"};
constexpr int TCLIENT_UPDATE_DOWNLOAD_SOURCE_COUNT = 2;
constexpr const char *TCLIENT_UPDATE_ZIP_PATH = "update/TClient-update-win64.zip";
constexpr const char *TCLIENT_UPDATE_BOOTSTRAP_PATH = "update/TClientUpdater.boot.exe";
constexpr const char *TCLIENT_UPDATE_ZIP_ASSET = "TClient-update-win64.zip";
constexpr const char *TCLIENT_UPDATE_HELPER_ASSET = "TClientUpdater.exe";
constexpr const char *TCLIENT_UPDATE_HELPER = "TClientUpdater.exe";
${oldTiming.join('\n')}
static int64_t s_Now = 100000;
int64_t time_timestamp() { return s_Now; }
const char *TCLocalize(const char *s) { return s; }
template<typename... Args> void log_info(Args...) {}
template<typename... Args> void log_warn(Args...) {}
void str_copy(char *d, const char *s, size_t n) { std::snprintf(d, n, "%s", s); }
template<size_t N> void str_copy(char (&d)[N], const char *s) { str_copy(d, s, N); }
void str_format(char *d, size_t n, const char *f, ...) { va_list a; va_start(a, f); std::vsnprintf(d, n, f, a); va_end(a); }
void str_format_v(char *d, size_t n, const char *f, va_list a) { std::vsnprintf(d, n, f, a); }
struct SHA256_DIGEST {};
int sha256_from_str(SHA256_DIGEST *, const char *s) { return std::strlen(s) == 64 ? 0 : -1; }
bool UpdateSourceIsApi(const char *) { return false; }
void FormatUpdateAssetUrl(char *d, size_t n, const char *base, const char *tag, const char *asset) { str_format(d, n, "%s%s/%s", base, tag, asset); }
struct CTimeout { int a,b,c,d; };
enum class IPRESOLVE { V4 };
enum class EHttpState { QUEUED, DONE, ERROR, ABORTED };
struct IHttpRequest {
    std::string url;
    EHttpState state = EHttpState::QUEUED;
    int aborts = 0;
    bool hash = false, resume = false;
    explicit IHttpRequest(const char *s): url(s) {}
    void Abort() { ++aborts; state = EHttpState::ABORTED; }
    bool Done() const { return state != EHttpState::QUEUED; }
    EHttpState State() const { return state; }
    void Timeout(CTimeout) {}
    void IpResolve(IPRESOLVE) {}
    void MaxResponseSize(int) {}
    void FailOnErrorStatus(bool) {}
    void HeaderString(const char *, const char *) {}
    void ExpectSha256(SHA256_DIGEST) { hash = true; }
    void ValidateBeforeOverwrite(bool) {}
    void AllowResume(bool value) { resume = value; }
};
struct SFakeHttp {
    std::vector<std::shared_ptr<IHttpRequest>> requests;
    void Run(std::shared_ptr<IHttpRequest> r) { requests.push_back(r); }
};
struct IStorage {
    enum { TYPE_SAVE, TYPE_ALL };
    bool zipExists = true, helperExists = true;
    int removeCalls = 0;
    void GetCompletePath(int, const char *file, char *d, size_t n) { str_format(d, n, "C:/fixture/%s", file); }
    void GetBinaryPathAbsolute(const char *file, char *d, size_t n) { str_format(d, n, "C:/fixture/%s", file); }
};
std::shared_ptr<IHttpRequest> HttpGet(const char *s) { return std::make_shared<IHttpRequest>(s); }
std::shared_ptr<IHttpRequest> HttpGetFile(const char *s, IStorage *, const char *, int) { return HttpGet(s); }
static bool s_FileExists = true, s_ProcessFails = false;
static int64_t s_ZipLength = 100, s_HelperLength = 20;
using IOHANDLE = int64_t *;
IOHANDLE io_open(const char *s, int) { if(!s_FileExists) return nullptr; return std::strstr(s, ".zip") ? &s_ZipLength : &s_HelperLength; }
int64_t io_length(IOHANDLE f) { return *f; }
void io_close(IOHANDLE) {}
int fs_is_file(const char *) { return s_FileExists; }
int fs_parent_dir(char *s) { char *p = std::strrchr(s, '/'); if(!p) return -1; *p = 0; return 0; }
int process_id() { return 123; }
int MultiByteToWideChar(int, int, const char *, int, wchar_t *, size_t) { return 0; }
void Sleep(int) {}
static int s_StopHelpers = 0;
void StopOwnHelperProcesses(const wchar_t *, bool = false) { ++s_StopHelpers; }
using PROCESS = int;
constexpr int INVALID_PROCESS = -1;
enum class EShellExecuteWindowState { BACKGROUND };
static int s_ProcessStarts = 0;
static std::vector<std::string> s_LastArguments;
PROCESS process_execute(const char *, EShellExecuteWindowState, const char **args, size_t n) {
    ++s_ProcessStarts; s_LastArguments.assign(args, args + n); return s_ProcessFails ? INVALID_PROCESS : 1;
}
struct IConsole { struct IResult {}; };
struct IClient { enum { STATE_QUITTING, STATE_RESTARTING }; };
struct CClient {
    float now = 0;
    int quits = 0, state = -1;
    float LocalTime() const { return now; }
    void SetState(int s) { state = s; if(s == IClient::STATE_QUITTING) ++quits; }
    void Quit();
    void Restart();
};
${body(engine, 'void CClient::Quit(')}
${body(engine, 'void CClient::Restart(')}
class CTClient {
public:
${enumAndRelease}
${internals}
    SFakeHttp http;
    IStorage storage;
    CClient client;
    std::shared_ptr<IHttpRequest> m_pTClientInfoTask;
    SFakeHttp *Http() { return &http; }
    IStorage *Storage() { return &storage; }
    CClient *Client() { return &client; }
    void FinishTClientInfo() {}
    void ResetTClientInfoTask() { m_pTClientInfoTask.reset(); }
    void DoFinishCheck() {}
    void RenderPlayerMiddleMenu() {}
    void OnRender();
    void OnShutdown();
    void UpdateCheck(bool);
    void UpdateStartDownload();
    void UpdateInstallAndRestart();
    static void ConUpdateCheck(IConsole::IResult *, void *);
    // Only the validated JSON/crypto result boundary is faked. Polling, HTTP
    // signature pairing, all reset/failure/download/install bodies are real.
    void SeedVerifiedRelease() {
        m_UpdateRelease.m_Valid = true;
        str_copy(m_UpdateRelease.m_aVersion, "1.7.8.85");
        str_copy(m_UpdateRelease.m_aTag, "v1.7.8.85");
        str_copy(m_UpdateRelease.m_aSha256, std::string(64, 'a').c_str());
        str_copy(m_UpdateRelease.m_aHelperSha256, std::string(64, 'b').c_str());
        m_UpdateRelease.m_Size = 100; m_UpdateRelease.m_HelperSize = 20;
        m_UpdateState = EUpdateState::AVAILABLE;
    }
};
void CTClient::FinishUpdateManifest(std::shared_ptr<IHttpRequest> sig, std::shared_ptr<IHttpRequest> manifest) {
    if(!sig || !manifest || sig->State() != EHttpState::DONE || manifest->State() != EHttpState::DONE)
        throw std::runtime_error("incorrect manifest/signature snapshot");
    SeedVerifiedRelease();
}
void CTClient::StartApiResolveTask(bool) { throw std::runtime_error("unexpected API source"); }
void CTClient::FinishApiResolve() { throw std::runtime_error("unexpected API result"); }
${bodies}
struct CButtonContainer {};
struct CUIRect {};
static CButtonContainer s_CheckButton, s_DownloadButton, s_InstallButton;
static CUIRect CheckRect, DownloadRect, InstallRect;
static const CButtonContainer *s_Clicked = nullptr;
bool DoButton_Menu(CButtonContainer *b, const char *, int, CUIRect *) { return b == s_Clicked; }
void Menu(CTClient &TC, const CButtonContainer *click = nullptr) {
    const auto State = TC.m_UpdateState;
    s_Clicked = click;
${buttonFlow[0]}
    s_Clicked = nullptr;
}
void Check(bool b, const char *s) { if(!b) throw std::runtime_error(s); }
void CompleteCheck(CTClient &t) {
    t.m_pUpdateManifestTask->state = EHttpState::DONE;
    t.OnRender();
    Check(t.m_pUpdateManifestSigTask != nullptr, "manual check must request the paired signature");
    Check(t.m_pUpdateManifestSigTask->url == std::string(t.m_aUpdateManifestUrl) + ".sig", "signature must match manifest source");
    t.m_pUpdateManifestSigTask->state = EHttpState::DONE;
    t.OnRender();
    Check(t.m_UpdateState == CTClient::EUpdateState::AVAILABLE && t.m_UpdateRelease.m_Valid, "validated result must write back available state");
}
void CheckStaleCleared(CTClient &t) {
    Check(!t.m_UpdateRelease.m_Valid && t.m_UpdateManifestSource == 0 && !t.m_UpdateFoundCurrentRelease &&
        t.m_UpdateDownloadAttempt == 0 && t.m_UpdateDownloadSource == 0 && !t.BootstrapHelperPending &&
        t.m_UpdateRetryAt < 0 && !t.m_pUpdateDownloadTask && !t.m_pUpdateHelperTask && !t.m_pUpdateApiResolveTask &&
        !t.m_pUpdateManifestSigTask && !t.m_aUpdateReleaseTag[0] && !t.m_aUpdateHelperUrl[0] && !t.m_aUpdateBootstrapPath[0],
        "fresh manual check must clear stale release/tasks/retry/helper state");
}
int main() { try {
    using State = CTClient::EUpdateState;
    CTClient idle;
    for(float now : {0.f, 11.f, 12.f, 60.f, 21601.f, 86400.f}) { idle.client.now = now; s_Now += 21601; idle.OnRender(); Menu(idle); }
    if(!idle.http.requests.empty()) std::printf("REPRO: startup/timer requested %zu manifest(s), state=%d\n", idle.http.requests.size(), int(idle.m_UpdateState));
    Check(idle.http.requests.empty() && idle.m_UpdateState == State::IDLE, "startup/timer/render must never check or download");
    Check(!${legacyAutomatic}, "OnInit still automatically requests legacy TClient version info");
    idle.UpdateCheck(false);
    Check(idle.http.requests.empty() && idle.m_UpdateState == State::IDLE, "non-explicit check must be a no-op");
    Check(s_ProcessStarts == 0 && idle.client.quits == 0, "idle must never install or quit");
    std::puts("PASS idle/startup/timer/menu-render/non-explicit request");

    Menu(idle, &s_CheckButton);
    Check(idle.http.requests.size() == 1 && idle.m_UpdateState == State::CHECKING, "check button must request manifest");
    Menu(idle, &s_CheckButton); Menu(idle, &s_DownloadButton); Menu(idle, &s_InstallButton);
    idle.UpdateCheck(true);
    Check(idle.http.requests.size() == 1 && s_ProcessStarts == 0, "busy state must ignore repeated/out-of-order actions");
    CompleteCheck(idle);
    const auto requests = idle.http.requests.size();
    for(int i = 0; i != 3; ++i) { idle.client.now += 30000; idle.OnRender(); Menu(idle); }
    Check(idle.http.requests.size() == requests && idle.m_UpdateState == State::AVAILABLE && idle.client.quits == 0, "available must not auto-download/install");
    std::puts("PASS check-button/paired-signature/available-state/no-auto-download");

    Menu(idle, &s_DownloadButton);
    Check(idle.m_UpdateState == State::DOWNLOADING && idle.m_pUpdateDownloadTask && idle.m_pUpdateDownloadTask->hash && idle.m_pUpdateDownloadTask->resume, "download button must start verified resumable ZIP download");
    auto zipTask = idle.m_pUpdateDownloadTask;
    Menu(idle, &s_DownloadButton); Menu(idle, &s_CheckButton); Menu(idle, &s_InstallButton);
    Check(idle.m_pUpdateDownloadTask == zipTask && s_ProcessStarts == 0, "downloading ignores check/download/install clicks");
    idle.m_pUpdateDownloadTask->state = EHttpState::DONE;
    idle.OnRender();
    Check(idle.m_pUpdateHelperTask && idle.m_pUpdateHelperTask->hash && idle.m_pUpdateHelperTask->resume, "verified ZIP completion must fetch hashed helper within manual download");
    idle.m_pUpdateHelperTask->state = EHttpState::DONE;
    idle.OnRender();
    Check(idle.m_UpdateState == State::DOWNLOADED && idle.BootstrapHelperPending, "download completion must stop in downloaded state");
    for(int i = 0; i != 3; ++i) { idle.client.now += 30000; idle.OnRender(); Menu(idle); }
    Check(s_ProcessStarts == 0 && idle.client.quits == 0 && idle.m_UpdateState == State::DOWNLOADED, "downloaded must wait for install button");
    std::puts("PASS download-button/ZIP-hash/helper-hash/downloaded-waits");

    Menu(idle, &s_InstallButton);
    Check(s_ProcessStarts == 1 && idle.client.quits == 1 && idle.m_UpdateState == State::IDLE, "install button must launch helper and then quit");
    Check(s_LastArguments.size() == 14 && s_LastArguments[4] == "--sha256" && s_LastArguments[6] == "--version" && s_LastArguments[10] == "--restart", "manual helper must retain verification/restart arguments");
    std::puts("PASS explicit-install-button/helper-launch/quit");

    CTClient exiting; exiting.SeedVerifiedRelease(); exiting.m_UpdateState = State::DOWNLOADED;
    exiting.BootstrapHelperPending = true; str_copy(exiting.m_aUpdateBootstrapPath, "C:/fixture/update/TClientUpdater.boot.exe");
    const int starts = s_ProcessStarts;
    exiting.client.Quit(); exiting.OnShutdown();
    Check(s_ProcessStarts == starts && exiting.storage.removeCalls == 0 && exiting.storage.zipExists && exiting.storage.helperExists, "ordinary quit must not install or delete downloaded files");
    std::puts("PASS ordinary-quit/downloaded-files-preserved/no-helper-launch");

    CTClient retry; Menu(retry, &s_CheckButton);
    retry.m_pUpdateManifestTask->state = EHttpState::ERROR; retry.OnRender();
    Check(retry.m_UpdateState == State::CHECKING && retry.m_UpdateManifestSource == 1, "manual check retains source fallback");
    retry.m_pUpdateManifestTask->state = EHttpState::ERROR; retry.OnRender();
    Check(retry.m_UpdateState == State::FAILED && !retry.m_pUpdateManifestTask && !retry.m_UpdateRelease.m_Valid, "exhausted sources must write failed state and stop tasks");
    const size_t failedRequests = retry.http.requests.size(); retry.OnRender(); Menu(retry);
    Check(retry.http.requests.size() == failedRequests, "failed check must not restart itself");
    retry.m_UpdateRetryAt = 1; retry.m_UpdateDownloadAttempt = 7; retry.BootstrapHelperPending = true;
    str_copy(retry.m_aUpdateHelperUrl, "stale"); str_copy(retry.m_aUpdateBootstrapPath, "stale");
    Menu(retry, &s_CheckButton); CheckStaleCleared(retry); CompleteCheck(retry);
    std::puts("PASS manual-fallback/failed-state/fresh-button-recovery/stale-data-reset");

    CTClient aborted; Menu(aborted, &s_CheckButton);
    aborted.m_pUpdateManifestTask->state = EHttpState::ABORTED; aborted.OnRender();
    Check(aborted.m_UpdateState == State::FAILED && aborted.http.requests.size() == 1 && !aborted.m_pUpdateManifestTask, "cancel must not fall back or retry");
    CTClient::ConUpdateCheck(nullptr, &aborted); CheckStaleCleared(aborted); CompleteCheck(aborted);
    std::puts("PASS cancellation/no-auto-retry/explicit-console-check");

    CTClient current; Menu(current, &s_CheckButton);
    current.ResetUpdateTasks(); current.m_UpdateFoundCurrentRelease = true;
    current.m_UpdateManifestSource = int(std::size(TCLIENT_UPDATE_MANIFEST_SOURCES));
    current.FetchUpdateManifest();
    Check(current.m_UpdateState == State::UP_TO_DATE && std::string(current.m_aUpdateStatus) == "You are on the latest version", "no newer release must expose up-to-date state/status");
    const size_t currentRequests = current.http.requests.size(); current.OnRender(); Menu(current);
    Check(current.http.requests.size() == currentRequests, "up-to-date state must never recheck automatically");
    Menu(current, &s_CheckButton); CheckStaleCleared(current); CompleteCheck(current);
    std::puts("PASS up-to-date-status/no-auto-recheck/manual-recheck");

    CTClient downloadRetry; downloadRetry.SeedVerifiedRelease(); Menu(downloadRetry, &s_DownloadButton);
    downloadRetry.m_pUpdateDownloadTask->state = EHttpState::ERROR; downloadRetry.OnRender();
    Check(downloadRetry.m_UpdateState == State::DOWNLOADING && downloadRetry.m_UpdateRetryAt == 2.f, "user-started download retains delayed mirror retry");
    downloadRetry.client.now = 1; downloadRetry.OnRender(); Check(!downloadRetry.m_pUpdateDownloadTask, "download retry must wait");
    downloadRetry.client.now = 2; downloadRetry.OnRender(); Check(downloadRetry.m_pUpdateDownloadTask != nullptr, "manual download retry must resume");
    downloadRetry.m_pUpdateDownloadTask->state = EHttpState::ERROR; downloadRetry.OnRender();
    Check(downloadRetry.m_UpdateState == State::FAILED && !downloadRetry.m_pUpdateDownloadTask && downloadRetry.m_UpdateRetryAt < 0, "exhausted downloads stop and expose failure");
    Menu(downloadRetry, &s_CheckButton); CheckStaleCleared(downloadRetry); CompleteCheck(downloadRetry);
    std::puts("PASS manual-download-retry/exhaustion/recheck-recovery");

    CTClient sizes; sizes.SeedVerifiedRelease(); Menu(sizes, &s_DownloadButton);
    s_ZipLength = 99; sizes.m_pUpdateDownloadTask->state = EHttpState::DONE; sizes.OnRender();
    Check(!sizes.m_pUpdateHelperTask && sizes.m_UpdateRetryAt >= 0, "wrong ZIP size must not fetch helper"); s_ZipLength = 100;
    sizes.StartUpdateHelperTask(); s_HelperLength = 19; sizes.m_pUpdateHelperTask->state = EHttpState::DONE; sizes.OnRender();
    Check(sizes.m_UpdateState != State::DOWNLOADED && !sizes.BootstrapHelperPending, "wrong helper size must not enable install"); s_HelperLength = 20;
    std::puts("PASS ZIP-size/helper-size-rejection");

    CTClient failedInstall; failedInstall.SeedVerifiedRelease(); failedInstall.m_UpdateState = State::DOWNLOADED;
    s_ProcessFails = true; Menu(failedInstall, &s_InstallButton); s_ProcessFails = false;
    Check(failedInstall.m_UpdateState == State::FAILED && failedInstall.client.quits == 0, "helper-start failure must write failed state without quitting");
    Menu(failedInstall, &s_CheckButton); CheckStaleCleared(failedInstall); CompleteCheck(failedInstall);
    std::puts("PASS helper-launch-failure/no-quit/manual-recovery");
    return 0;
} catch(const std::exception &e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; } }
`;

function compileAndRun() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tclient-manual-update-'));
    try {
        const cpp = path.join(dir, 'fixture.cpp'), exe = path.join(dir, process.platform === 'win32' ? 'fixture.exe' : 'fixture');
        fs.writeFileSync(cpp, fixture);
        if (process.platform === 'win32') {
            const vswhere = path.join(process.env['ProgramFiles(x86)'] || 'C:/Program Files (x86)', 'Microsoft Visual Studio/Installer/vswhere.exe');
            const vs = cp.execFileSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], {encoding:'utf8'}).trim();
            assert(vs, 'MSVC Build Tools not found');
            const vcvars = path.join(vs, 'VC/Auxiliary/Build/vcvars64.bat');
            cp.execSync(`call "${vcvars}" >nul && cl /nologo /std:c++17 /EHsc /W4 /utf-8 fixture.cpp /Fe:fixture.exe`, {cwd:dir, encoding:'utf8', shell:process.env.ComSpec || 'cmd.exe', stdio:'pipe'});
        } else {
            cp.execFileSync(process.env.CXX || 'c++', ['-std=c++17', '-Wall', '-Wextra', cpp, '-o', exe], {cwd:dir, encoding:'utf8', stdio:'pipe'});
        }
        process.stdout.write(cp.execFileSync(exe, [], {cwd:dir, encoding:'utf8', stdio:'pipe'}));
    } finally { fs.rmSync(dir, {recursive:true, force:true}); }
}

function staticGates() {
    for (const signature of ['void CTClient::OnInit(', 'void CTClient::OnRender(', 'void CTClient::OnUpdate(', 'void CTClient::OnStateChange(', 'void CTClient::OnShutdown(']) {
        const fn = body(source, signature);
        assert(!/\b(?:UpdateCheck|FetchUpdateManifest|FetchTClientInfo|UpdateStartDownload|StartDownloadTask|UpdateInstallAndRestart|process_execute)\s*\(/.test(fn), `${signature} has an implicit update entry point`);
    }
    assert(!/UpdateShouldAutoCheck|TCLIENT_UPDATE_FIRST_CHECK_DELAY|TCLIENT_UPDATE_CHECK_INTERVAL|m_LastUpdateCheck/.test(source + header), 'automatic scheduler remains');
    const manifest = body(source, 'void CTClient::FinishUpdateManifest(');
    assert(manifest.indexOf('VerifyUpdateManifestSignature(') >= 0 && manifest.indexOf('VerifyUpdateManifestSignature(') < manifest.indexOf('ResultJson('), 'manifest must be verified before parsing');
    assert(!/UpdateStartDownload\(|StartDownloadTask\(|UpdateInstallAndRestart\(/.test(manifest), 'available result must not download/install');
    assert(manifest.includes('EUpdateState::AVAILABLE') && manifest.includes('m_UpdateRelease.m_Valid = true'), 'verified result must write available release state');
    const verify = body(source, 'bool CTClient::VerifyUpdateManifestSignature(');
    assert(verify.includes('ed25519_verify(') && verify.includes('SignatureSize != 64'), 'Ed25519 gate must remain');
    const poll = body(source, 'void CTClient::PollUpdateTasks(');
    assert(!poll.includes('UpdateInstallAndRestart(') && !poll.includes('process_execute('), 'completion polling must never install');
    for (const signature of ['void CClient::Quit(', 'void CClient::Restart(', 'void CClient::Run(', 'void CClient::OnPostConnect(']) {
        const fn = body(engine, signature);
        assert(!/\b(?:UpdateCheck|UpdateStartDownload|UpdateInstallAndRestart|FetchUpdateManifest)\s*\(/.test(fn), `${signature} triggers TClient update`);
    }
    const init = body(engine, 'void CClient::Run(');
    assert(init.includes('RequestDDNetInfo();') && init.includes('m_ServerBrowser.OnInit();'), 'ordinary DDNet startup must remain');
    for (const file of ['src/game/client/components/menus.cpp', 'src/game/client/components/menus_start.cpp']) {
        assert(!/UpdateInstallAndRestart\(|EUpdateState::DOWNLOADED/.test(read(file)), `${file} has exit/downloaded update popup`);
    }
    const updater = read('src/engine/client/updater.cpp');
    assert(body(updater, 'void CUpdater::InitiateUpdate(').includes('FetchFile("update.json")'), 'legacy manually invoked updater must remain');
    const callers = [];
    function walk(dir) {
        for (const entry of fs.readdirSync(dir, {withFileTypes:true})) {
            const file = path.join(dir, entry.name);
            if (entry.isDirectory()) walk(file);
            else if (/\.(cpp|h)$/.test(entry.name)) {
                const text = fs.readFileSync(file, 'utf8').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
                for (const line of text.split(/\r?\n/)) if (/\bUpdateCheck\s*\(/.test(line) && !/void\s+(?:CTClient::)?UpdateCheck\s*\(/.test(line)) {
                    assert(/UpdateCheck\(true\)/.test(line), `non-explicit UpdateCheck caller in ${file}`);
                    callers.push(path.relative(root, file));
                }
            }
        }
    }
    walk(path.join(root, 'src'));
    assert(callers.length === 2, `expected only UI and explicit console callers, found ${callers.length}`);
    console.log('PASS static lifecycle/callers/Ed25519-before-JSON/DDNet-preservation/no-exit-popup');
    const labels = [...buttonFlow[0].matchAll(/TCLocalize\("([^"]+)"\)/g)].map(m => m[1]);
    assert.deepStrictEqual(labels, ['Get update', 'Download', 'Install and restart'], 'manual update labels must remain explicit');
    for (const [file, translation] of [['simplified_chinese.txt', '获取更新'], ['traditional_chinese.txt', '取得更新']]) {
        const lines = read(`data/tclient/languages/${file}`).split(/\r?\n/);
        assert.strictEqual(lines.filter(line => line === 'Get update').length, 1, `duplicate/missing Get update key in ${file}`);
        const key = lines.indexOf('Get update');
        assert.strictEqual(lines[key + 1], `== ${translation}`, `Get update translation in ${file}`);
    }
    console.log(`PASS real UI labels/translations: ${labels.join(' -> ')}`);
}
try {
    compileAndRun();
    staticGates();
    console.log(`PASS updater regression: ${root}`);
} catch (error) {
    console.error(error.stdout ? String(error.stdout).trim() : '');
    console.error(error.stderr ? String(error.stderr).trim() : error.message);
    process.exitCode = 1;
}
