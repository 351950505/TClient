#!/usr/bin/env python3
"""Exercise the actual Windows updater using disposable TEMP installations.

Set TCLIENT_UPDATER_EXE to the built helper. A Windows C++ compiler (MSVC or
MinGW) builds small fixture PEs; no game, user configuration, registry, visible
window, or non-test process is used. Optional TCLIENT_UPDATER_TEST_EVIDENCE copies
logs/results only to the chosen evidence directory. Run one case with:
    python test_tclient_updater_install.py TClientUpdaterInstallTest.test_normal_success
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
CREATE_NEW_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE = 0x0001
SYNCHRONIZE = 0x00100000
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 258
ERROR_INVALID_PARAMETER = 87
READY_NAMES = ("TCLIENT_UPDATER_READY_FILE", "TCLIENT_UPDATER_READY_TOKEN")

FIXTURE_SOURCE = r'''
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#ifndef FIXTURE_KIND
#define FIXTURE_KIND 0
#endif

static std::wstring Environment(const wchar_t *pName)
{
	const DWORD Required = GetEnvironmentVariableW(pName, nullptr, 0);
	if(Required == 0)
		return std::wstring();
	std::vector<wchar_t> Buffer(Required);
	const DWORD Written = GetEnvironmentVariableW(pName, Buffer.data(), (DWORD)Buffer.size());
	if(Written == 0 || Written >= Buffer.size())
		return std::wstring();
	return std::wstring(Buffer.data(), Written);
}

static bool WriteBytes(const std::wstring &Path, const std::string &Contents, DWORD Creation = CREATE_ALWAYS)
{
	if(Path.empty())
		return false;
	HANDLE hFile = CreateFileW(Path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, Creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
	if(hFile == INVALID_HANDLE_VALUE)
		return false;
	DWORD Written = 0;
	const bool Success = WriteFile(hFile, Contents.data(), (DWORD)Contents.size(), &Written, nullptr) && Written == Contents.size() && FlushFileBuffers(hFile);
	CloseHandle(hFile);
	return Success;
}

static bool WritePid(const std::wstring &Path)
{
	char aPid[64];
	const int Length = std::snprintf(aPid, sizeof(aPid), "%lu\n", (unsigned long)GetCurrentProcessId());
	return Length > 0 && WriteBytes(Path, std::string(aPid, (size_t)Length));
}

static bool WriteEvent(const wchar_t *pName)
{
	return WritePid(Environment(pName));
}

static std::string ConsoleState()
{
	std::vector<DWORD> Processes(16);
	SetLastError(ERROR_SUCCESS);
	DWORD Count = GetConsoleProcessList(Processes.data(), (DWORD)Processes.size());
	if(Count > Processes.size())
	{
		Processes.resize(Count);
		Count = GetConsoleProcessList(Processes.data(), (DWORD)Processes.size());
	}
	const DWORD Error = Count == 0 ? GetLastError() : ERROR_SUCCESS;
	std::string Result = "{\"window\":" + std::to_string((unsigned long long)(UINT_PTR)GetConsoleWindow()) + ",\"error\":" + std::to_string(Error) + ",\"pids\":[";
	for(DWORD i = 0; i < Count && i < Processes.size(); ++i)
	{
		if(i)
			Result += ",";
		Result += std::to_string(Processes[i]);
	}
	return Result + "]}";
}

static std::string JsonString(const wchar_t *pText)
{
	std::string Result = "\"";
	for(; *pText; ++pText)
	{
		char aEscape[7];
		std::snprintf(aEscape, sizeof(aEscape), "\\u%04x", (unsigned int)*pText);
		Result += aEscape;
	}
	return Result + "\"";
}

static bool InitializeConsole(int argc, wchar_t **argv, const wchar_t *pEventName)
{
	if(Environment(L"TCLIENT_FIXTURE_CONSOLE_REPORT").empty())
		return true;
	bool Silent = false;
	std::string Arguments = "[";
	for(int i = 0; i < argc; ++i)
	{
		if(i)
			Arguments += ",";
		Arguments += JsonString(argv[i]);
		if(std::wstring(argv[i]) == L"-s" || std::wstring(argv[i]) == L"--silent")
			Silent = true;
	}
	Arguments += "]";
	const std::string Before = ConsoleState();
	const DWORD StdoutType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
	// Match client.cpp's --silent gate and log.cpp's explicit parent attachment.
	const bool AttachAttempted = !Silent && StdoutType == FILE_TYPE_UNKNOWN;
	const bool Attached = AttachAttempted && AttachConsole(ATTACH_PARENT_PROCESS);
	const std::string Report = "{\"argv\":" + Arguments + ",\"silent\":" + (Silent ? "true" : "false") + ",\"stdout_type\":" + std::to_string(StdoutType) + ",\"attach_attempted\":" + (AttachAttempted ? "true" : "false") + ",\"attached\":" + (Attached ? "true" : "false") + ",\"before\":" + Before + ",\"after\":" + ConsoleState() + "}";
	return WriteBytes(Environment(pEventName) + L".console.json", Report);
}

static void AwaitConsoleProbe(const wchar_t *pEventName)
{
	if(Environment(L"TCLIENT_FIXTURE_CONSOLE_REPORT").empty())
		return;
	const std::wstring Event = Environment(pEventName);
	const ULONGLONG Deadline = GetTickCount64() + 30000;
	while(GetTickCount64() < Deadline)
	{
		if(GetFileAttributesW((Event + L".probe").c_str()) != INVALID_FILE_ATTRIBUTES)
		{
			WriteBytes(Event + L".after.json", ConsoleState());
			return;
		}
		Sleep(10);
	}
}

static bool WriteReadyMarker()
{
	const std::wstring Path = Environment(L"TCLIENT_UPDATER_READY_FILE");
	const std::wstring Token = Environment(L"TCLIENT_UPDATER_READY_TOKEN");
	SetEnvironmentVariableW(L"TCLIENT_UPDATER_READY_FILE", nullptr);
	SetEnvironmentVariableW(L"TCLIENT_UPDATER_READY_TOKEN", nullptr);
	if(Path.empty() || Token.empty())
		return false;
	const int Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Token.c_str(), (int)Token.size(), nullptr, 0, nullptr, nullptr);
	if(Length <= 0)
		return false;
	std::string Contents((size_t)Length, '\0');
	if(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Token.c_str(), (int)Token.size(), Contents.data(), Length, nullptr, nullptr) != Length)
		return false;
	Contents.push_back('\n');
	return WriteBytes(Path, Contents, CREATE_NEW);
}

static bool Spawn(const std::vector<std::wstring> &Arguments)
{
	if(Arguments.empty() || Arguments[0].empty())
		return false;
	std::wstring CommandLine;
	for(const std::wstring &Argument : Arguments)
	{
		if(!CommandLine.empty())
			CommandLine += L" ";
		CommandLine += L"\"" + Argument + L"\"";
	}
	std::vector<wchar_t> Buffer(CommandLine.begin(), CommandLine.end());
	Buffer.push_back(L'\0');
	STARTUPINFOW StartupInfo = {};
	StartupInfo.cb = sizeof(StartupInfo);
	PROCESS_INFORMATION ProcessInfo = {};
	if(!CreateProcessW(Arguments[0].c_str(), Buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &StartupInfo, &ProcessInfo))
		return false;
	CloseHandle(ProcessInfo.hThread);
	CloseHandle(ProcessInfo.hProcess);
	return true;
}

static std::wstring gs_ChildPath;
static std::wstring gs_ChildEvent;

static LRESULT CALLBACK FixtureWindowProc(HWND hWindow, UINT Message, WPARAM WParam, LPARAM LParam)
{
	if(Message == WM_CLOSE)
	{
		const bool Started = Spawn({gs_ChildPath, gs_ChildEvent});
		DestroyWindow(hWindow);
		PostQuitMessage(Started ? 0 : 44);
		return 0;
	}
	return DefWindowProcW(hWindow, Message, WParam, LParam);
}

int wmain(int argc, wchar_t **argv)
{
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if FIXTURE_KIND == 1 || FIXTURE_KIND == 6
	if(!WriteEvent(L"TCLIENT_FIXTURE_NEW_EVENT"))
		return 40;
	if(!InitializeConsole(argc, argv, L"TCLIENT_FIXTURE_NEW_EVENT"))
		return 55;
	if(!WriteReadyMarker())
		return 41;
#if FIXTURE_KIND == 6
	if(!WriteEvent(L"TCLIENT_FIXTURE_MARKER_EVENT"))
		return 48;
	Sleep(100);
	return 24;
#else
	AwaitConsoleProbe(L"TCLIENT_FIXTURE_NEW_EVENT");
	Sleep(120000);
	return 0;
#endif
#elif FIXTURE_KIND == 2
	WriteEvent(L"TCLIENT_FIXTURE_NEW_EVENT");
	return 23;
#elif FIXTURE_KIND == 3
	// Recovery must not receive the startup handshake meant for the new client.
	if(!Environment(L"TCLIENT_UPDATER_READY_FILE").empty() || !Environment(L"TCLIENT_UPDATER_READY_TOKEN").empty())
		return 49;
	if(!WriteEvent(L"TCLIENT_FIXTURE_OLD_EVENT"))
		return 42;
	if(!InitializeConsole(argc, argv, L"TCLIENT_FIXTURE_OLD_EVENT"))
		return 55;
	AwaitConsoleProbe(L"TCLIENT_FIXTURE_OLD_EVENT");
	Sleep(120000);
	return 0;
#elif FIXTURE_KIND == 4
	if(argc > 3)
	{
		gs_ChildPath = argv[2];
		gs_ChildEvent = argv[3];
		WNDCLASSW WindowClass = {};
		WindowClass.lpfnWndProc = FixtureWindowProc;
		WindowClass.hInstance = GetModuleHandleW(nullptr);
		WindowClass.lpszClassName = L"TClientUpdaterTestInvisibleWindow";
		if(!RegisterClassW(&WindowClass))
			return 50;
		// A hidden top-level window is enumerated by EnumWindows but never shown.
		HWND hWindow = CreateWindowW(WindowClass.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, WindowClass.hInstance, nullptr);
		if(!hWindow || !WritePid(argv[1]))
			return 51;
		MSG Message;
		while(GetMessageW(&Message, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&Message);
			DispatchMessageW(&Message);
		}
		return (int)Message.wParam;
	}
	if(argc > 1 && !WritePid(argv[1]))
		return 43;
	Sleep(120000);
	return 0;
#elif FIXTURE_KIND == 5
	if(argc < 3)
		return 45;
	HANDLE hFile = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(hFile == INVALID_HANDLE_VALUE)
		return 46;
	if(!WritePid(argv[2]))
	{
		CloseHandle(hFile);
		return 47;
	}
	Sleep(120000);
	CloseHandle(hFile);
	return 0;
#elif FIXTURE_KIND == 7
	if(!WriteEvent(L"TCLIENT_FIXTURE_NEW_EVENT"))
		return 40;
	if(!Spawn({Environment(L"TCLIENT_FIXTURE_CHILD_EXE"), Environment(L"TCLIENT_FIXTURE_CHILD_EVENT")}))
		return 52;
	Sleep(120000);
	return 0;
#elif FIXTURE_KIND == 8
	if(!WriteEvent(L"TCLIENT_FIXTURE_NEW_EVENT"))
		return 40;
	const std::wstring LockEvent = Environment(L"TCLIENT_FIXTURE_LOCK_EVENT");
	if(!Spawn({Environment(L"TCLIENT_FIXTURE_LOCKER_EXE"), Environment(L"TCLIENT_FIXTURE_LOCK_PATH"), LockEvent}))
		return 53;
	for(int Attempt = 0; Attempt < 200; Attempt++)
	{
		if(GetFileAttributesW(LockEvent.c_str()) != INVALID_FILE_ATTRIBUTES)
			return 23;
		Sleep(10);
	}
	return 54;
#else
	return 99;
#endif
}
'''


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def wait_for_pid_event(path: Path, timeout: float = 5.0) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = int(path.read_text(encoding="ascii").strip())
            if value > 0:
                return value
        except (OSError, ValueError):
            pass
        time.sleep(0.025)
    raise AssertionError(f"fixture PID event was not written: {path}")


def write_zip(path: Path, entries: list[tuple[str, bytes]]) -> str:
    import zipfile

    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in entries:
            archive.writestr(name, data)
    return file_sha256(path)


def updater_path() -> Path:
    override = os.environ.get("TCLIENT_UPDATER_EXE")
    if override:
        path = Path(override).resolve()
        if not path.is_file():
            raise AssertionError(f"TCLIENT_UPDATER_EXE does not exist: {path}")
        return path
    for path in (ROOT / "build" / "TClientUpdater.exe", ROOT / "build-vs2026" / "Release" / "TClientUpdater.exe"):
        if path.is_file():
            return path.resolve()
    raise unittest.SkipTest("build TClientUpdater.exe or set TCLIENT_UPDATER_EXE")


def find_vcvars() -> Path | None:
    explicit = os.environ.get("TCLIENT_VCVARS64")
    if explicit:
        return Path(explicit) if Path(explicit).is_file() else None
    candidates = sorted(Path("C:/").glob("VS*/BuildTools/VC/Auxiliary/Build/vcvars64.bat"), reverse=True)
    for directory in ("ProgramFiles", "ProgramFiles(x86)"):
        base = Path(os.environ.get(directory, "C:/Program Files")) / "Microsoft Visual Studio"
        candidates += sorted(base.glob("*/*/VC/Auxiliary/Build/vcvars64.bat"), reverse=True)
    return next((path for path in candidates if path.is_file()), None)


def compile_fixture(source: Path, output: Path, kind: int) -> None:
    cl = shutil.which("cl")
    mingw = next((path for name in ("g++", "x86_64-w64-mingw32-g++", "clang++") if (path := shutil.which(name))), None)
    flags = ["/nologo", "/std:c++17", "/EHsc", "/O2", "/MT", "/utf-8", "/DUNICODE", "/D_UNICODE", f"/DFIXTURE_KIND={kind}"]
    # The healthy and recovered clients match DDNet's GUI subsystem: no initial
    # console inheritance, but log_logger_stdout can explicitly attach later.
    gui_client = kind in (1, 3)
    link_flags = ["/SUBSYSTEM:WINDOWS", "/ENTRY:wmainCRTStartup"] if gui_client else ["/SUBSYSTEM:CONSOLE"]
    if cl:
        command = [cl, *flags, str(source), f"/Fe:{output}", "/link", *link_flags, "user32.lib"]
    elif mingw:
        command = [mingw, "-std=c++17", "-O2", "-municode", "-static", f"-DFIXTURE_KIND={kind}", str(source), "-o", str(output), "-luser32"]
        if gui_client:
            command += ["-mwindows", "-Wl,-e,wmainCRTStartup"]
    elif (vcvars := find_vcvars()) is not None:
        # cmd.exe receives unescaped batch text, not subprocess's CRT quote rules.
        batch = output.parent / "compile-fixture.cmd"
        batch.write_text(
            "@echo off\nchcp 65001 >nul\n"
            f'call "{vcvars}" >nul\n'
            "if errorlevel 1 exit /b 1\n"
            f'cl {" ".join(flags)} "{source}" /Fe:"{output}" /link {" ".join(link_flags)} user32.lib\n',
            encoding="utf-8",
        )
        # Preserve NoDefaultCurrentDirectoryInExePath: explicitly invoke the local batch.
        command = ["cmd.exe", "/d", "/c", f".\\{batch.name}"]
    else:
        raise unittest.SkipTest("MSVC or MinGW is required to build Windows fixture PEs")
    log = output.with_suffix(".compile.log")
    with log.open("wb") as stream:
        result = subprocess.run(command, cwd=output.parent, stdout=stream, stderr=subprocess.STDOUT, timeout=90, creationflags=CREATE_NO_WINDOW)
    if result.returncode or not output.is_file():
        raise AssertionError(f"fixture compile failed ({kind}):\n{log.read_text(encoding='utf-8', errors='replace')}")


@unittest.skipUnless(os.name == "nt", "actual updater integration requires Windows")
class TClientUpdaterInstallTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source_updater = updater_path()
        cls.temporary = tempfile.TemporaryDirectory(prefix="tclient-updater-actual-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.temp_root = Path(cls.temporary.name).resolve()
        cls.tools = cls.temp_root / "fixture tools"
        cls.tools.mkdir()
        cls.updater = cls.tools / "bootstrap updater.exe"
        shutil.copy2(cls.source_updater, cls.updater)
        cls.helper_sha256 = file_sha256(cls.updater)
        cls.source_sha256 = {
            str(path): file_sha256(path)
            for path in (
                ROOT / "src" / "tools" / "tclient_updater.cpp",
                ROOT / "src" / "engine" / "client" / "client.cpp",
                ROOT / "src" / "game" / "client" / "components" / "tclient" / "tclient.cpp",
                Path(__file__).resolve(),
            )
        }
        cls.results: list[dict] = []
        cls.evidence = Path(os.environ["TCLIENT_UPDATER_TEST_EVIDENCE"]).resolve() if os.environ.get("TCLIENT_UPDATER_TEST_EVIDENCE") else None
        if cls.evidence:
            cls.evidence.mkdir(parents=True, exist_ok=True)
        source = cls.tools / "fixture.cpp"
        source.write_text(FIXTURE_SOURCE, encoding="utf-8")
        names = ("healthy", "failing", "old_client", "runner", "locker", "ready_exit", "timeout_client", "rollback_blocker")
        for kind, name in enumerate(names, start=1):
            output = cls.tools / (name + ".exe")
            setattr(cls, name, output)
            compile_fixture(source, output, kind)

        cls.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        cls.kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        cls.kernel32.OpenProcess.restype = wintypes.HANDLE
        cls.kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        cls.kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
        cls.kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        cls.kernel32.TerminateProcess.restype = wintypes.BOOL
        cls.kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        cls.kernel32.WaitForSingleObject.restype = wintypes.DWORD
        cls.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        cls.kernel32.CloseHandle.restype = wintypes.BOOL

    def setUp(self) -> None:
        self.processes: dict[int, tuple[int, Path]] = {}
        self.events: dict[Path, Path] = {}
        self.children: list[subprocess.Popen] = []
        self.record: dict = {"test": self._testMethodName, "passed": False, "tracked_processes": []}
        self.case = self.temp_root / self._testMethodName
        self.target = self.case / "安装 target with spaces"
        self.target.mkdir(parents=True)
        self.archive = self.case / "update package.zip"
        self.log = self.target / "update" / "updater.log"
        self.old_event = self.event("old-client.pid", self.target / "DDNet.exe")
        self.new_event = self.event("new-client.pid", self.target / "DDNet.exe")
        self.environment = {"TCLIENT_FIXTURE_OLD_EVENT": str(self.old_event), "TCLIENT_FIXTURE_NEW_EVENT": str(self.new_event)}

    def tearDown(self) -> None:
        errors = []
        try:
            self.collect_events()
        except Exception as error:
            errors.append(str(error))
        for pid, (handle, expected) in reversed(list(self.processes.items())):
            try:
                if handle and self.is_alive_handle(handle):
                    self.verify_image(handle, expected)
                    if not self.kernel32.TerminateProcess(handle, 0xE0F1):
                        raise OSError(ctypes.get_last_error(), f"fixture termination failed: {pid}")
                    if self.kernel32.WaitForSingleObject(handle, 5000) != WAIT_OBJECT_0:
                        raise AssertionError(f"fixture did not exit: {pid}")
            except Exception as error:
                errors.append(str(error))
            finally:
                if handle:
                    self.kernel32.CloseHandle(handle)
        for child in self.children:
            try:
                child.wait(timeout=1)
            except subprocess.TimeoutExpired:
                errors.append(f"owned child still running after cleanup: {child.pid}")
        self.record["cleanup_errors"] = errors
        if errors:
            self.record["passed"] = False
        self.results.append(self.record)
        if self.evidence:
            destination = self.evidence / self._testMethodName
            destination.mkdir(exist_ok=True)
            for source in (self.log, self.case / "helper-output.log"):
                if source.is_file():
                    shutil.copy2(source, destination / source.name)
            summary = {
                "helper": str(self.source_updater),
                "helper_sha256": self.helper_sha256,
                "source_sha256": self.source_sha256,
                "helper_environment": {"TCLIENT_UPDATER_NO_UI": "1"},
                "temp_root": str(self.temp_root),
                "tests": self.results,
            }
            (self.evidence / "results.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
        if errors:
            self.fail("fixture cleanup errors:\n" + "\n".join(errors))

    def is_alive_handle(self, handle: int) -> bool:
        result = self.kernel32.WaitForSingleObject(handle, 0)
        if result not in (WAIT_OBJECT_0, WAIT_TIMEOUT):
            raise OSError(ctypes.get_last_error(), "cannot query fixture process handle")
        return result == WAIT_TIMEOUT

    def verify_image(self, handle: int, expected: Path) -> None:
        expected = expected.resolve()
        if not expected.is_relative_to(self.temp_root):
            raise AssertionError(f"refusing to control an executable outside test TEMP: {expected}")
        size = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(size.value)
        if not self.kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            if not self.is_alive_handle(handle):
                return
            raise OSError(ctypes.get_last_error(), "cannot identify fixture process")
        actual = os.path.normcase(os.path.abspath(buffer.value))
        if actual != os.path.normcase(str(expected)):
            raise AssertionError(f"refusing to control unrelated PID: expected {expected}, got {actual}")

    def remember(self, pid: int, expected: Path) -> None:
        if pid in self.processes:
            return
        handle = self.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, False, pid)
        if not handle:
            if ctypes.get_last_error() != ERROR_INVALID_PARAMETER:
                raise OSError(ctypes.get_last_error(), f"OpenProcess({pid}) failed")
        else:
            try:
                if self.is_alive_handle(handle):
                    self.verify_image(handle, expected)
            except Exception:
                self.kernel32.CloseHandle(handle)
                raise
        self.processes[pid] = (handle, expected.resolve())
        self.record["tracked_processes"].append({"pid": pid, "executable": str(expected)})

    def alive(self, pid: int) -> bool:
        handle = self.processes[pid][0]
        return bool(handle and self.is_alive_handle(handle))

    def event(self, name: str, expected: Path) -> Path:
        path = self.case / name
        self.events[path] = expected
        return path

    def collect_events(self) -> None:
        for event, expected in self.events.items():
            if event.is_file():
                try:
                    pid = int(event.read_text(encoding="ascii").strip())
                except (OSError, ValueError):
                    continue
                self.remember(pid, expected)

    def event_pid(self, event: Path) -> int:
        pid = wait_for_pid_event(event)
        self.remember(pid, self.events[event])
        return pid

    def start(self, executable: Path, arguments: list[str], environment: dict[str, str] | None = None) -> subprocess.Popen:
        env = os.environ.copy()
        for name in READY_NAMES:
            env.pop(name, None)
        if environment:
            env.update(environment)
        process = subprocess.Popen([str(executable), *arguments], cwd=self.case, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)
        self.children.append(process)
        self.remember(process.pid, executable)
        return process

    def start_runner(self, executable: Path, event: Path, child: Path | None = None, child_event: Path | None = None) -> int:
        arguments = [str(event)]
        if child:
            arguments += [str(child), str(child_event)]
        process = self.start(executable, arguments)
        self.assertEqual(self.event_pid(event), process.pid)
        return process.pid

    def install_old(self) -> str:
        shutil.copy2(self.old_client, self.target / "DDNet.exe")
        return file_sha256(self.target / "DDNet.exe")

    def run_updater(self, executable: Path | None = None, sha256: str | None = None, timeout_ms: int = 3000, restart: Path | None = None, with_console: bool = False) -> int:
        self.log.parent.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        for name in READY_NAMES:
            env.pop(name, None)
        env.update(self.environment)
        env["TCLIENT_UPDATER_WAIT_TIMEOUT_MS"] = "1000"
        env["TCLIENT_UPDATER_HEALTH_TIMEOUT_MS"] = str(timeout_ms)
        env["TCLIENT_UPDATER_NO_UI"] = "1"
        command = [str(executable or self.updater), "--pid", "0", "--zip", str(self.archive), "--sha256", sha256 or file_sha256(self.archive), "--target", str(self.target), "--restart", str(restart or self.target / "DDNet.exe"), "--log", str(self.log)]
        capture = self.case / "helper-output.log"
        self.record["command"] = command
        self.record["cwd"] = str(self.target)
        startup = None
        if with_console:
            env["TCLIENT_FIXTURE_CONSOLE_REPORT"] = "1"
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
        started = time.monotonic()
        with capture.open("wb") as stream:
            process = subprocess.Popen(command, cwd=self.target, env=env, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT, creationflags=CREATE_NEW_CONSOLE if with_console else CREATE_NO_WINDOW, startupinfo=startup)
            self.record["updater_pid"] = process.pid
            self.record["with_console"] = with_console
            try:
                # Wait for the helper handle only. Its surviving client may inherit standard
                # handles on Windows; reading PIPE to EOF would wait for that client too.
                code = process.wait(timeout=40)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                self.record["timeout"] = True
                raise AssertionError(f"helper timed out; output: {capture.read_text(encoding='utf-8', errors='replace')}")
            finally:
                self.record["elapsed_seconds"] = round(time.monotonic() - started, 3)
                self.record["returncode"] = process.returncode
                self.collect_events()
        return code

    def log_text(self) -> str:
        return self.log.read_text(encoding="utf-8", errors="replace") if self.log.is_file() else ""

    def assert_code(self, actual: int, expected: int) -> str:
        log = self.log_text()
        self.assertEqual(actual, expected, log)
        return log

    def assert_recovered(self, original_hash: str) -> None:
        pid = self.event_pid(self.old_event)
        self.assertTrue(self.alive(pid), "old client did not remain running")
        self.assertEqual(file_sha256(self.target / "DDNet.exe"), original_hash)
        self.assertTrue(self.archive.exists(), "failed update must preserve package")
        self.assertTrue(list(self.target.glob("update_backup_*")), "failed update must preserve backup")
        self.assertFalse(list(self.target.glob(".tclient-update-ready-*.tmp")), "health marker must be removed")

    def passed(self, **observations) -> None:
        self.record.update(observations)
        self.record["passed"] = True

    def test_normal_success(self) -> None:
        self.install_old()
        write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes()), ("data/版本.txt", b"new data")])
        log = self.assert_code(self.run_updater(), 0)
        self.assertTrue(self.alive(self.event_pid(self.new_event)))
        self.assertFalse(self.old_event.exists())
        self.assertFalse(self.archive.exists(), "mapped package must be closed and deleted")
        self.assertEqual(file_sha256(self.target / "DDNet.exe"), file_sha256(self.healthy))
        self.assertEqual((self.target / "data" / "版本.txt").read_bytes(), b"new data")
        self.assertIn("startup health ready marker accepted", log)
        self.assertFalse(list(self.target.glob(".tclient-update-ready-*.tmp")))
        self.passed(zip_deleted=True, new_client_alive=True)

    def assert_client_has_no_console(self, event: Path) -> None:
        pid = self.event_pid(event)
        self.assertTrue(self.alive(pid), "client must survive the updater")
        # Confirm the fixture really uses DDNet's Windows GUI subsystem.
        image = self.events[event].read_bytes()
        pe_offset = int.from_bytes(image[0x3C:0x40], "little")
        self.assertEqual(int.from_bytes(image[pe_offset + 92:pe_offset + 94], "little"), 2)
        # run_updater already waited for the helper handle. Ask the still-running
        # client to sample its console now, without attaching the test runner.
        Path(str(event) + ".probe").touch()
        snapshots = {}
        for suffix in (".console.json", ".after.json"):
            path = Path(str(event) + suffix)
            deadline = time.monotonic() + 5
            while True:
                try:
                    snapshots[suffix] = json.loads(path.read_text(encoding="ascii"))
                    break
                except (OSError, json.JSONDecodeError):
                    if time.monotonic() >= deadline:
                        self.fail(f"client did not report console state: {path}")
                    time.sleep(0.01)
        self.record["client_console"] = {"pid": pid, **snapshots}
        initial = snapshots[".console.json"]
        after_exit = snapshots[".after.json"]
        self.assertTrue(self.alive(pid), "console probe must leave the client running")
        self.assertEqual(after_exit["pids"], [], "client retained a console after updater exit")
        self.assertEqual(after_exit["window"], 0)
        self.assertIn("--silent", initial["argv"])
        self.assertTrue(initial["silent"])
        self.assertFalse(initial["attach_attempted"])
        self.assertFalse(initial["attached"])
        self.assertEqual(initial["before"]["pids"], [], "client inherited its parent's console")
        self.assertEqual(initial["after"]["pids"], [])

    def test_success_releases_updater_console_while_client_lives(self) -> None:
        self.install_old()
        write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes())])
        log = self.assert_code(self.run_updater(with_console=True), 0)
        self.assertIn("startup health ready marker accepted", log)
        self.assertFalse(self.old_event.exists())
        self.assert_client_has_no_console(self.new_event)
        self.passed(new_client_alive=True, client_console_detached=True)

    def make_junction(self, link: Path, destination: Path) -> None:
        self.assertTrue(link.absolute().is_relative_to(self.temp_root))
        self.assertTrue(destination.resolve().is_relative_to(self.temp_root))
        link.parent.mkdir(parents=True, exist_ok=True)
        batch = self.tools / "create-test-junction.cmd"
        batch.write_text(
            f'@echo off\nchcp 65001 >nul\nmklink /J "{link}" "{destination}"\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            ["cmd.exe", "/d", "/c", f".\\{batch.name}"],
            cwd=self.tools, capture_output=True, timeout=10, creationflags=CREATE_NO_WINDOW,
        )
        self.assertEqual(result.returncode, 0, repr(result.stdout + result.stderr))
        self.assertEqual(link.resolve(), destination.resolve())
        self.addCleanup(lambda: link.rmdir() if link.is_junction() else None)

    def test_staging_root_junction_preserves_external_files(self) -> None:
        self.staging_junction_cleanup(root_link=True)

    def test_staging_child_junction_preserves_external_files(self) -> None:
        self.staging_junction_cleanup(root_link=False)

    def staging_junction_cleanup(self, root_link: bool) -> None:
        self.install_old()
        outside = self.case / "outside staging"
        outside.mkdir()
        sentinel = outside / "must-survive.txt"
        sentinel.write_bytes(b"outside staging sentinel")
        staging = self.target / "update_staging_0"
        link = staging if root_link else staging / "linked directory"
        self.make_junction(link, outside)
        write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes()), ("payload.txt", b"new payload")])
        self.assert_code(self.run_updater(), 0)
        self.assertEqual(sentinel.read_bytes(), b"outside staging sentinel")
        self.assertEqual(sorted(path.name for path in outside.iterdir()), [sentinel.name])
        self.assertEqual((self.target / "payload.txt").read_bytes(), b"new payload")
        self.assertFalse(staging.exists())
        self.assertTrue(self.alive(self.event_pid(self.new_event)))
        self.passed(root_link=root_link, outside_sentinel_preserved=True)

    def test_backup_pruning_preserves_external_junction_targets(self) -> None:
        self.install_old()
        outside = self.case / "outside old backups"
        outside.mkdir()
        sentinel = outside / "must-survive.txt"
        sentinel.write_bytes(b"outside backup sentinel")
        root_backup = self.target / "update_backup_00000000_000000"
        child_backup = self.target / "update_backup_00000000_000001"
        self.make_junction(root_backup, outside)
        self.make_junction(child_backup / "linked directory", outside)
        (child_backup / "ordinary.txt").write_bytes(b"discard old backup")
        write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes())])
        self.assert_code(self.run_updater(), 0)
        self.assertEqual(sentinel.read_bytes(), b"outside backup sentinel")
        self.assertEqual(sorted(path.name for path in outside.iterdir()), [sentinel.name])
        self.assertFalse(root_backup.exists())
        self.assertFalse(child_backup.exists())
        self.assertEqual(len(list(self.target.glob("update_backup_*"))), 1)
        self.passed(outside_sentinel_preserved=True, root_and_child_backup_pruned=True)

    def test_installation_parent_junction_is_rejected(self) -> None:
        original_hash = self.install_old()
        outside = self.case / "outside installation"
        outside.mkdir()
        sentinel = outside / "must-survive.txt"
        sentinel.write_bytes(b"outside installation sentinel")
        self.make_junction(self.target / "data", outside)
        write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes()), ("data/must-survive.txt", b"replacement")])
        log = self.assert_code(self.run_updater(), 7)
        self.assertIn("refusing to follow reparse point", log)
        self.assertEqual(sentinel.read_bytes(), b"outside installation sentinel")
        self.assertEqual(file_sha256(self.target / "DDNet.exe"), original_hash)
        self.assertTrue(self.archive.exists())
        self.assertTrue(self.alive(self.event_pid(self.old_event)))
        self.assertFalse(list(self.target.glob("update_backup_*")))
        self.passed(outside_sentinel_preserved=True, installation_rejected_before_backup=True)

    def test_existing_backup_junction_is_rejected(self) -> None:
        self.backup_junction_rejected(root_link=True)

    def test_existing_backup_child_junction_is_rejected(self) -> None:
        self.backup_junction_rejected(root_link=False)

    def backup_junction_rejected(self, root_link: bool) -> None:
        from datetime import datetime, timedelta

        original_hash = self.install_old()
        outside = self.case / "outside new backup"
        outside.mkdir()
        sentinel = outside / "must-survive.txt"
        sentinel.write_bytes(b"outside new backup sentinel")
        (self.target / "data").mkdir()
        (self.target / "data" / sentinel.name).write_bytes(b"old installed contents")
        # The helper uses local time to the second. Pre-create a bounded window of names
        # so the actual helper deterministically encounters a link, without a timing race.
        now = datetime.now()
        for offset in range(-5, 61):
            backup = self.target / ("update_backup_" + (now + timedelta(seconds=offset)).strftime("%Y%m%d_%H%M%S"))
            self.make_junction(backup if root_link else backup / "data", outside)
        write_zip(self.archive, [("data/must-survive.txt", b"replacement")])
        log = self.assert_code(self.run_updater(), 7)
        self.assertIn("refusing to follow reparse point", log)
        self.assertEqual(sentinel.read_bytes(), b"outside new backup sentinel")
        self.assertEqual(sorted(path.name for path in outside.iterdir()), [sentinel.name])
        self.assertEqual((self.target / "data" / sentinel.name).read_bytes(), b"old installed contents")
        self.assertEqual(file_sha256(self.target / "DDNet.exe"), original_hash)
        self.assertTrue(self.archive.exists())
        self.assertTrue(self.alive(self.event_pid(self.old_event)))
        self.passed(root_link=root_link, outside_sentinel_preserved=True, backup_write_rejected=True)

    def test_success_closes_exact_target_processes_only_and_media_race(self) -> None:
        names = ("DDNet.exe", "DDNet-Server.exe", "island_media_monitor.exe", "netease-watcher.exe", "TClientUpdater.exe.old_fixture")
        for name in names:
            shutil.copy2(self.runner, self.target / name)
        target_pids = []
        for label, name in (("first", "DDNet.exe"), ("second", "DDNet.exe"), ("server", "DDNet-Server.exe"), ("stale-helper", "TClientUpdater.exe.old_fixture")):
            event = self.event(label + ".pid", self.target / name)
            target_pids.append(self.start_runner(self.target / name, event))
        media_event = self.event("media.pid", self.target / "island_media_monitor.exe")
        watcher_event = self.event("late-watcher.pid", self.target / "netease-watcher.exe")
        target_pids.append(self.start_runner(self.target / "island_media_monitor.exe", media_event, self.target / "netease-watcher.exe", watcher_event))
        self.assertFalse(watcher_event.exists(), "race child must start only during shutdown")
        surviving_pids = []
        for label, directory, name in (("prefix-sibling", Path(str(self.target) + "-old"), "DDNet.exe"), ("nested", self.target / "other installation", "netease-watcher.exe"), ("unplanned", self.target, "unplanned.exe")):
            directory.mkdir(exist_ok=True)
            executable = directory / name
            shutil.copy2(self.runner, executable)
            event = self.event(label + ".pid", executable)
            surviving_pids.append(self.start_runner(executable, event))
        entries = [("DDNet.exe", self.healthy.read_bytes())]
        entries += [(name, self.runner.read_bytes()) for name in names[1:4]]
        entries += [("TClientUpdater.exe", self.runner.read_bytes())]
        write_zip(self.archive, entries)
        log = self.assert_code(self.run_updater(), 0)
        target_pids.append(self.event_pid(watcher_event))
        self.assertTrue(all(not self.alive(pid) for pid in target_pids))
        self.assertTrue(all(self.alive(pid) for pid in surviving_pids))
        self.assertTrue(self.alive(self.event_pid(self.new_event)))
        self.assertFalse(self.archive.exists())
        self.assertGreaterEqual(log.count("target process sweep"), 2)
        for pid in target_pids:
            self.assertIn(f"pid={pid}", log)
        self.passed(closed_pids=target_pids, surviving_pids=surviving_pids, late_child_pid=target_pids[-1])

    def replacement_failure(self, fallback_position: str | None) -> None:
        old_hash = self.install_old()
        blocked = self.target / "blocked.bin"
        blocked.write_bytes(b"old blocked contents")
        event = self.event("locker.pid", self.locker)
        locker = self.start(self.locker, [str(blocked), str(event)])
        self.assertEqual(self.event_pid(event), locker.pid)
        entries = [("DDNet.exe", self.healthy.read_bytes()), ("blocked.bin", b"new blocked contents")]
        executable = None
        helper_hash = None
        if fallback_position:
            executable = self.target / "TClientUpdater.exe"
            shutil.copy2(self.updater, executable)
            helper_hash = file_sha256(executable)
            helper_entry = ("TClientUpdater.exe", self.runner.read_bytes())
            if fallback_position == "before":
                entries.insert(0, helper_entry)
            else:
                entries.append(helper_entry)
        write_zip(self.archive, entries)
        log = self.assert_code(self.run_updater(executable), 5)
        self.assert_recovered(old_hash)
        self.assertEqual(blocked.read_bytes(), b"old blocked contents")
        self.assertIn("could not replace", log)
        self.assertIn("previous state restored", log)
        if executable:
            self.assertEqual(file_sha256(executable), helper_hash)
            self.assertIn("restored fallback helper", log)
        self.passed(fallback_position=fallback_position, fallback_hash_restored=helper_hash)

    def test_replacement_failure_rolls_back_and_restarts(self) -> None:
        self.replacement_failure(None)

    def test_fallback_replaced_helper_restored_on_later_failure(self) -> None:
        self.replacement_failure("before")

    def test_fallback_rename_restored_before_helper_replacement(self) -> None:
        self.replacement_failure("after")

    def health_failure(self, fixture: Path, with_console: bool = False) -> tuple[int, str]:
        old_hash = self.install_old()
        (self.target / "version.dat").write_bytes(b"old data")
        entries = [("DDNet.exe", fixture.read_bytes()), ("version.dat", b"new data")]
        if fixture == self.timeout_client:
            entries.append(("netease-watcher.exe", self.runner.read_bytes()))
        write_zip(self.archive, entries)
        log = self.assert_code(self.run_updater(with_console=with_console), 9)
        new_pid = self.event_pid(self.new_event)
        self.assertFalse(self.alive(new_pid))
        self.assert_recovered(old_hash)
        self.assertEqual((self.target / "version.dat").read_bytes(), b"old data")
        return new_pid, log

    def test_immediate_new_client_exit_rolls_back_and_restarts_old_client(self) -> None:
        new_pid, log = self.health_failure(self.failing)
        self.assertIn("code 23", log)
        self.passed(failed_new_pid=new_pid)

    def test_rollback_releases_updater_console_while_old_client_lives(self) -> None:
        new_pid, log = self.health_failure(self.failing, with_console=True)
        self.assertIn("code 23", log)
        self.assert_client_has_no_console(self.old_event)
        self.passed(failed_new_pid=new_pid, old_client_alive=True, client_console_detached=True)

    def test_ready_marker_then_immediate_exit_rolls_back(self) -> None:
        marker_event = self.event("marker-written.pid", self.target / "DDNet.exe")
        self.environment["TCLIENT_FIXTURE_MARKER_EVENT"] = str(marker_event)
        new_pid, log = self.health_failure(self.ready_exit)
        self.assertEqual(self.event_pid(marker_event), new_pid)
        self.assertIn("code 24", log)
        self.assertNotIn("ready marker accepted", log)
        self.passed(marker_was_written=True, failed_new_pid=new_pid)

    def test_health_timeout_stops_new_client_and_child_then_rolls_back(self) -> None:
        child = self.target / "netease-watcher.exe"
        child_event = self.event("new-child.pid", child)
        self.environment.update({"TCLIENT_FIXTURE_CHILD_EXE": str(child), "TCLIENT_FIXTURE_CHILD_EVENT": str(child_event)})
        new_pid, log = self.health_failure(self.timeout_client)
        child_pid = self.event_pid(child_event)
        self.assertFalse(self.alive(child_pid))
        self.assertIn("did not report startup health", log)
        self.assertFalse(child.exists(), "newly installed executable must be removed on rollback")
        self.passed(timed_out_new_pid=new_pid, stopped_child_pid=child_pid)

    def test_missing_zip_restarts_old_client(self) -> None:
        old_hash = self.install_old()
        code = self.run_updater(sha256="0" * 64)
        self.assertNotEqual(code, 0, self.log_text())
        self.assertTrue(self.alive(self.event_pid(self.old_event)), self.log_text())
        self.assertEqual(file_sha256(self.target / "DDNet.exe"), old_hash)
        self.passed(missing_zip_recovery_code=code)

    def test_missing_zip_does_not_duplicate_running_old_client(self) -> None:
        self.install_old()
        old = self.start(self.target / "DDNet.exe", [], self.environment)
        self.assertEqual(self.event_pid(self.old_event), old.pid)
        code = self.run_updater(sha256="0" * 64)
        self.assertNotEqual(code, 0, self.log_text())
        self.assertEqual(self.event_pid(self.old_event), old.pid)
        self.assertTrue(self.alive(old.pid))
        self.assertIn("already running", self.log_text())
        self.passed(existing_old_pid=old.pid)

    def test_cross_install_restart_is_rejected_without_changing_target(self) -> None:
        old_hash = self.install_old()
        other = Path(str(self.target) + "-other")
        other.mkdir()
        executable = other / "DDNet.exe"
        shutil.copy2(self.runner, executable)
        other_event = self.event("other-install.pid", executable)
        other_pid = self.start_runner(executable, other_event)
        write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes())])
        code = self.run_updater(restart=executable)
        self.assertEqual(code, 1, self.log_text())
        self.assertEqual(file_sha256(self.target / "DDNet.exe"), old_hash)
        self.assertTrue(self.alive(other_pid))
        self.assertFalse(self.new_event.exists())
        self.assertFalse(self.old_event.exists())
        self.assertTrue(self.archive.exists())
        self.assertFalse(list(self.target.glob("update_backup_*")))
        self.passed(cross_install_restart_rejected=True, other_install_pid=other_pid)

    def test_drive_root_restart_is_rejected_without_changing_target(self) -> None:
        self.install_old()
        (self.target / "version.dat").write_bytes(b"old data")
        # Keep the test log outside the target so every target file can be
        # compared before and after rejecting the drive-root executable path.
        self.log = self.case / "updater.log"
        original_files = {
            str(path.relative_to(self.target)): file_sha256(path)
            for path in self.target.rglob("*")
            if path.is_file()
        }
        package_hash = write_zip(self.archive, [("DDNet.exe", self.healthy.read_bytes()), ("version.dat", b"new data")])
        root_executable = Path(self.target.anchor) / "DDNet.exe"
        # Do not create or launch anything at the drive root. Containment must
        # be rejected before existence checks while the helper cwd is target.
        code = self.run_updater(restart=root_executable)
        self.assertEqual(code, 1, self.log_text())
        self.assertEqual(self.record["cwd"], str(self.target))
        self.assertEqual(
            {
                str(path.relative_to(self.target)): file_sha256(path)
                for path in self.target.rglob("*")
                if path.is_file()
            },
            original_files,
        )
        self.assertEqual(file_sha256(self.archive), package_hash)
        self.assertFalse(self.new_event.exists())
        self.assertFalse(self.old_event.exists())
        self.assertFalse(list(self.target.glob("update_backup_*")))
        self.assertFalse(list(self.target.glob("update_staging_*")))
        self.passed(drive_root_restart_rejected=True, rejected_restart=str(root_executable), target_files_unchanged=original_files)

    def test_incomplete_rollback_does_not_restart_old_client(self) -> None:
        self.install_old()
        locked = self.target / "version.dat"
        locked.write_bytes(b"old data")
        lock_event = self.event("rollback-locker.pid", self.locker)
        self.environment.update({"TCLIENT_FIXTURE_LOCKER_EXE": str(self.locker), "TCLIENT_FIXTURE_LOCK_PATH": str(locked), "TCLIENT_FIXTURE_LOCK_EVENT": str(lock_event)})
        write_zip(self.archive, [("DDNet.exe", self.rollback_blocker.read_bytes()), ("version.dat", b"new data")])
        code = self.run_updater()
        log = self.assert_code(code, 10)
        self.assertTrue(self.alive(self.event_pid(lock_event)), "rollback lock fixture did not survive long enough")
        self.assertFalse(self.alive(self.event_pid(self.new_event)))
        self.assertIn("rollback", log)
        self.assertIn("incomplete", log.lower())
        # Give an erroneous restart enough time to reach the old-client entrypoint.
        deadline = time.monotonic() + 0.5
        while not self.old_event.exists() and time.monotonic() < deadline:
            time.sleep(0.025)
        self.collect_events()
        self.assertFalse(self.old_event.exists(), "incomplete rollback must not launch a mixed installation\n" + log)
        self.assertTrue(self.archive.exists())
        backups = list(self.target.glob("update_backup_*/version.dat"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b"old data")
        self.assertEqual(locked.read_bytes(), b"new data", "fixture must really obstruct rollback")
        self.passed(incomplete_rollback_code=code, restart_suppressed=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
