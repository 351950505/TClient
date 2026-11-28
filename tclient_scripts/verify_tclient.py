#!/usr/bin/env python3
"""Local, opt-in verification. Never starts DDNet, publishes, or edits user settings."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

SCHEMA_VERSION = 1
LOG_LIMIT = 16 * 1024 * 1024
KNOWN_DISABLED = {
    "ReplaceWords.DISABLED_MissingBoundaries",
    "ReplaceWords.DISABLED_Cyrillic",
    "ReplaceWords.DISABLED_Spread",
}


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git(repo, *args):
    result = subprocess.run(["git", "--no-optional-locks", "-C", str(repo), *args],
                            capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError("Git 检查失败：" + result.stderr.decode("utf-8", errors="replace")[-1000:])
    return result.stdout


def snapshot_repository(repo):
    repo = Path(repo).resolve()
    root = Path(os.fsdecode(git(repo, "rev-parse", "--show-toplevel")).strip()).resolve()
    if root != repo:
        raise ValueError("--repo 必须是源码仓库根目录")
    paths = git(repo, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0")
    files = {}
    for raw in sorted(set(paths)):
        if not raw:
            continue
        relative = os.fsdecode(raw)
        path = repo / relative
        if path.is_symlink():
            files[relative] = "symlink:" + digest_bytes(os.fsencode(os.readlink(path)))
        elif path.is_file():
            files[relative] = file_digest(path)
        elif path.is_dir():
            # Gitlinks are not traversed as ordinary files. A dirty dependency is
            # not a reproducible test input; never silently bless it as clean.
            if not (path / ".git").exists():
                files[relative] = "uninitialized-submodule"
            else:
                subhead = git(path, "rev-parse", "HEAD").decode().strip()
                if git(path, "status", "--porcelain", "--untracked-files=all").strip():
                    raise RuntimeError("子模块存在未冻结改动：" + relative)
                files[relative] = "gitlink:" + subhead
        else:
            files[relative] = "deleted"
    return {
        "repo": str(repo), "head": git(repo, "rev-parse", "HEAD").decode().strip(),
        "dirty": bool(git(repo, "status", "--porcelain", "--untracked-files=all").strip()),
        "files": files,
        "fingerprint": digest_bytes(json.dumps(files, sort_keys=True, ensure_ascii=True).encode()),
    }


def changed_inputs(before, after):
    old, new = before["files"], after["files"]
    return sorted(path for path in old.keys() | new.keys() if old.get(path) != new.get(path))


def git_changed_paths(repo):
    """Return current tracked and untracked Git paths without reading file contents."""
    separator = bytes([0])
    tracked = git(repo, "diff", "--name-only", "--no-renames", "-z", "HEAD", "--").split(separator)
    untracked = git(repo, "ls-files", "--others", "--exclude-standard", "-z").split(separator)
    return sorted({os.fsdecode(path) for path in tracked + untracked if path})


def validate_baseline(report, identity, tool_fingerprint, required_ids):
    try:
        valid = (report["schema_version"] == SCHEMA_VERSION and report["mode"] == "full"
                 and report["automatic_status"] == "PASS" and report["complete"] is True
                 and report["tool_fingerprint"] == tool_fingerprint
                 and report["identity"]["repo"] == identity["repo"]
                 and set(report["required_ids"]) == set(required_ids)
                 and len(report["results"]) == len(required_ids)
                 and {item["id"] for item in report["results"]} == set(required_ids)
                 and all(item["status"] == "PASS" for item in report["results"])
                 and isinstance(report["identity"]["files"], dict))
        if not valid:
            return False, "基线不是当前工具/清单/源码目录的完整通过记录"
        files = report["identity"]["files"]
        if digest_bytes(json.dumps(files, sort_keys=True, ensure_ascii=True).encode()) != report["identity"]["fingerprint"]:
            return False, "基线输入指纹不匹配"
        result = subprocess.run(["git", "-C", identity["repo"], "merge-base", "--is-ancestor",
                                 report["identity"]["head"], identity["head"]], capture_output=True, timeout=30)
        if result.returncode:
            return False, "基线提交不存在或不在当前开发历史中"
        return True, "完整自动回归基线有效（仅用来选测，不复用通过结果）"
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError):
        return False, "基线损坏或无法核验"


def cargo_harness_results(text):
    lines = text.splitlines()
    results, target, kind, running, observed, observed_failures, observed_ignored = [], None, None, None, 0, 0, 0
    summary_re = re.compile(r"^test result: (ok|FAILED)\. (\d+) passed; (\d+) failed; (\d+) ignored; (\d+) measured; (\d+) filtered out")
    for line in lines:
        unit = re.search(r"Running unittests .*[/\\]([^/\\]+?)-[0-9a-f]+(?:\.exe)?\)", line)
        doc = re.search(r"Doc-tests ([A-Za-z0-9_-]+)", line.strip())
        started = re.match(r"\s*running (\d+) tests?\s*$", line)
        summary = summary_re.match(line.strip())
        if unit:
            target, kind, running, observed, observed_failures, observed_ignored = unit.group(1), "unit", None, 0, 0, 0
        elif doc:
            target, kind, running, observed, observed_failures, observed_ignored = doc.group(1), "doc", None, 0, 0, 0
        elif started and target:
            running = int(started.group(1))
        elif target and re.match(r"\s*test .+ \.\.\. ", line):
            observed += 1
            observed_failures += int(line.rstrip().endswith("... FAILED"))
            observed_ignored += int("... ignored" in line)
        elif summary and target:
            results.append({"id": f"{target}:{kind}", "status": summary.group(1),
                            "test_count": int(summary.group(2)), "failed": int(summary.group(3)),
                            "ignored": int(summary.group(4)), "measured": int(summary.group(5)),
                            "filtered": int(summary.group(6)), "running": running,
                            "observed": observed, "observed_failed": observed_failures,
                            "observed_ignored": observed_ignored})
            target, kind, running, observed, observed_failures, observed_ignored = None, None, None, 0, 0, 0
    return results


def evaluate_output(contract, text, returncode, marker=None, *, cargo_targets=None, marker_count_group=None):
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    result = {"status": "PASS", "test_count": None, "reason": "退出码及约定证据匹配"}
    if returncode != 0:
        return {**result, "status": "FAIL", "reason": "测试命令返回非零退出码"}
    if contract == "unittest":
        found = re.search(r"^Ran (\d+) tests? in ", text, re.MULTILINE)
        count = int(found.group(1)) if found else 0
        result["test_count"] = count
        if not count or not re.search(r"^OK\s*$", text, re.MULTILINE):
            result.update(status="BLOCKED", reason="缺少非零测试结果，或出现跳过/预期失败项")
    elif contract == "cargo":
        harnesses = cargo_harness_results(text)
        failed = [item for item in harnesses if item["status"] != "ok" or item["failed"]]
        if failed:
            result.update(status="FAIL", reason="Rust 测试报告失败：" + ", ".join(item["id"] for item in failed),
                          test_count=sum(item["test_count"] for item in harnesses))
        elif not harnesses:
            result.update(status="BLOCKED", reason="缺少 Cargo 测试汇总，不能确认实际运行数")
        else:
            count = sum(item["test_count"] for item in harnesses)
            result.update(test_count=count, harnesses=harnesses, zero_test_harnesses=[
                item["id"] for item in harnesses if item["running"] == 0])
            ids = [item["id"] for item in harnesses]
            summary_count = len(re.findall(r"^\s*test result:", text, re.MULTILINE))
            if (not count or not cargo_targets or set(ids) != set(cargo_targets)
                    or len(ids) != len(set(ids)) or len(ids) != summary_count):
                result.update(status="BLOCKED", reason="Rust 目标缺失、重复、新增，或无实际通过用例")
            elif any(item["running"] != item["test_count"] or item["ignored"] or item["filtered"] or item["measured"]
                     or (item["running"] and item["observed"] != item["test_count"])
                     or item["observed_failed"] or item["observed_ignored"]
                     or item["test_count"] != cargo_targets[item["id"]] for item in harnesses):
                result.update(status="BLOCKED", reason="Rust 存在跳过、过滤、计数不符或少于登记用例")
            else:
                result["reason"] = f"{len(ids)} 个 Rust 目标，其中 {len(result['zero_test_harnesses'])} 个没有用例，不计作测试通过"
    elif contract == "translations":
        simplified = re.search(r"^scanned (\d+) files, (\d+) keys, (\d+) translated, (\d+) missing$", text, re.MULTILINE)
        if (not simplified or not int(simplified[1]) or not int(simplified[2])
                or simplified[2] != simplified[3] or int(simplified[4])
                or "OK: every TCLocalize key has a simplified Chinese translation" not in text.splitlines()):
            result.update(status="BLOCKED", reason="缺少有效的简体词典检查结果")
        else:
            trad = re.search(r"^traditional coverage: (\d+)/(\d+) keys, (\d+) missing, (\d+) reverse-drift$", text, re.MULTILINE)
            result["metrics"] = {"files_scanned": int(simplified[1]), "keys": int(simplified[2]),
                                 "translated": int(simplified[3]), "missing": int(simplified[4])}
            if trad:
                result["metrics"].update(traditional_covered=int(trad[1]), traditional_keys=int(trad[2]),
                                         traditional_missing=int(trad[3]), reverse_drift=int(trad[4]))
            result["reason"] = f"简体 {simplified[3]}/{simplified[2]} 词条；" + (
                f"繁体 {trad[1]}/{trad[2]}，缺 {trad[3]}，反向漂移 {trad[4]}（非阻断）" if trad else "无繁体覆盖报告（非阻断）")
    elif contract == "marker":
        matched = re.search(marker, text, re.MULTILINE) if marker else None
        if not matched:
            result.update(status="BLOCKED", reason="退出成功但缺少登记的完成证据")
        elif marker_count_group is not None:
            count = int(matched.group(marker_count_group))
            result["test_count"] = count
            if not count:
                result.update(status="BLOCKED", reason="完成标记中的检查数为零")
    else:
        result.update(status="BLOCKED", reason="未知测试结果契约")
    return result


def check_count_regression(result, baseline):
    if baseline and result["status"] == "PASS" and result.get("test_count") is not None:
        previous = next((item for item in baseline["results"] if item["id"] == result["id"]), {})
        count = previous.get("test_count")
        if count is not None and result["test_count"] < count:
            return {**result, "status": "BLOCKED", "reason": f"实际用例数从 {count} 降至 {result['test_count']}，需要复核测试清单"}
    return result


def evaluate_gtest(path, expected_disabled=0, discovered=None):
    try:
        root = ET.parse(path).getroot()
        cases = root.findall(".//testcase")
        failed = int(root.get("failures", "0")) + int(root.get("errors", "0"))
        disabled = [case for case in cases if case.get("status") == "notrun"]
        executed = [case for case in cases if case.get("status") == "run"]
        names = {case.get("classname", "") + "." + case.get("name", "") for case in cases}
        status, reason = "PASS", "原生测试 XML 与发现清单匹配"
        if failed or any(case.find("failure") is not None for case in cases):
            status, reason = "FAIL", "原生测试失败"
        elif (not executed or int(root.get("tests", "0")) != len(cases)
              or len(disabled) != expected_disabled
              or any(case.find("skipped") is not None or case.get("result") == "skipped" for case in executed)
              or (discovered is not None and names != set(discovered))):
            status, reason = "BLOCKED", "用例为空、发现数不一致或存在未批准跳过/禁用"
        return {"status": status, "reason": reason, "test_count": len(executed), "disabled_count": len(disabled)}
    except (OSError, ET.ParseError, ValueError):
        return {"status": "BLOCKED", "reason": "原生测试 XML 缺失或无法解析", "test_count": None}


class WindowsJob:
    """Own the complete process tree, including children after the parent exits."""
    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)

        class Limits(ctypes.Structure):
            _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                        ("flags", wintypes.DWORD), ("minimum", ctypes.c_size_t), ("maximum", ctypes.c_size_t),
                        ("process_limit", wintypes.DWORD), ("affinity", ctypes.c_size_t),
                        ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]

        class Extended(ctypes.Structure):
            _fields_ = [("basic", Limits), ("io", ctypes.c_ulonglong * 6),
                        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                        ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

        class Accounting(ctypes.Structure):
            _fields_ = [("times", ctypes.c_longlong * 4), ("page_faults", wintypes.DWORD),
                        ("total", wintypes.DWORD), ("active", wintypes.DWORD), ("terminated", wintypes.DWORD)]

        self.accounting = Accounting
        signatures = {
            "CreateJobObjectW": ([ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            "SetInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            "QueryInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
            "AssignProcessToJobObject": ([wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            "TerminateJobObject": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = Extended()
        limits.basic.flags = 0x2000 | 0x20  # KILL_ON_JOB_CLOSE and PRIORITY_CLASS
        limits.basic.priority = 0x4000  # BELOW_NORMAL_PRIORITY_CLASS, inherited by children
        if not self.kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.close()
            raise OSError("无法设置进程树回收与低优先级约束")

    def assign(self, process):
        if not self.kernel.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise self.ctypes.WinError(self.ctypes.get_last_error())

    def active(self):
        info = self.accounting()
        if not self.kernel.QueryInformationJobObject(self.handle, 1, self.ctypes.byref(info), self.ctypes.sizeof(info), None):
            raise self.ctypes.WinError(self.ctypes.get_last_error())
        return info.active

    def terminate(self):
        self.kernel.TerminateJobObject(self.handle, 1)
        deadline = time.monotonic() + 5
        while self.active() and time.monotonic() < deadline:
            time.sleep(0.02)

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def stop_process(process):
    # Only the PID created by this invocation, never kill by executable name.
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=15)
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def wait_log_release(path):
    if os.name != "nt" or not Path(path).exists():
        return True
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                   wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    deadline = time.monotonic() + 5
    while True:
        # A terminated Windows process can briefly retain inherited file handles.
        # Probe delete sharing, not deletion; no log or user file is removed here.
        handle = kernel.CreateFileW(str(path), 0x10000, 7, None, 3, 0, None)
        if handle != ctypes.c_void_p(-1).value:
            kernel.CloseHandle(handle)
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.02)


def run_command(argv, cwd, log_path, timeout=120, env=None):
    started = time.monotonic()
    kwargs = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {"start_new_session": True}
    result = {"status": "PASS", "reason": "命令完成", "returncode": None, "log": str(log_path)}
    job = None
    try:
        job = WindowsJob() if os.name == "nt" else None
        with Path(log_path).open("xb") as stream:
            command = [str(value) for value in argv]
            if job:
                if not shutil.which(command[0], path=(env if env is not None else os.environ).get("PATH")):
                    raise FileNotFoundError(command[0])
                # No target code runs before the launcher belongs to our job.
                launcher = ("import sys, subprocess; "
                            "sys.exit(subprocess.call(sys.argv[1:], stdin=subprocess.DEVNULL) "
                            "if sys.stdin.buffer.read(1) == b'1' else 2)")
                command = [sys.executable, "-I", "-S", "-B", "-c", launcher, *command]
            process = subprocess.Popen(command, cwd=cwd, env=env,
                                       stdin=subprocess.PIPE if job else subprocess.DEVNULL,
                                       stdout=stream, stderr=subprocess.STDOUT, **kwargs)
            try:
                if job:
                    job.assign(process)
                    process.stdin.write(b"1")
                    process.stdin.close()
                while process.poll() is None:
                    if time.monotonic() - started > timeout:
                        result.update(status="FAIL", reason="执行超时，已终止本轮进程树")
                        stop_process(process)
                        break
                    if Path(log_path).stat().st_size > LOG_LIMIT:
                        result.update(status="FAIL", reason="日志超过上限，已终止本轮进程树")
                        stop_process(process)
                        break
                    time.sleep(0.05)
                if job:
                    deadline = time.monotonic() + 0.5
                    while job.active() and time.monotonic() < deadline:
                        time.sleep(0.02)
                    remaining = job.active() > 0
                else:
                    try:
                        os.killpg(process.pid, 0)
                        remaining = True
                    except ProcessLookupError:
                        remaining = False
                if remaining:
                    if job:
                        job.terminate()
                    else:
                        os.killpg(process.pid, signal.SIGKILL)
                    if result["status"] == "PASS":
                        result.update(status="FAIL", reason="父进程退出后仍有子进程，已清理本轮进程树")
            except BaseException:
                stop_process(process)
                raise
            result["returncode"] = process.returncode
            if result["status"] == "PASS" and process.returncode:
                result.update(status="FAIL", reason="命令返回非零退出码")
    except (OSError, subprocess.SubprocessError) as error:
        result.update(status="BLOCKED", reason="执行环境不可用：" + str(error))
    finally:
        if job:
            job.close()
    if not wait_log_release(log_path):
        result.update(status="BLOCKED", reason="本轮日志句柄仍被占用，不能确认进程清理完成")
    result["duration_seconds"] = round(time.monotonic() - started, 3)
    return result


def read_log(path):
    with Path(path).open("rb") as stream:
        stream.seek(max(0, Path(path).stat().st_size - LOG_LIMIT))
        return stream.read(LOG_LIMIT).decode("utf-8", errors="replace")


def test_environment(temporary, parent):
    # Offline mode is forced for all registered suites, not inherited from a
    # potentially network-enabled Cargo configuration in the parent shell.
    clean = {k: v for k, v in parent.items() if not k.upper().startswith(("GTEST_", "TESTBRIDGE_"))}
    return {**clean, "TEMP": temporary, "TMP": temporary, "TMPDIR": temporary,
            "PYTHONDONTWRITEBYTECODE": "1", "PYTHONUTF8": "1", "CMAKE_BUILD_PARALLEL_LEVEL": "2",
            "CL_MPCount": "1", "MSBUILDDISABLENODEREUSE": "1",
            "DOTNET_CLI_USE_MSBUILD_SERVER": "0", "CARGO_NET_OFFLINE": "true", "CARGO_BUILD_JOBS": "2",
            "GIT_ALLOW_PROTOCOL": "file",
            "RUST_TEST_THREADS": "2"}


def build_command(cmake, build_dir, generator, targets):
    command = [str(cmake), "--build", str(build_dir), "--config", "Release", "--target", *targets, "--parallel", "2"]
    if generator.startswith("Visual Studio"):
        command += ["--", "/nr:false", "/p:CL_MPCount=1"]
    return command


def read_cache(build_dir):
    cache = {}
    for line in (Path(build_dir) / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^:#]+):[^=]+=(.*)", line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def tool_identity(catalog, build_dir):
    values = {"catalog": catalog, "python": sys.version, "platform": platform.platform()}
    for name in ("verify_tclient.py", "verification_policy.py", "verification_catalog.py"):
        values[name] = file_digest(Path(__file__).with_name(name))
    for name in ("git", "node", "cmake", "cargo", "cl"):
        executable = shutil.which(name)
        values[name] = [executable, file_digest(executable)] if executable else None
    values["environment"] = digest_bytes(json.dumps({key: os.environ.get(key) for key in
                                                   ("PATH", "CC", "CXX", "CL", "INCLUDE", "LIB", "CMAKE_GENERATOR")}, sort_keys=True).encode())
    if build_dir and (build_dir / "CMakeCache.txt").is_file():
        values["build_cache"] = file_digest(build_dir / "CMakeCache.txt")
        cached_cmake = read_cache(build_dir).get("CMAKE_COMMAND", "")
        if cached_cmake and Path(cached_cmake).is_file():
            values["configured_cmake"] = [cached_cmake, file_digest(cached_cmake)]
    return digest_bytes(json.dumps(values, sort_keys=True).encode())


def vs_release_exe(build_dir, target):
    project = ET.parse(build_dir / (target + ".vcxproj"))
    fields = {}
    for field in ("OutDir", "TargetName", "TargetExt"):
        values = {elem.text for elem in project.findall(".//{*}" + field)
                  if "=='Release|x64'" in elem.get("Condition", "").replace(" ", "")}
        if len(values) != 1 or not all(values):
            raise ValueError("不能确认 Release|x64 目标路径：" + target)
        fields[field] = values.pop()
    path = Path(fields["OutDir"]) / (fields["TargetName"] + fields["TargetExt"])
    if not path.is_absolute() or "$" in str(path) or not path.resolve().is_relative_to(build_dir.resolve()):
        raise ValueError("不支持的目标输出路径：" + str(path))
    return path


def run_native(repo, build_dir, output, env, binary):
    if not binary.is_file():
        return {"status": "BLOCKED", "reason": "本轮构建未产生 testrunner", "test_count": None}
    env = {**env, "PATH": str(binary.parent) + os.pathsep + str(build_dir) + os.pathsep + env.get("PATH", "")}
    listing = run_command([binary, "--gtest_list_tests"], repo, output / "native-discovery.log", 60, env)
    if listing["status"] != "PASS":
        return listing
    names, suite = set(), ""
    for line in read_log(output / "native-discovery.log").splitlines():
        name = line.split("#", 1)[0].strip()
        if not name:
            continue
        if not line.startswith(" ") and name.endswith("."):
            suite = name
        elif line.startswith("  ") and suite and re.fullmatch(r"[\w/.-]+", name):
            names.add(suite + name)
    disabled = {name for name in names if "DISABLED_" in name}
    if not names or disabled != KNOWN_DISABLED:
        return {"status": "BLOCKED", "reason": "未发现测试或禁用用例清单变化", "test_count": None}
    # These are native unit-test fixtures, not a client profile. No DDNet process
    # or storage.cfg is created. All test writes stay in the system temporary dir.
    with tempfile.TemporaryDirectory(prefix="tclient-native-") as directory:
        working = Path(directory)
        shutil.copytree(repo / "data", working / "data")
        result = run_command([binary, "--gtest_output=xml:" + str(output / "native.xml")],
                             working, output / "native.log", 1800, env)
        if result["status"] == "PASS":
            result.update(evaluate_gtest(output / "native.xml", len(KNOWN_DISABLED), names))
        result["binary_sha256"] = file_digest(binary)
        result["known_disabled"] = sorted(KNOWN_DISABLED)
        return result


def execute_suite(suite, repo, output, build_dir, built, env):
    identifier = suite["id"]
    if suite["type"] == "build":
        if not build_dir:
            return {"status": "BLOCKED", "reason": "需要 --build-dir 指向当前源码的已配置构建目录"}
        try:
            cache = read_cache(build_dir)
            if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != repo:
                return {"status": "BLOCKED", "reason": "构建目录不属于当前源码"}
            if cache.get("TEST_MYSQL", "").upper() not in ("OFF", "FALSE", "NO", "0"):
                return {"status": "BLOCKED", "reason": "必须确认 TEST_MYSQL=OFF，禁止启动会写入持久数据库的测试"}
            if cache.get("TCLIENT_RELEASE_MIRRORS", "").upper() not in ("OFF", "FALSE", "NO", "0"):
                return {"status": "BLOCKED", "reason": "本地候选构建必须使用 TCLIENT_RELEASE_MIRRORS=OFF，不运行发布镜像探测"}
            if not cache.get("CMAKE_GENERATOR", "").startswith("Visual Studio"):
                return {"status": "BLOCKED", "reason": "目前仅验证 Visual Studio Release|x64 的产物路径"}
            gtest_library = cache.get("GTEST_LIBRARY", "")
            if cache.get("SERVER") != "ON" or (cache.get("DOWNLOAD_GTEST") != "ON" and not Path(gtest_library).is_file()):
                return {"status": "BLOCKED", "reason": "当前构建未启用 server/GTest 测试依赖"}
            cmake = cache.get("CMAKE_COMMAND") or shutil.which("cmake")
            if not cmake or not Path(cmake).is_file():
                return {"status": "BLOCKED", "reason": "缺少 cmake，请先准备构建环境"}
            result = run_command(build_command(cmake, build_dir, cache.get("CMAKE_GENERATOR", ""), ["game-client", "testrunner"]),
                                 repo, output / (identifier + ".log"), 3600, env)
            if result["status"] == "PASS":
                artifacts = []
                for name in ("game-client", "testrunner"):
                    binary = vs_release_exe(build_dir, name)
                    if not binary.is_file():
                        return {**result, "status": "BLOCKED", "reason": "构建成功但缺少约定产物：" + name}
                    artifacts.append({"target": name, "path": str(binary), "sha256": file_digest(binary), "size": binary.stat().st_size})
                result["artifacts"] = artifacts
            return result
        except (OSError, ValueError, ET.ParseError) as error:
            return {"status": "BLOCKED", "reason": "构建缓存不可用：" + str(error)}
    if suite["type"] == "cmake-target":
        if not built or not build_dir:
            return {"status": "BLOCKED", "reason": "本轮未成功编译当前候选，不运行 Rust 测试目标"}
        if any(not Path(item["path"]).is_file() or file_digest(item["path"]) != item["sha256"] for item in built):
            return {"status": "BLOCKED", "reason": "本轮构建产物已被替换，不运行混代测试目标"}
        try:
            cache = read_cache(build_dir)
            if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != repo:
                return {"status": "BLOCKED", "reason": "Rust 构建目录不属于当前源码"}
            cmake = cache.get("CMAKE_COMMAND") or shutil.which("cmake")
            if not cmake or not Path(cmake).is_file():
                return {"status": "BLOCKED", "reason": "当前构建目录的 CMake 不可用"}
            result = run_command(build_command(cmake, build_dir, cache.get("CMAKE_GENERATOR", ""), [suite["target"]]),
                                 repo, output / (identifier + ".log"), suite.get("timeout", 3600), env)
            if result["status"] == "PASS":
                result.update(evaluate_output(suite["contract"], read_log(output / (identifier + ".log")),
                                              result["returncode"], cargo_targets=suite["targets"]))
            if any(file_digest(item["path"]) != item["sha256"] for item in built):
                result.update(status="BLOCKED", reason="Rust 测试后候选程序哈希变化，证据失效")
            return result
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            return {"status": "BLOCKED", "reason": "Rust 测试环境不可用：" + str(error)}
    if suite["type"] == "native":
        if not built or not build_dir:
            return {"status": "BLOCKED", "reason": "本轮未成功构建，不运行旧的 testrunner"}
        if read_cache(build_dir).get("TEST_MYSQL", "").upper() not in ("OFF", "FALSE", "NO", "0"):
            return {"status": "BLOCKED", "reason": "TEST_MYSQL 未确认关闭，不启动原生测试"}
        if any(not Path(item["path"]).is_file() or file_digest(item["path"]) != item["sha256"] for item in built):
            return {"status": "BLOCKED", "reason": "本轮构建产物已被替换，不运行旧或混代二进制"}
        binary = next((Path(item["path"]) for item in built if item.get("target") == "testrunner"), None)
        if binary is None:
            return {"status": "BLOCKED", "reason": "缺少本轮 testrunner 目标证据"}
        result = run_native(repo, build_dir, output, env, binary)
        if any(file_digest(item["path"]) != item["sha256"] for item in built):
            result.update(status="BLOCKED", reason="原生测试期间构建产物变化，证据失效")
        return result
    program = sys.executable if suite["type"] == "python" else shutil.which("node")
    if not program:
        return {"status": "BLOCKED", "reason": "缺少 " + suite["type"] + " 执行工具"}
    missing = [name for name in suite.get("requires_python", []) if importlib.util.find_spec(name) is None]
    if missing:
        return {"status": "BLOCKED", "reason": "缺少 Python 依赖：" + ", ".join(missing) + "；不会自动联网安装"}
    script = repo / suite["path"]
    if not script.is_file():
        return {"status": "BLOCKED", "reason": "登记的测试文件不存在：" + suite["path"]}
    command = [program, *(["-X", "utf8", "-I", "-B"] if suite["type"] == "python" else []), str(script), *suite.get("args", [])]
    result = run_command(command, repo, output / (identifier + ".log"), suite.get("timeout", 120), env)
    if result["status"] == "PASS":
        result.update(evaluate_output(suite["contract"], read_log(output / (identifier + ".log")),
                                      result["returncode"], suite.get("marker"), cargo_targets=suite.get("targets"),
                                      marker_count_group=suite.get("marker_count_group")))
    return result


def write_report(output, report):
    for result in report["results"]:
        if result.get("log") and Path(result["log"]).is_absolute():
            result["log"] = str(Path(result["log"]).relative_to(output))
    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# TClient 本地验证结果", "", "- 模式：`" + report["mode"] + "`",
             "- 自动验证：**" + report["automatic_status"] + "**",
             "- 范围：仅当前已登记的安全本地测试；不代表全功能/GUI/在线验收完成。",
             "- 源码：`" + report["identity"]["repo"] + "`",
             "- 提交：`" + report["identity"]["head"] + "`",
             "- 输入指纹：`" + report["identity"]["fingerprint"] + "`",
             "- 选测说明：" + report["selection"].get("escalation", ""), "",
             "| 测试组 | 状态 | 类型 | 实际用例数 | 说明 |", "|---|---|---|---|---|"]
    for item in report["results"]:
        reason = item.get("reason", "").replace("|", "/").replace("\n", " ")
        lines.append(f"| {item['id']} | {item['status']} | {item.get('evidence', '')} | {item.get('test_count') if item.get('test_count') is not None else '未计数'} | {reason} |")
    lines += ["", "## 本次选择", *["- " + item["id"] + "：" + item["reason"] for item in report["selection"]["selected"]],
              "", "## 本次未选择", *["- " + item["id"] + "：" + item["reason"] for item in report["selection"]["not_selected"]],
              "", "## 实机边界", "未启动客户端；不自动修改真实配置、绑定或渲染后端。",
              *["- NEEDS_MANUAL：" + item for item in report["selection"]["manual"]],
              "", "## 尚未纳入的测试", *["- " + item for item in report["coverage_gaps"]]]
    if report.get("error"):
        lines += ["", "## 阻断原因", report["error"]]
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv=None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from verification_catalog import SUITES, COVERAGE_GAPS
    from verification_policy import plan_tests
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("plan", "quick", "affected", "full"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--baseline", type=Path, help="上次本工具完整 full 通过的 report.json；只用于选测")
    parser.add_argument("--build-dir", type=Path, help="显式选择现有构建目录；不会自动配置/下载依赖")
    parser.add_argument("--output-dir", type=Path, help="仓库外的新目录；默认系统临时目录")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    output = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix="tclient-verification-"))
    if output == repo or repo in output.parents:
        parser.error("报告目录必须在源码仓库之外，避免改变被测输入")
    if args.output_dir:
        if output.exists():
            parser.error("--output-dir 必须是新目录，不覆盖旧证据")
        output.mkdir(parents=True)
    build_dir = args.build_dir.resolve() if args.build_dir else None
    try:
        identity = snapshot_repository(repo)
        tool = tool_identity(SUITES, build_dir)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        (output / "blocked.txt").write_text(str(error) + "\n", encoding="utf-8")
        print("BLOCKED：" + str(error) + "\n证据：" + str(output))
        return 2
    trusted, reason, baseline = False, "未提供可信 full 基线", None
    if args.baseline:
        try:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
            trusted, reason = validate_baseline(baseline, identity, tool, [suite["id"] for suite in SUITES])
        except (OSError, ValueError):
            reason = "基线文件缺失或格式错误"
    if trusted:
        changes = changed_inputs(baseline["identity"], identity)
    else:
        changes = git_changed_paths(repo)
    selection = plan_tests(args.mode, changes, SUITES, trusted_baseline=trusted, baseline_reason=reason)
    report = {"schema_version": SCHEMA_VERSION, "mode": args.mode, "identity": identity,
              "tool_fingerprint": tool, "baseline_valid": trusted, "baseline_reason": reason,
              "changes": changes, "selection": selection, "results": [], "required_ids": [s["id"] for s in SUITES],
              "automatic_status": "NOT_RUN", "complete": False, "coverage_gaps": COVERAGE_GAPS,
              "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    if args.mode == "plan":
        write_report(output, report)
        print("NOT_RUN：仅生成选测计划；未执行测试。\n报告：" + str(output / "summary.md"))
        return 0
    built = False
    with tempfile.TemporaryDirectory(prefix="tclient-test-tmp-") as temporary:
        env = test_environment(temporary, os.environ.copy())
        for selected in selection["selected"]:
            suite = next(s for s in SUITES if s["id"] == selected["id"])
            print("运行：" + suite["id"], flush=True)
            try:
                result = execute_suite(suite, repo, output, build_dir, built, env)
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
                result = {"status": "BLOCKED", "reason": str(error)}
            result.update(id=suite["id"], evidence=suite["evidence"], selected_because=selected["reason"])
            result = check_count_regression(result, baseline if trusted else None)
            report["results"].append(result)
            if suite["type"] == "build":
                built = result.get("artifacts", []) if result["status"] == "PASS" else False
            write_report(output, report)
            print(suite["id"] + ": " + result["status"], flush=True)
        try:
            after = snapshot_repository(repo)
            if after["fingerprint"] != identity["fingerprint"] or tool_identity(SUITES, build_dir) != tool:
                report["error"] = "测试期间输入或工具/构建配置变化；本轮证据失效，请在稳定候选上重跑"
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            report["error"] = "无法完成输入复核：" + str(error)
    statuses = {result["status"] for result in report["results"]}
    report["automatic_status"] = "FAIL" if "FAIL" in statuses else "BLOCKED" if "BLOCKED" in statuses or report.get("error") or not statuses else "PASS"
    report["complete"] = report["automatic_status"] == "PASS" and args.mode == "full" and len(report["results"]) == len(SUITES)
    write_report(output, report)
    print(report["automatic_status"] + "（仅登记的自动验证范围）\n报告：" + str(output / "summary.md"))
    return 0 if report["automatic_status"] == "PASS" else 1 if report["automatic_status"] == "FAIL" else 2


if __name__ == "__main__":
    raise SystemExit(main())
