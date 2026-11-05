#!/usr/bin/env python3
"""Compile and test the unchanged StopOwnHelperProcesses function on Windows."""

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


FUNCTION = re.compile(
    rb"^static void StopOwnHelperProcesses\(const wchar_t \*pInstallDir\)\r?\n\{.*?^\}",
    re.MULTILINE | re.DOTALL,
)
SOURCE = Path("src/game/client/components/tclient/tclient.cpp")
HELPERS = ("island_media_monitor.exe", "netease-watcher.exe")
HEADERS = b"""#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iterator>
#include <cwctype>
#include <cwchar>

"""
FIXTURE = b"""#include <windows.h>
int wmain()
{
    Sleep(120000);
    return 42;
}
"""


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def extract(source):
    matches = list(FUNCTION.finditer(source))
    if len(matches) != 1:
        raise RuntimeError("Expected exactly one StopOwnHelperProcesses definition")
    match = matches[0]
    return match.group(), source[:match.start()].count(b"\n") + 1


def hidden_options():
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    # Deliberately omit env: inherit the user's complete, real environment.
    return {"creationflags": subprocess.CREATE_NO_WINDOW, "startupinfo": startup}


def command(arguments, cwd, timeout=60):
    return subprocess.run(
        [str(arg) for arg in arguments], cwd=cwd, capture_output=True,
        timeout=timeout, **hidden_options(),
    )


def make_wrapper(function, sandbox):
    # The extra entry-point guard confines even a mistaken invocation to this
    # unique TEMP sandbox. The production function itself remains byte-for-byte.
    root = str(sandbox) + "\\"
    entry = r'''
int wmain(int argc, wchar_t **argv)
{
    if(argc != 2)
        return 64;
    wchar_t Full[32768];
    const DWORD Length = GetFullPathNameW(argv[1], static_cast<DWORD>(std::size(Full)), Full, nullptr);
    const std::wstring Root = LR"ROOT(__SANDBOX__)ROOT";
    if(Length == 0 || Length >= std::size(Full) || Length < Root.size() ||
        CompareStringOrdinal(Full, static_cast<int>(Root.size()), Root.c_str(),
            static_cast<int>(Root.size()), TRUE) != CSTR_EQUAL)
        return 65;
    StopOwnHelperProcesses(argv[1]);
    return 0;
}
'''.replace("__SANDBOX__", root)
    wrapper = HEADERS + function + b"\n" + entry.encode("utf-8")
    assert extract(wrapper)[0] == function
    return wrapper


def image_path(process):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    query = kernel.QueryFullProcessImageNameW
    query.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                      ctypes.POINTER(wintypes.DWORD))
    query.restype = wintypes.BOOL
    buffer = ctypes.create_unicode_buffer(32768)
    size = wintypes.DWORD(len(buffer))
    if not query(wintypes.HANDLE(int(process._handle)), 0, buffer, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer.value


def launch(executable, arguments, sandbox):
    executable.resolve().relative_to(sandbox.resolve())
    return subprocess.Popen(
        [str(executable), *arguments], cwd=sandbox,
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL, **hidden_options(),
    )


def stop_tracked(process, evidence):
    try:
        if process.poll() is None:
            process.terminate()  # Popen uses its retained process handle.
        process.wait(timeout=5)
        evidence["exit_code_after_finally"] = process.returncode
    finally:
        process._handle.Close()


def run_case(sandbox, fixture, wrapper, number, name, path_style, baseline, report):
    case_root = sandbox / "spaces 中文" / ("case " + str(number))
    target = case_root / "target"
    locations = {
        "target": target,
        "target-old": case_root / "target-old",
        "nested": target / "nested 子目录",
        "another": case_root / "another 其他安装",
    }
    case = {"name": name, "variant": "HEAD" if baseline else "current",
            "processes": [], "cleanup_errors": []}
    report["cases"].append(case)
    tracked = []
    runner = None
    try:
        for role, directory in locations.items():
            directory.mkdir(parents=True)
            for helper in HELPERS:
                executable = directory / helper
                shutil.copy2(fixture, executable)
                row = {"role": role, "executable": str(executable)}
                case["processes"].append(row)
                process = launch(executable, [], sandbox)
                tracked.append((process, row))
                row["pid"] = process.pid
                row["queried_image_path"] = image_path(process)
                if os.path.normcase(row["queried_image_path"]) != os.path.normcase(str(executable)):
                    raise AssertionError("Fixture image does not match its tracked TEMP copy")
        # Popen completion alone means image creation succeeded; also establish
        # that every control is alive before invoking the production cleanup.
        for process, row in tracked:
            row["alive_before"] = process.poll() is None
            if not row["alive_before"]:
                raise AssertionError("Fixture exited before cleanup: " + row["executable"])
        argument = str(target)
        if "forward" in path_style:
            argument = argument.replace("\\", "/")
        if "uppercase" in path_style:
            argument = argument.upper()
        if "trailing" in path_style:
            argument += "/" if "forward" in path_style else "\\"
        case["install_argument"] = argument
        case["command"] = [str(wrapper), argument]
        runner = launch(wrapper, [argument], sandbox)
        case["wrapper_pid"] = runner.pid
        case["wrapper_exit_code"] = runner.wait(timeout=30)
        if case["wrapper_exit_code"] != 0:
            raise AssertionError("Wrapper failed: " + str(case["wrapper_exit_code"]))
        for process, row in tracked:
            row["exit_code_after_cleanup"] = process.poll()
            row["alive_after_cleanup"] = row["exit_code_after_cleanup"] is None
            expected_dead = row["role"] == "target" or (baseline and row["role"] in ("target-old", "nested"))
            row["expected_alive"] = not expected_dead
            row["passed"] = row["alive_after_cleanup"] == row["expected_alive"]
            if expected_dead:
                row["passed"] = row["passed"] and row["exit_code_after_cleanup"] == 0
        case["passed"] = all(row["passed"] for row in case["processes"])
        if not case["passed"]:
            raise AssertionError("Unexpected helper cleanup result in " + name)
    finally:
        if runner is not None:
            try:
                stop_tracked(runner, case)
            except Exception as error:
                case["cleanup_errors"].append("wrapper: " + repr(error))
        for process, row in tracked:
            try:
                stop_tracked(process, row)
            except Exception as error:
                case["cleanup_errors"].append(str(process.pid) + ": " + repr(error))
        if case["cleanup_errors"]:
            raise RuntimeError("Tracked process cleanup failed: " + repr(case["cleanup_errors"]))


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
    sandbox = Path(tempfile.mkdtemp(prefix="tclient-parent-cleanup-"))
    build = sandbox / "build"
    build.mkdir()
    report = {
        "source": str(repo / SOURCE), "evidence": str(evidence),
        "sandbox": str(sandbox), "python": sys.version,
        "invocation": [sys.executable, "-I", *sys.argv],
        "environment_inherited": True, "environment_overrides": [],
        "hidden_no_gui": True, "tracked_process_handles_only": True,
        "fixture_fallback_exit_ms": 120000, "cases": [], "passed": False,
        "extractions": {}, "limitations": [
            "Only the extracted cleanup function is compiled; no full client or updater build.",
            "Fixtures share the current user's token; cross-user/access-denied cases are not covered.",
            "Canonical local paths under MAX_PATH are tested; junctions, UNC and long paths are not covered.",
            "No null pointer input is tested; the production caller supplies a directory string.",
        ],
    }
    started = time.monotonic()
    current_function = None
    try:
        if not args.vcvars.is_file():
            raise FileNotFoundError(args.vcvars)
        current_bytes = (repo / SOURCE).read_bytes()
        current_function, current_line = extract(current_bytes)
        head = command(["git", "-C", repo, "rev-parse", "HEAD"], repo)
        baseline = command(["git", "-C", repo, "show", "HEAD:" + SOURCE.as_posix()], repo)
        if head.returncode or baseline.returncode:
            raise RuntimeError("Cannot read HEAD baseline from git")
        report["head_commit"] = head.stdout.decode("ascii").strip()
        baseline_function, baseline_line = extract(baseline.stdout)
        for name, function, line in (("current", current_function, current_line),
                                     ("head", baseline_function, baseline_line)):
            wrapper = make_wrapper(function, sandbox)
            (build / (name + ".cpp")).write_bytes(wrapper)
            (evidence / (name + "-function.cpp")).write_bytes(function)
            (evidence / (name + "-wrapper.cpp")).write_bytes(wrapper)
            report["extractions"][name] = {
                "line": line, "bytes": len(function), "sha256": sha256(function),
                "wrapper_sha256": sha256(wrapper), "unchanged_in_wrapper": True,
            }
        (build / "fixture.cpp").write_bytes(FIXTURE)
        (evidence / "fixture.cpp").write_bytes(FIXTURE)
        script = ['@echo off', 'call "' + str(args.vcvars) + '"',
                  'if errorlevel 1 exit /b 1', 'where cl']
        compile_commands = []
        for name in ("current", "head", "fixture"):
            cl = (f'cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /MD '
                  f'"{build / (name + ".cpp")}" /Fo"{build / (name + ".obj")}" '
                  f'/Fe"{build / (name + ".exe")}" /link /SUBSYSTEM:CONSOLE')
            compile_commands.append(cl)
            script.extend([cl, 'if errorlevel 1 exit /b 1'])
        script.append('exit /b 0')
        batch = "\r\n".join(script).encode("utf-8") + b"\r\n"
        batch_path = build / "compile.cmd"
        batch_path.write_bytes(batch)
        (evidence / "compile.cmd").write_bytes(batch)
        report["compile_commands"] = compile_commands
        report["build_command"] = [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(batch_path)]
        compiled = command(report["build_command"], build, timeout=120)
        (evidence / "build.stdout.log").write_bytes(compiled.stdout)
        (evidence / "build.stderr.log").write_bytes(compiled.stderr)
        report["build_exit_code"] = compiled.returncode
        if compiled.returncode:
            raise RuntimeError("MSVC compilation failed; see build logs")
        report["binary_sha256"] = {
            name: sha256((build / (name + ".exe")).read_bytes())
            for name in ("current", "head", "fixture")
        }
        cases = (
            ("native backslashes", "native"),
            ("native trailing separator", "native_trailing"),
            ("forward slashes", "forward"),
            ("uppercase forward slashes and trailing separator", "forward_uppercase_trailing"),
        )
        for number, (name, style) in enumerate(cases, start=1):
            run_case(sandbox, build / "fixture.exe", build / "current.exe", number,
                     name, style, False, report)
        run_case(sandbox, build / "fixture.exe", build / "head.exe", 5,
                 "HEAD prefix collision regression", "native", True, report)
        report["production_function_unchanged_during_test"] = (
            extract((repo / SOURCE).read_bytes())[0] == current_function)
        if not report["production_function_unchanged_during_test"]:
            raise AssertionError("Production function changed during this test")
        report["passed"] = True
    except Exception:
        report["error"] = traceback.format_exc()
    finally:
        try:
            # Only remove the unique sandbox created by this invocation.
            shutil.rmtree(sandbox)
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
        dead = sum(row.get("alive_after_cleanup") is False for row in case["processes"])
        alive = sum(row.get("alive_after_cleanup") is True for row in case["processes"])
        print(f'{case["variant"]}: {case["name"]}: dead={dead}, alive={alive}, passed={case.get("passed", False)}')
    print("sandbox_removed=" + str(report["sandbox_removed"]))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
