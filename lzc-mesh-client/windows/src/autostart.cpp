#include "app.h"
#include "autostart.h"

namespace autostart {
namespace {
const wchar_t *kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t *kValue = L"LZC Mesh";

std::wstring command() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return L"\"" + std::wstring(exe) + L"\" --background";
}
}  // namespace

bool enabled() {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, kKey, kValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

bool set(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG r;
    if (on) {
        std::wstring cmd = command();
        r = RegSetValueExW(k, kValue, 0, REG_SZ, (const BYTE *)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, kValue);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
}  // namespace autostart
