// Portable private-test launcher: access code entered at runtime, never bundled.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
HWND Edit = nullptr;
std::wstring Root;

bool Prepare()
{
	const auto profile = fs::path(Root) / L"test-profile";
	if(!fs::is_regular_file(fs::path(Root) / L"DDNet.exe") || !fs::is_directory(fs::path(Root) / L"data/mapres"))
		return false;
	fs::create_directories(profile);
	std::ofstream storage(fs::path(Root) / L"storage.cfg", std::ios::binary | std::ios::trunc);
	const std::wstring path = profile.generic_wstring();
	const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), int(path.size()), nullptr, 0, nullptr, nullptr);
	if(size <= 0)
		return false;
	std::string utf8(size, '\0');
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), int(path.size()), utf8.data(), size, nullptr, nullptr);
	storage << "add_path " << utf8 << "\nadd_path $DATADIR\nadd_path $CURRENTDIR\n";
	storage.close();
	return bool(storage);
}

bool Valid(const std::wstring &key)
{
	return key.size() == 64 && std::all_of(key.begin(), key.end(), [](wchar_t c) {
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
	});
}

bool Launch(std::wstring &key)
{
	if(!Valid(key) || !Prepare())
		return false;
	std::vector<std::wstring> environment;
	LPWCH existing = GetEnvironmentStringsW();
	if(!existing)
		return false;
	for(const wchar_t *p = existing; *p; p += wcslen(p) + 1)
	{
		std::wstring entry(p);
		if(_wcsnicmp(entry.c_str(), L"CF_WS_PROTOTYPE_KEY=", 20) != 0)
			environment.push_back(std::move(entry));
	}
	FreeEnvironmentStringsW(existing);
	environment.push_back(L"CF_WS_PROTOTYPE_KEY=" + key);
	std::sort(environment.begin(), environment.end(), [](const auto &a, const auto &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
	std::vector<wchar_t> block;
	for(const auto &entry : environment)
	{
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	const auto exe = (fs::path(Root) / L"DDNet.exe").wstring();
	std::wstring command = L"\"" + exe + L"\" \"tc_relay_cf_only 1\" \"tc_relay_cf 1\" \"tc_relay_p2p 0\" \"me_steam_hide 1\"";
	STARTUPINFOW start{};
	start.cb = sizeof(start);
	PROCESS_INFORMATION process{};
	const bool ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
				block.data(), Root.c_str(), &start, &process) != FALSE;
	SecureZeroMemory(block.data(), block.size() * sizeof(wchar_t));
	for(auto &entry : environment)
		if(!entry.empty())
			SecureZeroMemory(entry.data(), entry.size() * sizeof(wchar_t));
	SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
	key.clear();
	if(ok)
	{
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
	return ok;
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
	if(message == WM_CREATE)
	{
		const auto font = GetStockObject(DEFAULT_GUI_FONT);
		HWND label = CreateWindowW(L"STATIC", L"请输入开发者私下提供的 CF 测试码（不会保存）", WS_CHILD | WS_VISIBLE,
			16, 16, 480, 22, window, nullptr, nullptr, nullptr);
		Edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
			16, 48, 460, 27, window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
		HWND button = CreateWindowW(L"BUTTON", L"启动 CF-only 测试版", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			240, 90, 236, 30, window, reinterpret_cast<HMENU>(2), nullptr, nullptr);
		for(HWND control : {label, Edit, button})
			SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		SendMessageW(Edit, EM_SETLIMITTEXT, 64, 0);
		SetFocus(Edit);
		return 0;
	}
	if(message == WM_COMMAND && LOWORD(w) == 2)
	{
		wchar_t buffer[65]{};
		GetWindowTextW(Edit, buffer, 65);
		std::wstring key(buffer);
		SecureZeroMemory(buffer, sizeof(buffer));
		if(!Valid(key))
		{
			if(!key.empty())
				SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
			MessageBoxW(window, L"测试码需为64位小写十六进制字符。", L"测试码格式错误", MB_OK | MB_ICONWARNING);
			return 0;
		}
		SetWindowTextW(Edit, L"");
		try
		{
			if(Launch(key))
			{
				DestroyWindow(window);
				return 0;
			}
		}
		catch(...)
		{
		}
		if(!key.empty())
			SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
		MessageBoxW(window, L"启动失败。请完整解压测试包，确保同目录有 DDNet.exe、DLL 和 data。", L"CF 测试版", MB_OK | MB_ICONERROR);
		return 0;
	}
	if(message == WM_DESTROY)
	{
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, w, l);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int)
{
	wchar_t module[32768]{};
	DWORD n = GetModuleFileNameW(nullptr, module, DWORD(std::size(module)));
	if(!n || n == std::size(module))
		return 2;
	Root = fs::path(module).parent_path().wstring();
	try
	{
		if(std::wstring(arguments) == L"--verify-package")
			return Prepare() ? 0 : 2;
		if(std::wstring(arguments) == L"--test-stdin")
		{
			char buffer[128]{};
			DWORD read = 0;
			if(!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer) - 1, &read, nullptr))
				return 2;
			std::wstring key;
			for(DWORD i = 0; i < read && buffer[i] != '\n' && buffer[i] != '\r'; ++i)
				key.push_back(wchar_t(buffer[i]));
			SecureZeroMemory(buffer, sizeof(buffer));
			return Launch(key) ? 0 : 2;
		}
	}
	catch(...)
	{
		return 2;
	}
	WNDCLASSW cls{};
	cls.lpfnWndProc = Proc;
	cls.hInstance = instance;
	cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	cls.lpszClassName = L"TClientCFPrivateTestLauncher";
	if(!RegisterClassW(&cls))
		return 2;
	HWND window = CreateWindowW(cls.lpszClassName, L"TClient CF 信令预发布测试 — 访问码", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 510, 165, nullptr, nullptr, instance, nullptr);
	if(!window)
		return 2;
	ShowWindow(window, SW_SHOW);
	MSG msg{};
	while(GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		if(!IsDialogMessageW(window, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	return 0;
}
