// 懒猫设备接力 (LZC Mesh) setup for Windows. A single self-contained exe: the client is embedded
// as an RCDATA resource. Installs per user (no admin): %LOCALAPPDATA%\Programs\LZC Mesh,
// Start menu + Startup shortcuts and an "Apps & features" entry. The installed copy of
// this program acts as the uninstaller.
//
//   lzc-mesh-windows-setup.exe            interactive install
//   lzc-mesh-windows-setup.exe /S         silent install
//   uninstall.exe /uninstall [/S]         (registered as the uninstaller)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <string>

namespace {
const wchar_t *kTitle = L"懒猫设备接力 安装";
const wchar_t *kName = L"懒猫设备接力";
const wchar_t *kVersion = L"0.2.1";
const wchar_t *kExeName = L"LZC Mesh.exe";
const wchar_t *kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LZCMesh";
bool silent = false;

std::wstring env(const wchar_t *name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : L"";
}
std::wstring installDir() { return env(L"LOCALAPPDATA") + L"\\Programs\\LZC Mesh"; }
std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
std::wstring startMenuLink() { return knownFolder(FOLDERID_Programs) + L"\\懒猫设备接力.lnk"; }
std::wstring startupLink() { return knownFolder(FOLDERID_Startup) + L"\\懒猫设备接力.lnk"; }
// Shortcut names used by v0.1.0; removed on upgrade and uninstall.
std::wstring oldStartMenuLink() { return knownFolder(FOLDERID_Programs) + L"\\LZC Mesh.lnk"; }
std::wstring oldStartupLink() { return knownFolder(FOLDERID_Startup) + L"\\LZC Mesh.lnk"; }

void message(const std::wstring &text, UINT icon = MB_ICONINFORMATION) {
    if (!silent) MessageBoxW(nullptr, text.c_str(), kTitle, MB_OK | icon);
}
bool fail(const std::wstring &what) {
    DWORD e = GetLastError();
    message(what + L"（错误 " + std::to_wstring(e) + L"）", MB_ICONERROR);
    return false;
}

// Closes a running client so its exe can be replaced.
void stopClient() {
    if (HWND w = FindWindowW(L"LzcMeshMain", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        // The client hides to the tray on WM_CLOSE; this registered message makes it exit.
        PostMessageW(w, RegisterWindowMessageW(L"LzcMeshQuit"), 0, 0);
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(p, 5000);
            CloseHandle(p);
        }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W e{sizeof e};
    DWORD session = 0, mine = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (_wcsicmp(e.szExeFile, kExeName) != 0) continue;
        if (!ProcessIdToSessionId(e.th32ProcessID, &session) || session != mine) continue;
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, e.th32ProcessID)) {
            TerminateProcess(p, 0);
            WaitForSingleObject(p, 3000);
            CloseHandle(p);
        }
    }
    CloseHandle(snap);
}

bool writeEmbeddedClient(const std::wstring &path) {
    HRSRC res = FindResourceW(nullptr, L"CLIENT", RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    const void *bytes = data ? LockResource(data) : nullptr;
    if (!bytes || !size) return false;
    std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes, size, &written, nullptr) && written == size;
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool createShortcut(const std::wstring &link, const std::wstring &target, const std::wstring &dir) {
    IShellLinkW *sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetDescription(kName);
    IPersistFile *pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && SUCCEEDED(pf->Save(link.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

void setString(HKEY k, const wchar_t *name, const std::wstring &v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void setDword(HKEY k, const wchar_t *name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v); }

bool install() {
    if (!silent && MessageBoxW(nullptr, (L"将懒猫设备接力 " + std::wstring(kVersion) + L" 安装到：\n" + installDir() +
                                         L"\n\n安装后会创建开始菜单快捷方式并随系统启动。是否继续？").c_str(),
                               kTitle, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return false;
    std::wstring dir = installDir(), exe = dir + L"\\" + kExeName, uninstaller = dir + L"\\uninstall.exe";
    stopClient();
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetLastError() != ERROR_ALREADY_EXISTS &&
        GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail(L"无法创建安装目录 " + dir);
    if (!writeEmbeddedClient(exe)) return fail(L"无法写入 " + exe + L"，请关闭正在运行的懒猫设备接力后重试");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (_wcsicmp(self, uninstaller.c_str()) != 0 && !CopyFileW(self, uninstaller.c_str(), FALSE)) return fail(L"无法写入卸载程序");
    DeleteFileW(oldStartMenuLink().c_str());
    DeleteFileW(oldStartupLink().c_str());
    if (!createShortcut(startMenuLink(), exe, dir) || !createShortcut(startupLink(), exe, dir)) return fail(L"无法创建快捷方式");

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        setString(k, L"DisplayName", kName);
        setString(k, L"DisplayVersion", kVersion);
        setString(k, L"Publisher", L"ponzS");
        setString(k, L"DisplayIcon", exe);
        setString(k, L"InstallLocation", dir);
        setString(k, L"URLInfoAbout", L"https://github.com/ponzS/lzc-mesh");
        setString(k, L"UninstallString", L"\"" + uninstaller + L"\" /uninstall");
        setString(k, L"QuietUninstallString", L"\"" + uninstaller + L"\" /uninstall /S");
        setDword(k, L"NoModify", 1);
        setDword(k, L"NoRepair", 1);
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &a)) setDword(k, L"EstimatedSize", a.nFileSizeLow / 1024 + 1);
        RegCloseKey(k);
    }
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    message(L"懒猫设备接力已安装并启动。\n\n可在开始菜单中找到它，也可在“设置 › 应用”中卸载。");
    return true;
}

bool uninstall() {
    if (!silent && MessageBoxW(nullptr, L"确定要卸载懒猫设备接力吗？\n\n设备 ID 和设置保存在 %USERPROFILE%\\.config\\lzc-mesh，不会被删除。",
                               L"卸载懒猫设备接力", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return false;
    std::wstring dir = installDir();
    stopClient();
    DeleteFileW(startMenuLink().c_str());
    DeleteFileW(startupLink().c_str());
    DeleteFileW(oldStartMenuLink().c_str());
    DeleteFileW(oldStartupLink().c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW((dir + L"\\" + kExeName).c_str());
    // This uninstaller is running from the install dir; remove it and the dir once it exits.
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + dir + L"\\uninstall.exe\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    message(L"懒猫设备接力已卸载。");
    return true;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool remove = false;
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/S") || !_wcsicmp(argv[i], L"--silent")) silent = true;
        if (!_wcsicmp(argv[i], L"/uninstall") || !_wcsicmp(argv[i], L"--uninstall")) remove = true;
    }
    LocalFree(argv);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = remove ? uninstall() : install();
    CoUninitialize();
    return ok ? 0 : 1;
}
