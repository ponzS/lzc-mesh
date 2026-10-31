// LZC Mesh for Windows: Win32 window with status panel, layout editor and log.
#include "app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "input.h"
#include "lzc.h"
#include "net.h"

App g;

namespace {
HFONT font, fontBold, fontSmall, fontMono;
HWND hStatusText[S_COUNT], hLayout, hLog, hBox, hSwap, hPermBtn, hOpenBtn;
UINT dpi = 96;
long long serviceCheckedAt = 0;
bool probing = false;
bool updatingSwap = false;
long long lastMoveLog = 0, lastKeyLog = 0;

enum { ID_RETRY = 100, ID_OPEN, ID_COPY, ID_QUIT, ID_BOX_APPLY, ID_SWAP, ID_ELEVATE, ID_TIMER = 1, ID_FG_TIMER = 2 };

const wchar_t *labels[S_COUNT] = {L"懒猫微服客户端", L"懒猫网络", L"登录会话", L"LZC Mesh 应用", L"中枢连接", L"输入权限", L"控制状态", L"本机屏幕"};

int S(int v) { return MulDiv(v, (int)dpi, 96); }

std::wstring configPath() {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    std::wstring dir = std::wstring(home, n) + L"\\.config\\lzc-mesh";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

void saveConfig() {
    std::string text = JsonOut().s("device_id", g.deviceId).s("box", g.boxOverride).str();
    HANDLE f = CreateFileW(configPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, text.data(), (DWORD)text.size(), &w, nullptr);
    CloseHandle(f);
}

void loadConfig() {
    HANDLE f = CreateFileW(configPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[4096];
        DWORD n = 0;
        ReadFile(f, buf, sizeof buf, &n, nullptr);
        CloseHandle(f);
        Json j;
        if (Json::parse(std::string(buf, n), j)) {
            g.deviceId = j["device_id"].str();
            g.boxOverride = j["box"].str();
        }
    }
    if (g.deviceId.empty()) {
        GUID id;
        CoCreateGuid(&id);
        char b[64];
        snprintf(b, sizeof b, "windows-%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1],
                 id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
        g.deviceId = b;
        saveConfig();
    }
}

const MeshDevice *deviceById(const std::string &id) {
    for (auto &d : g.devices) if (d.id == id) return &d;
    return nullptr;
}

void updateControlStatus() {
    auto name = [](const std::string &id) { auto *d = deviceById(id); return widen(d ? d->name : id); };
    if (g.captured) setStatus(S_CONTROL, L_OK, L"本机正在控制 " + name(g.activeTarget) + L"（Ctrl+Alt+Esc 收回）");
    else if (g.controlled) setStatus(S_CONTROL, L_OK, name(g.activeSource) + L" 正在控制本机");
    else setStatus(S_CONTROL, L_UNKNOWN, L"空闲：把光标移出屏幕边缘即可控制相邻设备");
}

void updateScreenStatus() {
    input::refreshScreen();
    setStatus(S_SCREEN, L_OK, fmt(L"%d×%d（%d 个显示器，缩放 %d%%，逻辑像素 %.0f×%.0f）", g.screenW, g.screenH, GetSystemMetrics(SM_CMONITORS),
                                  (int)std::lround(g.dpiScale * 100), g.screenW / g.dpiScale, g.screenH / g.dpiScale));
}

void sendHello() {
    wchar_t name[256];
    DWORD n = 256;
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n);
    hubws::send(JsonOut().s("t", "hello").s("id", g.deviceId).s("name", narrow(name)).s("platform", "windows")
                    .n("w", g.screenW / g.dpiScale).n("h", g.screenH / g.dpiScale).s("version", LZC_MESH_VERSION).str());
}

void startProbe() {
    if (probing) return;
    probing = true;
    bool check = !g.hubConnected || GetTickCount64() - serviceCheckedAt > 30000;
    lzcProbeAsync(check);
}

void onProbe(Probe *p) {
    probing = false;
    setStatus(S_LZC_CLIENT, p->client.first, p->client.second);
    setStatus(S_LZC_NETWORK, p->net.first, p->net.second);
    setStatus(S_LOGIN, p->login.first, p->login.second);
    if (p->hasService) {
        serviceCheckedAt = GetTickCount64();
        setStatus(S_SERVICE, p->service.first, p->service.second);
    }
    bool changed = g.boxDomain != p->boxDomain || g.cookie != p->cookie;
    g.boxName = p->boxName;
    g.boxDomain = p->boxDomain;
    g.loginUser = p->loginUser;
    g.cookie = p->cookie;
    bool ready = p->net.first == L_OK && p->login.first == L_OK && g.status[S_SERVICE].level == L_OK;
    if (changed && g.hubConnected) {
        logLine(L"懒猫微服或登录会话变化，重新连接");
        hubws::close();
    }
    if (ready && !g.hubConnected && !hubws::busy()) {
        setStatus(S_HUB, L_WARN, L"正在连接 wss://lzc-mesh." + widen(g.boxDomain) + L"/api/ws");
        hubws::connect(L"lzc-mesh." + widen(g.boxDomain), widen(g.cookie));
    }
    if (!ready && g.hubConnected) hubws::close();
    if (!ready) setStatus(S_HUB, L_UNKNOWN, L"等待 LZC Mesh 应用可用");
    EnableWindow(hOpenBtn, !g.boxDomain.empty());
    delete p;
}

void onHubText(const std::string &text) {
    Json o;
    if (!Json::parse(text, o) || !o.isObj()) return;
    std::string t = o["t"].str();
    double x = o["x"].num(0.5), y = o["y"].num(0.5);
    if (t == "welcome") {
        g.hubUser = o["user"].str();
        if (!o["id"].str().empty()) logLine(L"已加入 " + widen(g.hubUser) + L" 的设备组");
    } else if (t == "state") {
        g.devices.clear();
        int online = 0;
        for (auto &d : o["devices"].a) {
            MeshDevice m{d["id"].str(), d["name"].str(), d["platform"].str(), d["version"].str(), d["x"].num(), d["y"].num(), d["w"].num(), d["h"].num(), d["online"].boolean()};
            online += m.online;
            g.devices.push_back(m);
        }
        g.activeSource = o["active"]["source"].str();
        g.activeTarget = o["active"]["target"].str();
        g.swapCmdCtrl = o["settings"]["swap_cmd_ctrl"].boolean(true);
        setStatus(S_HUB, L_OK, fmt(L"已连接 · 用户 %ls · %d 台设备在线", widen(g.hubUser).c_str(), online));
        updateControlStatus();
        devicesChanged();
    } else if (t == "edges") {
        input::setEdges(o["left"].boolean(), o["right"].boolean(), o["up"].boolean(), o["down"].boolean());
    } else if (t == "capture") {
        g.captured = true;
        logLine(L"开始控制其他设备");
        input::capture(true);
        updateControlStatus();
    } else if (t == "release") {
        g.captured = false;
        input::capture(false);
        if (o["warp"].boolean()) input::warp(x, y);
        logLine(L"控制已收回本机");
        updateControlStatus();
    } else if (t == "enter") {
        g.controlled = true;
        logLine(fmt(L"被远程控制：光标进入本机 (%.2f, %.2f)", x, y));
        input::enter(x, y);
        updateControlStatus();
    } else if (t == "leave") {
        g.controlled = false;
        input::leave();
        logLine(L"远程控制结束");
        updateControlStatus();
    } else if (t == "move") {
        input::move(x, y);
        if (GetTickCount64() - lastMoveLog > 2000) {
            lastMoveLog = GetTickCount64();
            logLine(fmt(L"收到远程光标 (%.3f, %.3f)", x, y));
        }
    } else if (t == "button") {
        input::button((int)o["button"].num(1), o["down"].boolean());
    } else if (t == "wheel") {
        input::wheel(o["dx"].num(), o["dy"].num());
    } else if (t == "key") {
        uint32_t code = (uint32_t)o["code"].num();
        bool down = o["down"].boolean();
        logLine(fmt(L"收到按键 code=%u %ls", code, down ? L"按下" : L"抬起"));
        input::key(code, down);
    } else if (t == "error") {
        logLine(L"服务端错误：" + widen(o["message"].str()));
    }
}

void onHubClosed(int httpStatus, const std::wstring &why) {
    bool was = g.hubConnected;
    g.hubConnected = false;
    input::setHubConnected(false);
    if (was) {
        logLine(L"与 LZC Mesh 服务的连接已断开：" + why);
        setStatus(S_HUB, L_WARN, L"连接已断开，稍后自动重连");
    } else {
        logLine(L"连接 LZC Mesh 服务失败：" + why);
        setStatus(S_HUB, L_ERROR, L"连接失败：" + why);
        if (httpStatus == 401 || httpStatus == 302 || httpStatus == 307) g.cookie.clear(), serviceCheckedAt = 0;
    }
    if (g.captured) input::capture(false);
    if (g.controlled) input::leave();
    g.captured = g.controlled = false;
    input::setEdges(false, false, false, false);
    updateControlStatus();
}

// Windows blocks injected input into windows of elevated (admin) processes
// unless we are elevated too (UIPI). Report it so it can be fixed with one click.
void checkForegroundIntegrity() {
    if (input::isElevated()) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool elevated = false;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        HANDLE token;
        if (OpenProcessToken(h, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD len;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof e, &len)) elevated = e.TokenIsElevated;
            CloseHandle(token);
        }
        CloseHandle(h);
    } else if (fg && GetLastError() == ERROR_ACCESS_DENIED) {
        elevated = true;
    }
    if (elevated != g.elevatedForeground) {
        g.elevatedForeground = elevated;
        if (elevated) {
            setStatus(S_INPUT, L_WARN, L"当前前台是管理员权限窗口，系统禁止普通权限程序向其发送输入。点击“以管理员身份运行”授予权限");
            ShowWindow(hPermBtn, SW_SHOW);
        } else {
            setStatus(S_INPUT, L_OK, L"输入钩子与 SendInput 就绪（普通权限；管理员窗口需提权）");
        }
    }
}

void relaunchElevated() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"--elevated";
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        PostQuitMessage(0);
    } else {
        logLine(fmt(L"提权被取消或失败（%lu）", GetLastError()));
    }
}

void copyDiagnostics() {
    std::wstring s = L"LZC Mesh Windows " LZC_MESH_VERSION L"\r\n设备 ID: " + widen(g.deviceId) + L"\r\n微服: " + widen(g.boxDomain) + L"\r\n";
    for (int i = 0; i < S_COUNT; i++) s += std::wstring(labels[i]) + L": " + g.status[i].text + L"\r\n";
    int len = GetWindowTextLengthW(hLog);
    std::wstring log(len + 1, L'\0');
    GetWindowTextW(hLog, log.data(), len + 1);
    log.resize(len);
    s += L"\r\n" + log;
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        memcpy(GlobalLock(mem), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
        logLine(L"诊断信息已复制到剪贴板");
    }
}

void applyBox() {
    wchar_t buf[128];
    GetWindowTextW(hBox, buf, 128);
    std::string name = narrow(buf);
    for (auto &c : name) c = (char)tolower((unsigned char)c);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    const std::string suffix = ".heiyu.space";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) name.resize(name.size() - suffix.size());
    for (char c : name)
        if (!isalnum((unsigned char)c) && c != '-') { logLine(L"微服名只能包含字母、数字和 -"); return; }
    g.boxOverride = name;
    saveConfig();
    logLine(L"使用微服：" + (name.empty() ? std::wstring(L"懒猫客户端默认微服") : widen(name)));
    g.cookie.clear();
    serviceCheckedAt = 0;
    hubws::close();
    startProbe();
}

// ------------------------------------------------------------------ layout control

struct LayoutState {
    double scale = 1, ox = 0, oy = 0;
    int drag = -1;
    POINT dragOffset{}, dragPos{};
} L;

void layoutCompute(RECT rc) {
    if (g.devices.empty()) return;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (auto &d : g.devices) { x0 = std::min(x0, d.x); y0 = std::min(y0, d.y); x1 = std::max(x1, d.x + d.w); y1 = std::max(y1, d.y + d.h); }
    double pad = S(20), W = rc.right - 2 * pad, H = rc.bottom - 2 * pad;
    L.scale = std::min({W / ((x1 - x0) * 1.6), H / ((y1 - y0) * 1.6), 0.25 * dpi / 96});
    L.ox = pad + (W - (x1 - x0) * L.scale) / 2 - x0 * L.scale;
    L.oy = pad + (H - (y1 - y0) * L.scale) / 2 - y0 * L.scale;
}

RECT deviceRect(int i) {
    auto &d = g.devices[i];
    int x = (int)(L.ox + d.x * L.scale), y = (int)(L.oy + d.y * L.scale);
    if (i == L.drag) x = L.dragPos.x, y = L.dragPos.y;
    return RECT{x, y, x + (int)(d.w * L.scale), y + (int)(d.h * L.scale)};
}

void layoutPaint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(248, 248, 248));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    for (int x = S(8); x < rc.right; x += S(16))
        for (int y = S(8); y < rc.bottom; y += S(16)) SetPixel(dc, x, y, RGB(220, 220, 220));
    SetBkMode(dc, TRANSPARENT);
    if (g.devices.empty()) {
        SelectObject(dc, font);
        SetTextColor(dc, RGB(128, 128, 128));
        DrawTextW(dc, g.hubConnected ? L"暂无设备" : L"连接 LZC Mesh 服务后显示屏幕布局", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        if (L.drag < 0) layoutCompute(rc);
        for (int i = 0; i < (int)g.devices.size(); i++) {
            auto &d = g.devices[i];
            RECT r = deviceRect(i);
            bool me = d.id == g.deviceId, active = d.id == g.activeSource || d.id == g.activeTarget;
            int grey = active ? 18 : d.online ? 115 : 190;
            HPEN pen = CreatePen(d.online ? PS_SOLID : PS_DOT, active ? S(2) + 1 : S(1) + (S(1) > 1 ? 0 : 1) - 1 + 1, RGB(grey, grey, grey));
            HBRUSH fill = CreateSolidBrush(me ? RGB(236, 236, 236) : RGB(254, 254, 254));
            SelectObject(dc, pen);
            SelectObject(dc, fill);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, S(8), S(8));
            DeleteObject(pen);
            DeleteObject(fill);
            const wchar_t *plat = d.platform == "macos" ? L"macOS" : d.platform == "windows" ? L"Windows" : d.platform == "linux" ? L"Linux" : L"?";
            std::wstring title = widen(d.name) + (me ? L"（本机）" : L"");
            std::wstring meta = fmt(L"%ls · %.0f×%.0f%ls", plat, d.w, d.h, d.online ? L"" : L" · 离线");
            RECT t1 = r, t2 = r;
            int mid = (r.top + r.bottom) / 2;
            t1.bottom = mid; t2.top = mid;
            InflateRect(&t1, -S(4), 0);
            InflateRect(&t2, -S(4), 0);
            SelectObject(dc, fontBold);
            SetTextColor(dc, d.online ? RGB(17, 17, 17) : RGB(128, 128, 128));
            DrawTextW(dc, title.c_str(), -1, &t1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, fontSmall);
            SetTextColor(dc, RGB(119, 119, 119));
            DrawTextW(dc, meta.c_str(), -1, &t2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

LRESULT CALLBACK layoutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: layoutPaint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!g.hubConnected) return 0;
        for (int i = (int)g.devices.size() - 1; i >= 0; i--) {
            RECT r = deviceRect(i);
            if (PtInRect(&r, p)) {
                L.drag = i;
                L.dragPos = POINT{r.left, r.top};
                L.dragOffset = POINT{p.x - r.left, p.y - r.top};
                SetCapture(h);
                break;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (L.drag >= 0) {
            L.dragPos = POINT{GET_X_LPARAM(l) - L.dragOffset.x, GET_Y_LPARAM(l) - L.dragOffset.y};
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (L.drag >= 0) {
            ReleaseCapture();
            if (L.drag < (int)g.devices.size()) {
                auto &d = g.devices[L.drag];
                hubws::send(JsonOut().s("t", "layout").s("id", d.id).n("x", (L.dragPos.x - L.ox) / L.scale).n("y", (L.dragPos.y - L.oy) / L.scale).str());
            }
            L.drag = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ main window

HWND staticLabel(HWND parent, const wchar_t *text, HFONT f, int id = 0) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return h;
}
HWND buttonCtl(HWND parent, const wchar_t *text, int id, DWORD style = BS_PUSHBUTTON) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

HWND hKeyLabel[S_COUNT], hTitles[3], hHint, hBoxLabel, hButtons[6];
RECT dotRects[S_COUNT];

void makeFonts() {
    for (HFONT f : {font, fontBold, fontSmall, fontMono}) if (f) DeleteObject(f);
    auto mk = [](int size, int weight, const wchar_t *face) {
        return CreateFontW(-S(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    font = mk(13, FW_NORMAL, L"Microsoft YaHei UI");
    fontBold = mk(13, FW_SEMIBOLD, L"Microsoft YaHei UI");
    fontSmall = mk(11, FW_NORMAL, L"Microsoft YaHei UI");
    fontMono = mk(11, FW_NORMAL, L"Consolas");
}

void layoutControls(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = S(16), x = pad, y = pad, w = rc.right - 2 * pad;
    MoveWindow(hTitles[0], x, y, w, S(20), TRUE);
    y += S(26);
    for (int i = 0; i < S_COUNT; i++) {
        dotRects[i] = RECT{x, y + S(6), x + S(8), y + S(14)};
        MoveWindow(hKeyLabel[i], x + S(16), y, S(110), S(20), TRUE);
        MoveWindow(hStatusText[i], x + S(130), y, w - S(130), S(20), TRUE);
        y += S(22);
    }
    y += S(4);
    MoveWindow(hBoxLabel, x, y + S(3), S(80), S(20), TRUE);
    MoveWindow(hBox, x + S(84), y, S(320), S(24), TRUE);
    MoveWindow(hButtons[5], x + S(410), y, S(70), S(24), TRUE);
    y += S(32);
    int bx = x;
    const int widths[] = {90, 110, 110, 70};
    HWND btns[] = {hButtons[0], hOpenBtn, hButtons[2], hButtons[3]};
    for (int i = 0; i < 4; i++) { MoveWindow(btns[i], bx, y, S(widths[i]), S(28), TRUE); bx += S(widths[i]) + S(8); }
    MoveWindow(hPermBtn, bx, y, S(150), S(28), TRUE);
    y += S(42);
    MoveWindow(hTitles[1], x, y, w, S(20), TRUE);
    y += S(24);
    MoveWindow(hHint, x, y, w, S(18), TRUE);
    y += S(22);
    int logH = S(130);
    int layoutH = std::max(S(160), (int)rc.bottom - y - S(30) - S(30) - logH - pad);
    MoveWindow(hLayout, x, y, w, layoutH, TRUE);
    y += layoutH + S(6);
    MoveWindow(hSwap, x, y, w, S(22), TRUE);
    y += S(30);
    MoveWindow(hTitles[2], x, y, w, S(20), TRUE);
    y += S(24);
    MoveWindow(hLog, x, y, w, rc.bottom - y - pad, TRUE);
    InvalidateRect(h, nullptr, TRUE);
}

void applyFonts() {
    for (HWND c : hStatusText) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    for (HWND c : hKeyLabel) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    for (HWND c : hTitles) SendMessageW(c, WM_SETFONT, (WPARAM)fontBold, TRUE);
    for (HWND c : hButtons) if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    for (HWND c : {hHint, hBoxLabel}) SendMessageW(c, WM_SETFONT, (WPARAM)fontSmall, TRUE);
    for (HWND c : {hBox, hSwap, hPermBtn, hOpenBtn}) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(hLog, WM_SETFONT, (WPARAM)fontMono, TRUE);
}

void createControls(HWND h) {
    makeFonts();
    hTitles[0] = staticLabel(h, L"状态", fontBold);
    for (int i = 0; i < S_COUNT; i++) {
        hKeyLabel[i] = staticLabel(h, labels[i], font);
        hStatusText[i] = staticLabel(h, g.status[i].text.c_str(), font, 200 + i);
    }
    hBoxLabel = staticLabel(h, L"使用的微服", fontSmall);
    hBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", widen(g.boxOverride).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h, nullptr, nullptr, nullptr);
    SendMessageW(hBox, EM_SETCUEBANNER, TRUE, (LPARAM)L"留空使用懒猫客户端默认微服，例如 ponzs");
    hButtons[5] = buttonCtl(h, L"应用", ID_BOX_APPLY);
    hButtons[0] = buttonCtl(h, L"重新检测", ID_RETRY);
    hOpenBtn = buttonCtl(h, L"打开管理页面", ID_OPEN);
    hButtons[2] = buttonCtl(h, L"复制诊断信息", ID_COPY);
    hButtons[3] = buttonCtl(h, L"退出", ID_QUIT);
    hPermBtn = buttonCtl(h, L"以管理员身份运行", ID_ELEVATE);
    ShowWindow(hPermBtn, SW_HIDE);
    hTitles[1] = staticLabel(h, L"屏幕布局", fontBold);
    hHint = staticLabel(h, L"拖动屏幕调整相对位置，松手后自动贴边并同步到所有设备。控制其他设备时按 Ctrl+Alt+Esc 收回光标。", fontSmall);
    WNDCLASSW wc{};
    wc.lpfnWndProc = layoutProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LzcMeshLayout";
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassW(&wc);
    hLayout = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, h, nullptr, nullptr, nullptr);
    hSwap = buttonCtl(h, L"Mac 与 Windows/Linux 互控时交换 Command 和 Ctrl", ID_SWAP, BS_AUTOCHECKBOX);
    SendMessageW(hSwap, BM_SETCHECK, BST_CHECKED, 0);
    hTitles[2] = staticLabel(h, L"事件日志", fontBold);
    hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, h, nullptr, nullptr, nullptr);
    SendMessageW(hLog, EM_SETLIMITTEXT, 0, 0);
    applyFonts();
    layoutControls(h);
}

HBRUSH bgBrush = CreateSolidBrush(RGB(245, 245, 245));

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g.hwnd = h;
        dpi = GetDpiForWindow(h);
        createControls(h);
        SetTimer(h, ID_TIMER, 3000, nullptr);
        SetTimer(h, ID_FG_TIMER, 1000, nullptr);
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(w);
        RECT *r = (RECT *)l;
        makeFonts();
        applyFonts();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE: layoutControls(h); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)l)->ptMinTrackSize = POINT{S(720), S(700)}; return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        HWND c = (HWND)l;
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID(c);
        COLORREF col = RGB(17, 17, 17);
        if (c == hHint || c == hBoxLabel) col = RGB(119, 119, 119);
        for (int i = 0; i < S_COUNT; i++) if (c == hKeyLabel[i]) col = RGB(85, 85, 85);
        if (id >= 200 && id < 200 + S_COUNT && g.status[id - 200].level == L_ERROR) col = RGB(0, 0, 0);
        if (c == hLog) { SetBkColor(dc, RGB(250, 250, 250)); SetTextColor(dc, RGB(51, 51, 51)); static HBRUSH lb = CreateSolidBrush(RGB(250, 250, 250)); return (LRESULT)lb; }
        SetTextColor(dc, col);
        return (LRESULT)bgBrush;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        for (int i = 0; i < S_COUNT; i++) {
            Level lv = g.status[i].level;
            COLORREF fill = lv == L_OK ? RGB(17, 17, 17) : lv == L_WARN ? RGB(136, 136, 136) : lv == L_ERROR ? RGB(255, 255, 255) : RGB(200, 200, 200);
            HBRUSH b = CreateSolidBrush(fill);
            HPEN p = CreatePen(PS_SOLID, lv == L_ERROR ? S(2) : 1, lv == L_ERROR ? RGB(17, 17, 17) : fill);
            SelectObject(dc, b);
            SelectObject(dc, p);
            RECT r = dotRects[i];
            Ellipse(dc, r.left, r.top, r.right, r.bottom);
            DeleteObject(b);
            DeleteObject(p);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, bgBrush);
        return 1;
    }
    case WM_TIMER:
        if (w == ID_TIMER) startProbe();
        if (w == ID_FG_TIMER) checkForegroundIntegrity();
        return 0;
    case WM_DISPLAYCHANGE:
        updateScreenStatus();
        if (g.hubConnected) sendHello();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_RETRY: logLine(L"手动重新检测"); g.cookie.clear(); serviceCheckedAt = 0; hubws::close(); startProbe(); break;
        case ID_OPEN: if (!g.boxDomain.empty()) ShellExecuteW(h, L"open", (L"https://lzc-mesh." + widen(g.boxDomain) + L"/").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_COPY: copyDiagnostics(); break;
        case ID_QUIT: DestroyWindow(h); break;
        case ID_BOX_APPLY: applyBox(); break;
        case ID_ELEVATE: relaunchElevated(); break;
        case ID_SWAP:
            if (!updatingSwap) hubws::send(JsonOut().s("t", "settings").b("swap_cmd_ctrl", SendMessageW(hSwap, BM_GETCHECK, 0, 0) == BST_CHECKED).str());
            break;
        }
        return 0;
    case WM_APP_PROBE: onProbe((Probe *)l); return 0;
    case WM_APP_HUB_OPEN:
        g.hubConnected = true;
        input::setHubConnected(true);
        setStatus(S_HUB, L_OK, L"已连接");
        logLine(L"已连接 LZC Mesh 服务");
        sendHello();
        return 0;
    case WM_APP_HUB_TEXT: {
        std::string *s = (std::string *)l;
        onHubText(*s);
        delete s;
        return 0;
    }
    case WM_APP_HUB_CLOSED: {
        std::wstring *s = (std::wstring *)l;
        onHubClosed((int)w, *s);
        delete s;
        return 0;
    }
    case WM_APP_EDGE: {
        static const char *dirs[] = {"left", "right", "up", "down"};
        if (g.hubConnected && !g.captured && !g.controlled) hubws::send(JsonOut().s("t", "edge").s("dir", dirs[w & 3]).n("pos", l / 10000.0).str());
        return 0;
    }
    case WM_APP_INPUT:
        if (w == 1) setStatus(S_INPUT, L_OK, input::isElevated() ? L"输入钩子与 SendInput 就绪（管理员权限）" : L"输入钩子与 SendInput 就绪（普通权限；管理员窗口需提权）");
        else if (w == 0) setStatus(S_INPUT, L_ERROR, fmt(L"无法安装全局输入钩子（%lu），请以管理员身份运行或检查安全软件拦截", GetLastError()));
        else if (w == 2) {
            setStatus(S_INPUT, L_ERROR, fmt(L"系统拒绝注入输入（%lu）：可能处于锁屏/UAC 安全桌面，或前台是管理员窗口。点击“以管理员身份运行”", (DWORD)l));
            ShowWindow(hPermBtn, SW_SHOW);
        }
        if (input::isElevated()) ShowWindow(hPermBtn, SW_HIDE);
        return 0;
    case WM_DESTROY:
        hubws::close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

// ------------------------------------------------------------------ helpers from app.h

std::wstring widen(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring fmt(const wchar_t *f, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf(buf, 2047, f, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}
const char *statusName(int key) {
    static const char *n[] = {"lzc-client", "lzc-network", "login", "service", "hub", "input", "control", "screen"};
    return n[key];
}

static HANDLE logFile = INVALID_HANDLE_VALUE;
static void fileLog(const std::wstring &line) {
    if (logFile == INVALID_HANDLE_VALUE) return;
    std::string s = narrow(line) + "\r\n";
    DWORD w;
    WriteFile(logFile, s.data(), (DWORD)s.size(), &w, nullptr);
}

void setStatus(int key, Level level, const std::wstring &text) {
    Status &s = g.status[key];
    if (s.level == level && s.text == text) return;
    s.level = level;
    s.text = text;
    static const wchar_t *lv[] = {L"..", L"ok", L"warn", L"ERROR"};
    fileLog(fmt(L"[status] %hs %ls: %ls", statusName(key), lv[level], text.c_str()));
    if (hStatusText[key]) {
        SetWindowTextW(hStatusText[key], text.c_str());
        InvalidateRect(g.hwnd, &dotRects[key], TRUE);
    }
}

void logLine(const std::wstring &text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring line = fmt(L"%02d:%02d:%02d  %ls", t.wHour, t.wMinute, t.wSecond, text.c_str());
    fileLog(line);
    if (!hLog) return;
    int len = GetWindowTextLengthW(hLog);
    if (len > 60000) {
        SendMessageW(hLog, EM_SETSEL, 0, 20000);
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(hLog);
    }
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)(line + L"\r\n").c_str());
}

void devicesChanged() {
    if (hLayout) InvalidateRect(hLayout, nullptr, FALSE);
    updatingSwap = true;
    SendMessageW(hSwap, BM_SETCHECK, g.swapCmdCtrl ? BST_CHECKED : BST_UNCHECKED, 0);
    updatingSwap = false;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LzcMeshClient");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Hand over to an elevated relaunch: wait briefly for the old instance to quit.
        if (WaitForSingleObject(single, 3000) != WAIT_OBJECT_0 && WaitForSingleObject(single, 0) != WAIT_ABANDONED) {
            if (HWND other = FindWindowW(L"LzcMeshMain", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
            return 0;
        }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadConfig();
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        std::wstring dir = std::wstring(local) + L"\\LZC Mesh";
        CreateDirectoryW(dir.c_str(), nullptr);
        logFile = CreateFileW((dir + L"\\client.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"LzcMeshMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    UINT sysDpi = GetDpiForSystem();
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"LZC Mesh", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(900, sysDpi, 96),
                             MulDiv(860, sysDpi, 96), nullptr, nullptr, inst, nullptr);
    logLine(L"LZC Mesh Windows " LZC_MESH_VERSION L" 启动，设备 ID " + widen(g.deviceId) + (input::isElevated() ? L"（管理员）" : L""));
    input::start();
    updateScreenStatus();
    ShowWindow(h, show);
    startProbe();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    input::stop();
    if (single) ReleaseMutex(single);
    return 0;
}
