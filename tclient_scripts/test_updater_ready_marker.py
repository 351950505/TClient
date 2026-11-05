#!/usr/bin/env python3
"""Compile the production Windows ready-marker functions and exercise their protocol.

Only a unique TEMP sandbox is used for compilation and marker fixtures. The child
inherits the real environment; handshake keys are changed only inside the wrapper.
This does not run CClient::Run, render a frame, or establish full-client health.
"""

import argparse
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import traceback


SOURCE = Path("src/engine/client/client.cpp")
HANDSHAKE_KEYS = ("TCLIENT_UPDATER_READY_FILE", "TCLIENT_UPDATER_READY_TOKEN")
DEFINITIONS = (
    ("SUpdaterReadyMarker", rb"struct SUpdaterReadyMarker", rb"\};"),
    ("GetWideEnvironmentVariable",
     rb"static std::wstring GetWideEnvironmentVariable\(const wchar_t \*pName\)", rb"\}"),
    ("TakeUpdaterReadyMarker",
     rb"static SUpdaterReadyMarker TakeUpdaterReadyMarker\(\)", rb"\}"),
    ("WriteUpdaterReadyMarker",
     rb"static bool WriteUpdaterReadyMarker\(const SUpdaterReadyMarker &Marker\)", rb"\}"),
)
HEADERS = rb'''#define NOMINMAX
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static unsigned g_Errors = 0;
static unsigned g_Infos = 0;

static void log_error(const char *pSystem, const char *pFormat, ...)
{
    ++g_Errors;
    va_list Arguments;
    va_start(Arguments, pFormat);
    std::fprintf(stderr, "error[%s]: ", pSystem);
    std::vfprintf(stderr, pFormat, Arguments);
    std::fprintf(stderr, "\n");
    va_end(Arguments);
}

static void log_info(const char *pSystem, const char *pFormat, ...)
{
    ++g_Infos;
    va_list Arguments;
    va_start(Arguments, pFormat);
    std::fprintf(stderr, "info[%s]: ", pSystem);
    std::vfprintf(stderr, pFormat, Arguments);
    std::fprintf(stderr, "\n");
    va_end(Arguments);
}

'''
ENTRY = r'''
static void Require(bool Condition, const char *pMessage)
{
    if(!Condition)
    {
        std::fprintf(stderr, "ASSERTION: %s (last error %lu)\n", pMessage, (unsigned long)GetLastError());
        std::exit(1);
    }
}

static bool IsUnset(const wchar_t *pName)
{
    SetLastError(ERROR_SUCCESS);
    return GetEnvironmentVariableW(pName, nullptr, 0) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
}

static void SetKey(const wchar_t *pName, const wchar_t *pValue)
{
    Require(SetEnvironmentVariableW(pName, pValue) != FALSE, "could not prepare child environment");
}

static void RequireSandboxPath(const std::wstring &Path)
{
    // Validate both ordinary and extended paths against this invocation's TEMP root.
    std::wstring Normal = Path;
    if(Normal.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        Normal = L"\\\\" + Normal.substr(8);
    else if(Normal.compare(0, 4, L"\\\\?\\") == 0)
        Normal.erase(0, 4);
    const DWORD Required = GetFullPathNameW(Normal.c_str(), 0, nullptr, nullptr);
    Require(Required != 0, "could not resolve fixture path");
    std::vector<wchar_t> Buffer(Required);
    const DWORD Written = GetFullPathNameW(Normal.c_str(), Required, Buffer.data(), nullptr);
    Require(Written != 0 && Written < Required, "could not resolve complete fixture path");
    const std::wstring Full(Buffer.data(), Written);
    const std::wstring Root = LR"ROOT(__SANDBOX__)ROOT";
    Require(Full.size() > Root.size() &&
        CompareStringOrdinal(Full.c_str(), (int)Root.size(), Root.c_str(), (int)Root.size(), TRUE) == CSTR_EQUAL,
        "fixture path is outside this TEMP sandbox");
}

static std::string ReadExclusive(const std::wstring &Path)
{
    // Share mode zero, before wrapper exit, detects a production handle left open.
    HANDLE hFile = CreateFileW(Path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(hFile != INVALID_HANDLE_VALUE, "marker cannot be reopened exclusively before process exit");
    LARGE_INTEGER Size;
    Require(GetFileSizeEx(hFile, &Size) && Size.QuadPart >= 0 && Size.QuadPart <= 1024,
        "unexpected marker size");
    std::string Contents((size_t)Size.QuadPart, '\0');
    DWORD Read = 0;
    Require(ReadFile(hFile, Contents.data(), (DWORD)Contents.size(), &Read, nullptr) && Read == Contents.size(),
        "could not read complete marker");
    Require(CloseHandle(hFile) != FALSE, "could not close verification handle");
    return Contents;
}

static std::string ExpectedContents(const std::wstring &Token)
{
    const int Required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Token.c_str(), (int)Token.size(), nullptr, 0, nullptr, nullptr);
    Require(Required > 0, "could not encode expected test token");
    std::string Bytes((size_t)Required, '\0');
    Require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Token.c_str(), (int)Token.size(), Bytes.data(), Required, nullptr, nullptr) == Required,
        "could not encode complete expected token");
    return Bytes + '\n';
}

int wmain(int argc, wchar_t **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    Require(argc == 4, "expected mode, TEMP marker path and token");
    const std::wstring Mode = argv[1];
    const std::wstring Path = argv[2];
    const std::wstring Token = argv[3];
    RequireSandboxPath(Path);
    const bool Incomplete = Mode == L"path-only" || Mode == L"token-only";
    const bool Complete = Mode == L"success" || Mode == L"existing" || Mode == L"unwritable" || Mode == L"missing-parent";
    Require(Mode == L"absent" || Incomplete || Complete, "unknown test mode");

    // Only this wrapper's environment is changed; its parent and siblings inherit normally.
    SetKey(L"TCLIENT_UPDATER_READY_FILE", nullptr);
    SetKey(L"TCLIENT_UPDATER_READY_TOKEN", nullptr);
    if(Complete || Mode == L"path-only")
        SetKey(L"TCLIENT_UPDATER_READY_FILE", Path.c_str());
    if(Complete || Mode == L"token-only")
        SetKey(L"TCLIENT_UPDATER_READY_TOKEN", Token.c_str());

    const std::string Before = Mode == L"existing" ? ReadExclusive(Path) : std::string();
    const SUpdaterReadyMarker Marker = TakeUpdaterReadyMarker();
    Require(IsUnset(L"TCLIENT_UPDATER_READY_FILE"), "ready-file environment key was not removed");
    Require(IsUnset(L"TCLIENT_UPDATER_READY_TOKEN"), "ready-token environment key was not removed");
    Require(Marker.m_Path == (Complete ? Path : std::wstring()), "Take returned an unexpected path");
    Require(Marker.m_Token == (Complete ? Token : std::wstring()), "Take returned an unexpected token");
    const SUpdaterReadyMarker Second = TakeUpdaterReadyMarker();
    Require(Second.m_Path.empty() && Second.m_Token.empty(), "handshake was not consumed exactly once");

    // Match the production caller's pending guard without invoking CClient::Run.
    const bool WriteCalled = !Marker.m_Path.empty();
    const bool Written = WriteCalled && WriteUpdaterReadyMarker(Marker);
    Require(WriteCalled == Complete, "write was called for an absent or incomplete handshake");
    Require(Written == (Mode == L"success"), "unexpected production write result");
    bool ExclusiveRead = false;
    size_t Bytes = 0;
    if(Mode == L"success" || Mode == L"existing")
    {
        const std::string Contents = ReadExclusive(Path);
        ExclusiveRead = true;
        Bytes = Contents.size();
        Require(Contents == (Mode == L"success" ? ExpectedContents(Token) : Before),
            "marker contents were incomplete or an existing marker was overwritten");
    }
    else
    {
        Require(GetFileAttributesW(Path.c_str()) == INVALID_FILE_ATTRIBUTES, "unexpected marker was created");
        const DWORD Error = GetLastError();
        Require(Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND,
            "marker absence could not be confirmed");
    }
    Require(IsUnset(L"TCLIENT_UPDATER_READY_FILE") && IsUnset(L"TCLIENT_UPDATER_READY_TOKEN"),
        "write restored handshake environment keys");
    Require(g_Infos == (Written ? 1u : 0u), "unexpected success log count");
    Require(g_Errors == ((Incomplete || (Complete && !Written)) ? 1u : 0u), "unexpected failure log count");
    std::printf("{\"environment_keys_unset\":true,\"taken_values_match\":true,\"second_take_empty\":true,"
                "\"write_called\":%s,\"write_returned\":%s,\"exclusive_reopen_before_exit\":%s,"
                "\"bytes_read\":%llu,\"error_logs\":%u,\"info_logs\":%u}\n",
        WriteCalled ? "true" : "false", Written ? "true" : "false", ExclusiveRead ? "true" : "false",
        (unsigned long long)Bytes, g_Errors, g_Infos);
    return 0;
}
'''


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def extract(source):
    result = {}
    for name, signature, ending in DEFINITIONS:
        pattern = re.compile(rb"^" + signature + rb"\r?\n\{.*?^" + ending,
                             re.MULTILINE | re.DOTALL)
        matches = list(pattern.finditer(source))
        if len(matches) != 1:
            raise RuntimeError("Expected exactly one production definition of " + name)
        match = matches[0]
        result[name] = (match.group(), source[:match.start()].count(b"\n") + 1)
    return result


def extended_path(path):
    value = str(path)
    if value.startswith("\\\\?\\"):
        return value
    if value.startswith("\\\\"):
        return "\\\\?\\UNC\\" + value[2:]
    return "\\\\?\\" + value


def command(arguments, cwd, timeout=30):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    # No env argument: do not redirect APPDATA, TEMP, HOME, or any other setting.
    return subprocess.run(
        [str(arg) for arg in arguments], cwd=cwd, capture_output=True,
        stdin=subprocess.DEVNULL, timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW, startupinfo=startup,
    )


def create_unwritable_directory(path):
    """Deny adding files only on a newly created TEMP directory, leaving it deletable."""
    class SecurityAttributes(ctypes.Structure):
        _fields_ = [("nLength", wintypes.DWORD), ("lpSecurityDescriptor", wintypes.LPVOID),
                    ("bInheritHandle", wintypes.BOOL)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = (wintypes.LPCWSTR, wintypes.DWORD,
                        ctypes.POINTER(wintypes.LPVOID), ctypes.POINTER(wintypes.DWORD))
    convert.restype = wintypes.BOOL
    create = kernel.CreateDirectoryW
    create.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(SecurityAttributes))
    create.restype = wintypes.BOOL
    free = kernel.LocalFree
    free.argtypes = (wintypes.HLOCAL,)
    free.restype = wintypes.HLOCAL
    # FILE_ADD_FILE is 0x2 for a directory. No registry or existing ACL is touched.
    sddl = "D:P(D;;0x00000002;;;WD)(A;;FA;;;WD)"
    descriptor = wintypes.LPVOID()
    if not convert(sddl, 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        attributes = SecurityAttributes(ctypes.sizeof(SecurityAttributes), descriptor, False)
        if not create(str(path), ctypes.byref(attributes)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        free(descriptor)
    return sddl


def run_cases(sandbox, executable, evidence, report):
    fixtures = sandbox / "cases"
    fixtures.mkdir()
    unicode_dir = fixtures / "spaces 中文目录"
    unicode_dir.mkdir()
    long_dir = unicode_dir
    while len(str(long_dir / "ready marker.tmp")) <= 300:
        long_dir /= "long segment 中文 " + "x" * 32
    Path(extended_path(long_dir)).mkdir(parents=True)
    unwritable = fixtures / "unwritable"
    report["unwritable_directory_sddl"] = create_unwritable_directory(unwritable)
    token = "ready-token 中文 with spaces 0123456789"
    existing = fixtures / "existing marker.tmp"
    original = b"existing marker must not change\r\n\x00tail"
    existing.write_bytes(original)
    cases = (
        ("absent_environment", "absent", fixtures / "absent.tmp", False),
        ("unicode_spaces", "success", unicode_dir / "ready marker.tmp", False),
        ("extended_long_path", "success", long_dir / "ready marker.tmp", True),
        ("path_only_environment", "path-only", fixtures / "path-only.tmp", False),
        ("token_only_environment", "token-only", fixtures / "token-only.tmp", False),
        ("existing_marker", "existing", existing, False),
        ("unwritable_parent", "unwritable", unwritable / "ready.tmp", False),
        ("missing_parent", "missing-parent", fixtures / "missing" / "ready.tmp", False),
    )
    for name, mode, path, use_extended in cases:
        path.relative_to(sandbox)
        argument = extended_path(path) if use_extended else str(path)
        case = {"name": name, "mode": mode, "path": argument,
                "path_length_without_prefix": len(str(path)), "extended_path": use_extended,
                "passed": False}
        report["cases"].append(case)
        try:
            if use_extended and len(str(path)) <= 260:
                raise AssertionError("Long-path fixture does not exceed MAX_PATH")
            case["command"] = [str(executable), mode, argument, token]
            result = command(case["command"], sandbox)
            (evidence / (name + ".stdout.log")).write_bytes(result.stdout)
            (evidence / (name + ".stderr.log")).write_bytes(result.stderr)
            case["exit_code"] = result.returncode
            if result.returncode:
                raise AssertionError("Wrapper failed; see case stderr log")
            case["wrapper_checks"] = json.loads(result.stdout)
            if mode in ("success", "existing"):
                actual = Path(argument).read_bytes()
                expected = (token + "\n").encode("utf-8") if mode == "success" else original
                case["contents_hex"] = actual.hex()
                case["contents_match"] = actual == expected
                if actual != expected:
                    raise AssertionError("Independent Python byte comparison failed")
            elif Path(argument).exists():
                raise AssertionError("Unexpected marker exists after wrapper exit")
            case["passed"] = True
        except Exception:
            case["error"] = traceback.format_exc()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--vcvars", type=Path,
                        default=Path("C:/VS2026/BuildTools/VC/Auxiliary/Build/vcvars64.bat"))
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("This extraction test requires Windows and MSVC")
    repo = args.repo.resolve()
    args.evidence.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-")
    evidence = Path(tempfile.mkdtemp(prefix=stamp, dir=args.evidence.resolve()))
    sandbox = Path(tempfile.mkdtemp(prefix="tclient-ready-marker-"))
    build = sandbox / "build"
    build.mkdir()
    parent_keys = tuple(os.environ.get(key) for key in HANDSHAKE_KEYS)
    report = {
        "source": str(repo / SOURCE), "evidence": str(evidence), "sandbox": str(sandbox),
        "python": sys.version, "invocation": [sys.executable, "-I", *sys.argv],
        "environment_inherited": True, "parent_environment_overrides": [],
        "handshake_keys_changed_only_in_wrapper": True, "hidden_no_gui": True,
        "cases": [], "passed": False, "extractions": {},
        "limitations": [
            "Only the four extracted marker definitions are compiled; logging is stubbed, Windows APIs are real.",
            "No CClient::Run, renderer, first-frame runtime, or full-client startup-health test is performed.",
            "The production success log mentions the first frame; that log is not evidence of rendering here.",
            "Long-path coverage uses an explicit extended prefix; UNC paths and unprefixed long paths are not covered.",
            "Successful Write includes the real FlushFileBuffers call; crash/power-loss durability is not tested.",
        ],
    }
    started = time.monotonic()
    definitions = None
    try:
        if not args.vcvars.is_file():
            raise FileNotFoundError(args.vcvars)
        source = (repo / SOURCE).read_bytes()
        report["source_sha256_before"] = sha256(source)
        definitions = extract(source)
        production = b"\n\n".join(item[0] for item in definitions.values())
        wrapper = HEADERS + production + b"\n" + ENTRY.replace(
            "__SANDBOX__", str(sandbox) + "\\").encode("utf-8")
        wrapped_definitions = extract(wrapper)
        for name, (definition, line) in definitions.items():
            identical = wrapped_definitions[name][0] == definition
            if not identical:
                raise AssertionError("Wrapper changed production definition " + name)
            report["extractions"][name] = {
                "source_line": line, "bytes": len(definition), "sha256": sha256(definition),
                "unchanged_in_wrapper": identical,
            }
        report["wrapper_sha256"] = sha256(wrapper)
        (build / "marker.cpp").write_bytes(wrapper)
        (evidence / "production-marker.cpp").write_bytes(production)
        (evidence / "marker-wrapper.cpp").write_bytes(wrapper)
        compile_command = (f'cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /MD '
                           f'"{build / "marker.cpp"}" /Fo"{build / "marker.obj"}" '
                           f'/Fe"{build / "marker.exe"}" /link /SUBSYSTEM:CONSOLE')
        batch = "\r\n".join([
            "@echo off", 'call "' + str(args.vcvars) + '"', "if errorlevel 1 exit /b 1",
            "where cl", compile_command, "if errorlevel 1 exit /b 1", "exit /b 0", "",
        ]).encode("utf-8")
        batch_path = build / "compile.cmd"
        batch_path.write_bytes(batch)
        (evidence / "compile.cmd").write_bytes(batch)
        report["compile_command"] = compile_command
        report["build_command"] = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(batch_path)]
        compiled = command(report["build_command"], build, timeout=120)
        (evidence / "build.stdout.log").write_bytes(compiled.stdout)
        (evidence / "build.stderr.log").write_bytes(compiled.stderr)
        report["build_exit_code"] = compiled.returncode
        if compiled.returncode:
            raise RuntimeError("MSVC compilation failed; see build logs")
        report["binary_sha256"] = sha256((build / "marker.exe").read_bytes())
        run_cases(sandbox, build / "marker.exe", evidence, report)
        report["passed"] = len(report["cases"]) == 8 and all(case["passed"] for case in report["cases"])
    except Exception:
        report["error"] = traceback.format_exc()
    finally:
        try:
            if definitions is not None:
                current = (repo / SOURCE).read_bytes()
                current_definitions = extract(current)
                report["source_sha256_after"] = sha256(current)
                report["production_definitions_unchanged_during_test"] = all(
                    current_definitions[name][0] == value[0] for name, value in definitions.items())
                if not report["production_definitions_unchanged_during_test"]:
                    report["passed"] = False
            report["parent_handshake_environment_unchanged"] = parent_keys == tuple(
                os.environ.get(key) for key in HANDSHAKE_KEYS)
            if not report["parent_handshake_environment_unchanged"]:
                report["passed"] = False
        except Exception:
            report["verification_error"] = traceback.format_exc()
            report["passed"] = False
        try:
            shutil.rmtree(extended_path(sandbox))
            report["sandbox_removed"] = not sandbox.exists()
        except Exception:
            report["sandbox_removed"] = False
            report["sandbox_cleanup_error"] = traceback.format_exc()
            report["passed"] = False
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        result_path = evidence / "results.json"
        result_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(("PASS" if report["passed"] else "FAIL") + ": " + str(result_path))
    for case in report["cases"]:
        print(case["name"] + ": " + ("PASS" if case["passed"] else "FAIL"))
    print("sandbox_removed=" + str(report["sandbox_removed"]))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
