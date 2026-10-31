#include "app.h"
#include "tray.h"

#include <shellapi.h>

#include "autostart.h"

namespace tray {
namespace {
NOTIFYICONDATAW nid{};
HICON iconOn, iconOff;
bool added = false;

// Two small screens drawn in code: solid when sharing and connected, grey otherwise.
// Drawn at the shell's small-icon size so it stays sharp on scaled displays.
bool lightTaskbar() {
    DWORD v = 0, size = sizeof v;
    // SystemUsesLightTheme = 1: taskbar and tray are light (Windows 10 1903+ / 11).
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 1;
}

HICON makeIcon(bool on) {
    int s = GetSystemMetrics(SM_CXSMICON);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, color);
    memset(bits, 0, (size_t)s * s * 4);
    bool light = lightTaskbar();
    COLORREF ink = on ? (light ? RGB(17, 17, 17) : RGB(255, 255, 255)) : (light ? RGB(150, 150, 150) : RGB(140, 140, 140));
    HPEN pen = CreatePen(PS_SOLID, std::max(1, s / 10), ink);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int u = s;
    RoundRect(dc, u * 1 / 16, u * 2 / 16, u * 11 / 16, u * 9 / 16, u / 6, u / 6);
    RoundRect(dc, u * 5 / 16, u * 7 / 16, u * 15 / 16, u * 14 / 16, u / 6, u / 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    // GDI does not write alpha: make every drawn pixel opaque.
    auto *px = (DWORD *)bits;
    for (int i = 0; i < s * s; i++) if (px[i] & 0x00FFFFFF) px[i] |= 0xFF000000;
    SelectObject(dc, old);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

std::wstring tooltip() {
    std::wstring t = L"懒猫设备接力";
    if (!g.hubConnected) t += L"\n未连接";
    else if (!g.sharingEnabled) t += L"\n键鼠共享已暂停";
    else if (g.captured || g.controlled) t += g.captured ? L"\n正在控制其他设备" : L"\n正在被控制";
    else {
        int online = 0;
        for (auto &d : g.devices) online += d.online;
        t += fmt(L"\n已连接 · %d 台设备在线", online);
    }
    return t.substr(0, 127);
}
}  // namespace

void add(HWND owner) {
    if (!iconOn) iconOn = makeIcon(true), iconOff = makeIcon(false);
    nid = NOTIFYICONDATAW{};
    nid.cbSize = sizeof nid;
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g.hubConnected && g.sharingEnabled ? iconOn : iconOff;
    wcsncpy(nid.szTip, tooltip().c_str(), 127);
    added = Shell_NotifyIconW(NIM_ADD, &nid);
    if (!added) logLine(fmt(L"托盘图标添加失败（%lu），资源管理器启动后会自动重试", GetLastError()));
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void remove() {
    if (added) Shell_NotifyIconW(NIM_DELETE, &nid);
    added = false;
}

void refresh() {
    if (!added) return;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = g.hubConnected && g.sharingEnabled ? iconOn : iconOff;
    wcsncpy(nid.szTip, tooltip().c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void onTaskbarCreated(HWND owner) {
    added = false;
    add(owner);
}

void onThemeChanged() {
    // Light / dark taskbar switched: redraw both icon variants.
    if (iconOn) DestroyIcon(iconOn), DestroyIcon(iconOff);
    iconOn = makeIcon(true);
    iconOff = makeIcon(false);
    refresh();
}

void showMenu(HWND owner) {
    HMENU m = CreatePopupMenu();
    // First line: connection status only; the control line below covers control state.
    std::wstring status;
    if (!g.hubConnected) status = L"未连接";
    else if (!g.sharingEnabled) status = L"已连接 · 键鼠共享已暂停";
    else {
        int online = 0;
        for (auto &d : g.devices) online += d.online;
        status = fmt(L"已连接 · %d 台设备在线", online);
    }
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
    if (g.captured || g.controlled) {
        std::wstring who;
        for (auto &d : g.devices) if (d.id == (g.captured ? g.activeTarget : g.activeSource)) who = widen(d.name);
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (g.captured ? L"正在控制 " + who : who + L" 正在控制本机").c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.sharingEnabled ? MF_CHECKED : 0), ID_TRAY_SHARING, L"键鼠共享");
    AppendMenuW(m, MF_STRING | (g.clipboardEnabled ? MF_CHECKED : 0) | (g.hubConnected ? 0 : MF_GRAYED), ID_TRAY_CLIPBOARD, L"共享剪贴板");
    AppendMenuW(m, MF_STRING | (autostart::enabled() ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_SHOW, L"打开主窗口");
    AppendMenuW(m, MF_STRING | (g.boxDomain.empty() ? MF_GRAYED : 0), ID_TRAY_OPEN, L"打开管理页面");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"退出懒猫设备接力");
    SetMenuDefaultItem(m, ID_TRAY_SHOW, FALSE);
    POINT p;
    GetCursorPos(&p);
    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(owner);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, p.x, p.y, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
}
}  // namespace tray
