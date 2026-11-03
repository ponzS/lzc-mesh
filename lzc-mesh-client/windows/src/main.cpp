// 懒猫设备接力 (LZC Mesh) for Windows: tray icon, main window (mainwin.cpp), hub messages.
#include "app.h"
#include "autostart.h"
#include "bluetooth.h"
#include "clipboard.h"
#include "direct.h"
#include "files.h"
#include "layoutview.h"
#include "mainwin.h"
#include "tray.h"
#include "ui.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "input.h"
#include "identity.h"
#include "lzc.h"
#include "net.h"

App g;

namespace {
using namespace mainwin;
long long serviceCheckedAt = 0;
bool probing = false;
long long lastMoveLog = 0;
UINT taskbarCreated = 0;
UINT quitMessage = 0;   // sent by the installer / uninstaller
bool quitting = false;

enum { ID_TIMER = 1, ID_FG_TIMER = 2 };

constexpr const wchar_t *labels[] = {L"懒猫微服客户端", L"懒猫网络", L"登录会话", L"设备接力应用", L"中枢连接", L"输入权限", L"控制状态", L"本机屏幕", L"共享剪贴板", L"输入通道", L"光标通道", L"键盘通道", L"设备信任"};
static_assert(sizeof(labels)/sizeof(*labels)==S_COUNT,"Every status needs a diagnostic label");

std::wstring configPath() {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    std::wstring dir = std::wstring(home, n) + L"\\.config\\lzc-mesh";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

void saveConfig() {
    std::string text = JsonOut().s("device_id", g.deviceId).s("box", g.boxOverride).b("sharing", g.sharingEnabled).str();
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
            g.sharingEnabled = j["sharing"].boolean(true);
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
    if (g.hubConnected && !g.authVerified) { setStatus(S_CONTROL,L_WARN,L"设备未获信任，共享保持停用；请核对指纹并批准"); return; }
    if (g.deviceReady && !g.deviceEnabled) { setStatus(S_CONTROL, L_WARN, L"客户端已关闭，可在管理页面或本机重新开启"); return; }
    auto name = [](const std::string &id) { auto *d = deviceById(id); return widen(d ? d->name : id); };
    if (g.captured) setStatus(S_CONTROL, L_OK, L"本机正在控制 " + name(g.activeTarget) + L"（Ctrl+Alt+Esc 收回）");
    else if (g.controlled) setStatus(S_CONTROL, L_OK, name(g.activeSource) + L" 正在控制本机");
    else if (!g.inputEnabled) setStatus(S_CONTROL, L_WARN, L"全部设备接力已暂停，可在管理页面或本地设置中恢复");
    else if (!g.sharingEnabled) setStatus(S_CONTROL, L_UNKNOWN, L"本机键鼠共享已暂停");
    else setStatus(S_CONTROL, L_UNKNOWN, L"空闲：把光标移出屏幕边缘即可控制相邻设备");
}

void updateClipboardStatus() {
    if (g.hubConnected && !g.authVerified) setStatus(S_CLIPBOARD,L_UNKNOWN,L"设备未获信任，双向剪贴板已停用");
    else if (g.deviceReady && !g.deviceEnabled) setStatus(S_CLIPBOARD, L_UNKNOWN, L"客户端已关闭，双向剪贴板已停用");
    else if (!g.hubConnected) setStatus(S_CLIPBOARD, L_UNKNOWN, L"等待连接中枢");
    else if (g.clipboardEnabled) setStatus(S_CLIPBOARD, L_OK, L"已开启：复制的文本会同步到其他在线设备");
    else setStatus(S_CLIPBOARD, L_UNKNOWN, L"已关闭（可在管理页面或托盘菜单中开启）");
}

void updateScreenStatus() {
    input::refreshScreen();
    setStatus(S_SCREEN, L_OK, fmt(L"%d×%d（%d 个显示器，缩放 %d%%，逻辑像素 %.0f×%.0f）", g.screenW, g.screenH, GetSystemMetrics(SM_CMONITORS),
                                  (int)std::lround(g.dpiScale * 100), g.screenW / g.dpiScale, g.screenH / g.dpiScale));
}

void onInputMessage(const Json &o);

void refreshClientActivity() {
    static bool was = false;
    const bool on = g.operational();
    if (was != on) {
        was = on;
        files::setActive(on);
        clipboard::setActive(on);
        if (on) { direct::connected(onInputMessage); input::start(); }
        else {
            direct::disconnect();
            bluetooth::stop();
            input::stop();
            g.captured = g.controlled = false;
            input::setEdges(false, false, false, false);
            files::update({});
            files::setStatusText(g.deviceReady && !g.deviceEnabled ? L"客户端已关闭，文件共享已停用" : L"等待设备状态确认");
        }
        logLine(on ? L"客户端已开启" : L"客户端已休眠：共享功能已停止，保留远程唤醒连接");
    }
    input::setSharing(on && g.sharingEnabled && g.inputEnabled);
    if (!on) {
        setStatus(S_INPUT, L_UNKNOWN, L"客户端未运行共享功能，输入钩子已停止");
        for (int key : {S_TRANSPORT, S_POINTER_CHANNEL, S_KEYBOARD_CHANNEL}) setStatus(key, L_UNKNOWN,
            g.deviceReady && !g.deviceEnabled ? L"客户端已关闭 · 仅保留远程唤醒连接" : L"等待设备状态确认");
    }
}

void sendHello() {
    if (!identity::init()) return;
    wchar_t name[256];
    DWORD n = 256;
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n);
    hubws::send(JsonOut().s("t", g.authVerified ? "hello" : "device_hello").s("id", g.deviceId).s("name", narrow(name)).s("platform", "windows")
                    .s("public_key", identity::publicKey()).n("w", g.screenW / g.dpiScale).n("h", g.screenH / g.dpiScale).s("version", LZC_MESH_VERSION).str());
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
    if (changed && (g.hubConnected || hubws::busy())) {
        logLine(L"懒猫微服或登录会话变化，重新连接");
        hubws::close();
    }
    if (ready && !g.hubConnected && !hubws::busy()) {
        setStatus(S_HUB, L_WARN, L"正在连接 wss://lzc-mesh." + widen(g.boxDomain) + L"/api/ws");
        hubws::connect(L"lzc-mesh." + widen(g.boxDomain), widen(g.cookie));
    }
    if (!ready && (g.hubConnected || hubws::busy())) hubws::close();
    if (!ready) setStatus(S_HUB, L_UNKNOWN, L"等待懒猫设备接力应用可用");
    setOpenEnabled(!g.boxDomain.empty());
    mainwin::refresh();
    tray::refresh();
    delete p;
}

void onInputMessage(const Json &o) {
    const auto t = o["t"].str();
    const double x = o["x"].num(0.5), y = o["y"].num(0.5);
    const auto scope = o["_control_session"].str();
    const bool begin = t == "direct_active" && !o["source"].str().empty();
    if (!(begin ? mesh_input_gate_begin(&g.inputGate, scope.c_str()) : mesh_input_gate_accept(&g.inputGate, scope.c_str()))) return;
    if (scope.empty() && !g.blockedControlId.empty() && g.blockedControlId == g.activeControlId &&
        (t == "enter" || t == "capture" || t == "move" || t == "key" || t == "button" || t == "wheel")) return;
    if (t == "direct_active") {
        g.activeSource = o["source"].str(); g.activeTarget = o["target"].str();
        updateControlStatus();
    } else if (t == "release") {
        const bool was = g.captured;
        g.captured = false; input::capture(false);
        if (o["warp"].boolean()) input::warp(x, y);
        if (was) logLine(L"控制已收回本机");
        updateControlStatus();
    } else if (t == "leave") {
        const bool was = g.controlled;
        g.controlled = false; input::leave();
        if (was) logLine(L"远程控制结束");
        updateControlStatus();
    } else if (!g.operational() || !g.inputEnabled || !g.sharingEnabled) {
        return;
    } else if (t == "capture") {
        if (g.controlled) input::leave();
        g.controlled = false;
        g.captured = true; input::capture(true, scope);
        logLine(L"开始控制其他设备"); updateControlStatus();
    } else if (t == "enter") {
        if (g.captured) input::capture(false);
        g.captured = false;
        g.receiveControlId = scope.empty() ? g.activeControlId : scope;
        g.controlled = true; input::enter(x, y);
        logLine(fmt(L"被远程控制：光标进入本机 (%.2f, %.2f)", x, y)); updateControlStatus();
    } else if (t == "move" && g.controlled && input::receiving()) {
        input::move(x, y);
        if (GetTickCount64() - lastMoveLog > 2000) {
            lastMoveLog = GetTickCount64(); logLine(fmt(L"收到远程光标 (%.3f, %.3f)", x, y));
        }
    } else if (t == "button" && g.controlled && input::receiving()) {
        input::button((int)o["button"].num(1), o["down"].boolean());
    } else if (t == "wheel" && g.controlled && input::receiving()) {
        input::wheel(o["dx"].num(), o["dy"].num());
    } else if (t == "key" && g.controlled && input::receiving()) {
        input::key((uint32_t)o["code"].num(), o["down"].boolean());
    }
}

void onHubText(const std::string &text) {
    Json o;
    if (!Json::parse(text, o) || !o.isObj()) return;
    std::string t = o["t"].str();
    if (t == "device_challenge") {
        if (o["user"].str()!=g.hubUser) { hubws::close(); return; }
        const auto proof=identity::proof(g.hubUser,o["nonce"].str());
        if (!proof.empty()) hubws::send(JsonOut().s("t","device_proof").s("signature",proof).str());
    } else if (t == "device_auth") {
        const auto status=o["status"].str(),token=o["token"].str();
        const bool valid=o["fingerprint"].str()==identity::fingerprint();
        g.authVerified=valid && status=="approved" && token.size()==44 && token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")==std::string::npos;
        g.fileToken=g.authVerified?token:"";
        const auto message=g.authVerified?L"已批准":status=="key_mismatch"?L"凭证不匹配，拒绝绑定":status=="revoked"?L"信任已撤销":L"等待管理页核对并批准";
        setStatus(S_TRUST,g.authVerified?L_OK:L_WARN,std::wstring(message)+L" · 指纹 "+widen(identity::fingerprint()));
        refreshClientActivity(); updateControlStatus(); updateClipboardStatus();
        if (g.authVerified && !g.sharingEnabled) hubws::send(JsonOut().s("t","sharing").b("enabled",false).str());
    } else if (t == "welcome") {
        if (o["auth_version"].num()!=1) setStatus(S_TRUST,L_ERROR,L"需要支持设备凭证的新版中枢；共享保持停用");
        g.hubUser = o["user"].str();
        if (!o["id"].str().empty()) logLine(L"已加入 " + widen(g.hubUser) + L" 的设备组");
    } else if (t == "state") {
        g.devices.clear();
        g.deviceReady = false;
        g.deviceTrusted = false;
        int online = 0;
        for (auto &d : o["devices"].a) {
            MeshDevice m{d["id"].str(), d["name"].str(), d["platform"].str(), d["version"].str(), d["x"].num(), d["y"].num(), d["w"].num(), d["h"].num(), d["online"].boolean(),
                         d["sharing"].boolean(true), d["enabled"].boolean(true)};
            if (m.id == g.deviceId) { g.deviceReady = true; g.deviceEnabled = m.enabled; g.deviceControl = d["enabled"].type == Json::Bool; g.deviceTrusted = d["trusted"].boolean(false); }
            online += m.online;
            g.devices.push_back(m);
        }
        g.activeSource = o["active"]["source"].str();
        g.activeTarget = o["active"]["target"].str();
        g.activeControlId = o["active"]["control_id"].str();
        if (g.controlled && g.receiveControlId.empty()) g.receiveControlId = g.activeControlId;
        g.swapCmdCtrl = o["settings"]["swap_cmd_ctrl"].boolean(true);
        g.nativeProtocol = o["settings"]["transport_mode"].type == Json::Str;
        g.transportMode = o["settings"]["transport_mode"].str("auto");
        g.inputEnabled = o["settings"]["input_enabled"].boolean(true);
        refreshClientActivity();
        if (!g.inputEnabled) {
            if (g.captured) input::capture(false);
            if (g.controlled) input::leave();
            g.captured = g.controlled = false;
        }
        if (!g.nativeProtocol) {
            for (int key : {S_TRANSPORT, S_POINTER_CHANNEL, S_KEYBOARD_CHANNEL}) setStatus(key, L_OK, L"中枢转发 · 兼容旧版中枢");
        }
        bool clip = o["settings"]["clipboard"].boolean(true);
        if (clip != g.clipboardEnabled) logLine(clip ? L"共享剪贴板已开启" : L"共享剪贴板已关闭");
        g.clipboardEnabled = clip;
        updateClipboardStatus();
        setStatus(S_HUB, L_OK, fmt(L"已连接 · 用户 %ls · %d 台设备在线", widen(g.hubUser).c_str(), online));
        updateControlStatus();
        devicesChanged();
    } else if (t == "edges") {
        input::setEdges(o["left"].boolean(), o["right"].boolean(), o["up"].boolean(), o["down"].boolean());
    } else if (t == "clipboard") {
        clipboard::set(o["text"].str(), o["from"].str());
    } else if (t == "files") {
        std::vector<SharedFile> list;
        for (auto &f : o["files"].a) list.push_back(SharedFile{f["name"].str(), f["uploader"].str(), (long long)f["size"].num(), f["modified"].num()});
        files::update(g.operational() ? list : std::vector<SharedFile>{});
    } else if (t == "error") {
        logLine(L"服务端错误：" + widen(o["message"].str()));
    } else onInputMessage(o);
    direct::hub(text);
}

void onHubClosed(int httpStatus, const std::wstring &why) {
    bool was = g.hubConnected;
    g.hubConnected = false;
    g.nativeProtocol = false;
    g.deviceReady = false;
    g.authVerified = g.deviceTrusted = false; g.fileToken.clear();
    refreshClientActivity();
    direct::disconnect();
    input::setHubConnected(false);
    for (int key : {S_TRANSPORT, S_POINTER_CHANNEL, S_KEYBOARD_CHANNEL}) setStatus(key, L_UNKNOWN, L"等待中枢认证");
    if (was) {
        logLine(L"与设备接力中枢的连接已断开：" + why);
        setStatus(S_HUB, L_WARN, L"连接已断开，稍后自动重连");
    } else {
        logLine(L"连接设备接力中枢失败：" + why);
        setStatus(S_HUB, L_ERROR, L"连接失败：" + why);
        if (httpStatus == 401 || httpStatus == 302 || httpStatus == 307) g.cookie.clear(), serviceCheckedAt = 0;
    }
    if (g.captured) input::capture(false);
    if (g.controlled) input::leave();
    g.captured = g.controlled = false;
    g.activeSource.clear(); g.activeTarget.clear();
    input::setEdges(false, false, false, false);
    updateControlStatus();
    updateClipboardStatus();
}

// Windows blocks injected input into windows of elevated (admin) processes
// unless we are elevated too (UIPI). Report it so it can be fixed with one click.
void checkForegroundIntegrity() {
    if (!g.operational() || input::isElevated()) return;
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
            showElevate(true);
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
    std::wstring s = L"懒猫设备接力 Windows " LZC_MESH_VERSION L"\r\n设备 ID: " + widen(g.deviceId) + L"\r\n微服: " + widen(g.boxDomain) + L"\r\n";
    for (int i = 0; i < S_COUNT; i++) s += std::wstring(labels[i]) + L": " + g.status[i].text + L"\r\n";
    HWND hLog = logWindow();
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
    GetWindowTextW(boxEdit(), buf, 128);
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

void setSharing(bool on) {
    if (on == g.sharingEnabled) return;
    g.sharingEnabled = on;
    saveConfig();
    input::setSharing(g.operational() && on && g.inputEnabled);
    // Give the cursor back at once if this PC is controlling another device.
    if (!on && g.captured) input::releaseRemote();
    if (!on && g.controlled) { input::leave(); g.controlled = false; }
    hubws::send(JsonOut().s("t", "sharing").b("enabled", on).str());
    logLine(on ? L"键鼠共享已开启" : L"键鼠共享已暂停");
    updateControlStatus();
    mainwin::refresh();
    tray::refresh();
}

void setAutostart(bool on) {
    if (!autostart::set(on)) logLine(L"设置开机启动失败：无法写入注册表");
    else logLine(on ? L"已开启开机自动启动" : L"已关闭开机自动启动");
    mainwin::refresh();
}

void showWindow(HWND h) {
    ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(h);
}

void openManagement(HWND h) {
    if (!g.boxDomain.empty()) ShellExecuteW(h, L"open", (L"https://lzc-mesh." + widen(g.boxDomain) + L"/").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void quit(HWND h) {
    quitting = true;
    DestroyWindow(h);
}

HBRUSH editBrush = CreateSolidBrush(RGB(255, 255, 255));

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (taskbarCreated && m == taskbarCreated) { tray::onTaskbarCreated(h); return 0; }
    if (quitMessage && m == quitMessage) { quit(h); return 0; }
    switch (m) {
    case WM_CREATE:
        g.hwnd = h;
        mainwin::create(h);
        SetTimer(h, ID_TIMER, 3000, nullptr);
        SetTimer(h, ID_FG_TIMER, 1000, nullptr);
        clipboard::start(h);
        updateClipboardStatus();
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        quitMessage = RegisterWindowMessageW(L"LzcMeshQuit");
        // Allow the (possibly lower-integrity) installer to send it.
        ChangeWindowMessageFilterEx(h, quitMessage, MSGFLT_ALLOW, nullptr);
        tray::add(h);
        mainwin::refresh();
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        mainwin::onDpi(h);
        return 0;
    }
    case WM_SIZE: mainwin::layout(h); return 0;
    case WM_VSCROLL: mainwin::scroll(LOWORD(w)); return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(h, &pt);
        if (mainwin::wheel(pt, GET_WHEEL_DELTA_WPARAM(w))) return 0;
        break;
    }
    case WM_GETMINMAXINFO: {
        UINT d = GetDpiForWindow(h);
        ((MINMAXINFO *)l)->ptMinTrackSize = POINT{MulDiv(900, d, 96), MulDiv(640, d, 96)};
        return 0;
    }
    case WM_PAINT: mainwin::paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM: return ui::drawItem((DRAWITEMSTRUCT *)l) ? TRUE : FALSE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        // Edit boxes (incl. the read-only log) on white with dark text.
        HDC dc = (HDC)w;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, (HWND)l == logWindow() ? RGB(64, 64, 64) : RGB(17, 17, 17));
        return (LRESULT)editBrush;
    }
    case WM_CLOSE:
        // Closing the window keeps the client running in the tray.
        if (!quitting) { ShowWindow(h, SW_HIDE); return 0; }
        break;
    case WM_TIMER:
        if (w == ID_TIMER) startProbe();
        if (w == ID_FG_TIMER) checkForegroundIntegrity();
        return 0;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t *)l, L"ImmersiveColorSet") == 0) tray::onThemeChanged();
        return 0;
    case WM_DISPLAYCHANGE:
        updateScreenStatus();
        if (g.hubConnected) sendHello();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(l)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT: showWindow(h); break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: tray::showMenu(h); break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_RETRY: logLine(L"手动重新检测"); g.cookie.clear(); serviceCheckedAt = 0; hubws::close(); startProbe(); break;
        case ID_OPEN:
        case tray::ID_TRAY_OPEN: openManagement(h); break;
        case ID_COPY: copyDiagnostics(); break;
        case ID_BOX_APPLY: applyBox(); break;
        case ID_ELEVATE: relaunchElevated(); break;
        case ID_FILES_UPLOAD: files::pickAndUpload(); break;
        case ID_FILES_DOWNLOAD: files::downloadSelected(); break;
        case ID_FILES_DELETE: files::deleteSelected(); break;
        case ID_SHARING:
        case tray::ID_TRAY_SHARING: setSharing(!g.sharingEnabled); break;
        case ID_AUTOSTART:
        case tray::ID_TRAY_AUTOSTART: setAutostart(!autostart::enabled()); break;
        case ID_SWAP: hubws::send(JsonOut().s("t", "settings").b("swap_cmd_ctrl", !g.swapCmdCtrl).str()); break;
        case ID_TRANSPORT:
            if (HIWORD(w) == CBN_SELCHANGE) hubws::send(JsonOut().s("t", "settings").s("transport_mode", mainwin::selectedTransport()).str());
            break;
        case ID_PAUSE_ALL: hubws::send(JsonOut().s("t", "settings").b("input_enabled", !g.inputEnabled).str()); break;
        case ID_CLIENT:
        case tray::ID_TRAY_CLIENT:
            if (g.hubConnected && g.deviceControl) hubws::send(JsonOut().s("t", "device_enabled").s("id", g.deviceId).b("enabled", !g.deviceEnabled).str());
            break;
        case tray::ID_TRAY_CLIPBOARD: hubws::send(JsonOut().s("t", "settings").b("clipboard", !g.clipboardEnabled).str()); break;
        case tray::ID_TRAY_SHOW: showWindow(h); break;
        case tray::ID_TRAY_QUIT: quit(h); break;
        case ID_BOX_EDIT:
            // Repaint the frame around the edit when it gains / loses focus.
            if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
            break;
        }
        return 0;
    case WM_APP_PROBE: onProbe((Probe *)l); return 0;
    case WM_CLIPBOARDUPDATE: clipboard::onUpdate(); return 0;
    case WM_DROPFILES: files::onDrop((HDROP)w); return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)l;
        if (n->idFrom == ID_FILES_LIST && n->code == NM_DBLCLK) files::downloadSelected();
        return 0;
    }
    case WM_APP_FILES_STATUS: {
        std::wstring *s = (std::wstring *)l;
        if (g.operational() && files::accepts((uint64_t)w)) { files::setStatusText(*s); logLine(*s); }
        delete s;
        return 0;
    }
    case WM_APP_FILES_REVEAL: {
        std::wstring *p = (std::wstring *)l;
        if (g.operational() && files::accepts((uint64_t)w)) ShellExecuteW(h, L"open", L"explorer.exe", (L"/select,\"" + *p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        delete p;
        return 0;
    }
    case WM_APP_HUB_OPEN:
        if (!hubws::isCurrent(static_cast<unsigned>(w))) return 0;
        g.hubConnected = true;
        g.deviceReady = g.deviceTrusted = g.authVerified = false; g.fileToken.clear();
        refreshClientActivity();
        input::setHubConnected(true);
        setStatus(S_HUB, L_OK, L"已连接");
        logLine(L"已连接设备接力中枢");
        sendHello();
        tray::refresh();
        return 0;
    case WM_APP_HUB_TEXT: {
        std::string *s = (std::string *)l;
        if (hubws::isCurrent(static_cast<unsigned>(w))) onHubText(*s);
        delete s;
        return 0;
    }
    case WM_APP_HUB_CLOSED: {
        auto *s = reinterpret_cast<hubws::Closed *>(l);
        if (hubws::isCurrent(s->generation, true)) { onHubClosed(s->status, s->reason); tray::refresh(); }
        delete s;
        return 0;
    }
    case WM_APP_NET_LOG: {
        auto *s = reinterpret_cast<std::wstring *>(l);
        if (hubws::isCurrent(static_cast<unsigned>(w))) logLine(*s);
        delete s;
        return 0;
    }
    case WM_APP_EDGE: {
        static const char *dirs[] = {"left", "right", "up", "down"};
        if (g.operational() && g.sharingEnabled && g.inputEnabled && !g.captured && !g.controlled) direct::send(JsonOut().s("t", "edge").s("dir", dirs[w & 3]).n("pos", l / 10000.0).str());
        return 0;
    }
    case WM_APP_DIRECT: direct::drain(); return 0;
    case WM_APP_INPUT:
        if (!g.operational()) return 0;
        if (w == 4) {
            if (!g.controlled || (uint64_t)l != input::receiveEpoch()) { input::completeLocalActivity((uint64_t)l,false); return 0; }
            const auto id = g.receiveControlId;
            mesh_input_gate_cancel(&g.inputGate);
            g.blockedControlId = id;
            g.controlled = false;
            input::leave();
            if (!id.empty()) direct::send(JsonOut().s("t", "local_activity").s("control_id", id).str());
            input::completeLocalActivity((uint64_t)l,true);
            logLine(L"检测到本机实体输入，已收回控制并释放远程按键和按钮");
            updateControlStatus();
        }
        else if (w == 3) {
            if ((uint64_t)l != input::captureGeneration()) return 0;
            logLine(L"输入捕获未就绪或队列超限，已安全收回控制");
            input::releaseRemote();
            g.captured = false; input::capture(false); updateControlStatus();
        }
        else if (w == 1) setStatus(S_INPUT, L_OK, input::isElevated() ? L"输入钩子与 SendInput 就绪（管理员权限）" : L"输入钩子与 SendInput 就绪（普通权限；管理员窗口需提权）");
        else if (w == 0) setStatus(S_INPUT, L_ERROR, fmt(L"无法安装全局输入钩子（%lu），请以管理员身份运行或检查安全软件拦截", GetLastError()));
        else if (w == 2) {
            setStatus(S_INPUT, L_ERROR, fmt(L"系统拒绝注入输入（%lu）：可能处于锁屏/UAC 安全桌面，或前台是管理员窗口。点击“以管理员身份运行”", (DWORD)l));
            showElevate(true);
        }
        if (input::isElevated()) showElevate(false);
        return 0;
    case WM_DESTROY:
        g.hubConnected = false; g.deviceReady = false;
        refreshClientActivity();
        direct::shutdown();
        tray::remove();
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
    static const char *n[] = {"lzc-client", "lzc-network", "login", "service", "hub", "input", "control", "screen", "clipboard", "transport", "pointer-channel", "keyboard-channel", "device-trust"};
    static_assert(sizeof(n)/sizeof(*n)==S_COUNT,"Every status needs a stable diagnostic key");
    return key>=0 && key<S_COUNT?n[key]:"unknown";
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
    if (g.hwnd) {
        mainwin::refresh();
        tray::refresh();
    }
}

void logLine(const std::wstring &text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring line = fmt(L"%02d:%02d:%02d  %ls", t.wHour, t.wMinute, t.wSecond, text.c_str());
    fileLog(line);
    HWND hLog = mainwin::logWindow();
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
    mainwin::refresh();
    tray::refresh();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\LzcMeshClient");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Hand over to an elevated relaunch: wait briefly for the old instance to quit.
        const DWORD waited = WaitForSingleObject(single, 3000);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
            // Second launch (e.g. Start menu while running in the tray): show the existing window.
            if (HWND other = FindWindowW(L"LzcMeshMain", nullptr)) { ShowWindow(other, IsIconic(other) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(other); }
            if (single) CloseHandle(single);
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
    bool background = wcsstr(GetCommandLineW(), L"--background") != nullptr;
    // Default size fits every panel without scrolling, but never more than the work area.
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ww = std::min(MulDiv(1240, sysDpi, 96), (int)(work.right - work.left) - MulDiv(40, sysDpi, 96));
    int wh = std::min(MulDiv(880, sysDpi, 96), (int)(work.bottom - work.top) - MulDiv(40, sysDpi, 96));
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"懒猫设备接力", WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, nullptr, nullptr, inst, nullptr);
    logLine(L"懒猫设备接力 Windows " LZC_MESH_VERSION L" 启动，设备 ID " + widen(g.deviceId) + (input::isElevated() ? L"（管理员）" : L""));
    input::setSharing(false);
    updateScreenStatus();
    // Started at login: stay in the tray without opening the window.
    if (!background) ShowWindow(h, show);
    startProbe();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    input::stop();
    identity::shutdown();
    // Close the writer before another instance acquires the mutex on restart.
    if (logFile != INVALID_HANDLE_VALUE) { CloseHandle(logFile); logFile = INVALID_HANDLE_VALUE; }
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    return 0;
}
