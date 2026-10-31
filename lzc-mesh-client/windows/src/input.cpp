#include "app.h"
#include "input.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

#include "app.h"
#include "net.h"

namespace input {
namespace {
const ULONG_PTR kMarker = 0x4C5A4D45;  // dwExtraInfo of events we inject

std::atomic<bool> captured{false}, hubOn{false}, controlled{false};
std::atomic<bool> edges[4];  // left right up down
std::atomic<long long> quietUntil{0}, lastEdge{0};
std::atomic<int> vx{0}, vy{0}, vw{0}, vh{0};
std::atomic<double> scale{1.0};
POINT anchor{};
HHOOK mouseHook, keyHook;
DWORD hookThreadId;
std::thread hookThread, sendThread;
HWND overlay;

// ---- outgoing queue: hook callbacks must return fast, so a sender thread does the network I/O.
std::mutex qmu;
std::condition_variable qcv;
std::deque<std::string> queue;
double pendDx = 0, pendDy = 0;
bool running = false;

void enqueue(std::string s) {
    { std::lock_guard<std::mutex> l(qmu); queue.push_back(std::move(s)); }
    qcv.notify_one();
}
void enqueueMotion(double dx, double dy) {
    { std::lock_guard<std::mutex> l(qmu); pendDx += dx; pendDy += dy; }
    qcv.notify_one();
}
void sender() {
    std::unique_lock<std::mutex> l(qmu);
    while (running) {
        qcv.wait(l, [] { return !running || !queue.empty() || pendDx != 0 || pendDy != 0; });
        // Motion first keeps button presses at the right place: flush accumulated motion, then events.
        if (pendDx != 0 || pendDy != 0) {
            std::string m = JsonOut().s("t", "motion").n("dx", pendDx).n("dy", pendDy).str();
            pendDx = pendDy = 0;
            l.unlock();
            hubws::send(m);
            l.lock();
        }
        while (!queue.empty()) {
            std::string s = std::move(queue.front());
            queue.pop_front();
            l.unlock();
            hubws::send(s);
            l.lock();
        }
    }
}

long long nowMs() { return (long long)GetTickCount64(); }

// ---- key codes: evdev <-> PC set-1 scan codes (evdev 1..88 equal the scan code).
struct Sc { uint16_t sc; bool ext; };
bool evdevToScan(uint32_t e, Sc &out) {
    if (e >= 1 && e <= 88 && e != 84 && e != 85) { out = {(uint16_t)e, false}; return true; }
    switch (e) {
    case 96: out = {0x1C, true}; return true;   // KP enter
    case 97: out = {0x1D, true}; return true;   // right ctrl
    case 98: out = {0x35, true}; return true;   // KP slash
    case 99: out = {0x37, true}; return true;   // print screen
    case 100: out = {0x38, true}; return true;  // right alt
    case 102: out = {0x47, true}; return true;  // home
    case 103: out = {0x48, true}; return true;  // up
    case 104: out = {0x49, true}; return true;  // page up
    case 105: out = {0x4B, true}; return true;  // left
    case 106: out = {0x4D, true}; return true;  // right
    case 107: out = {0x4F, true}; return true;  // end
    case 108: out = {0x50, true}; return true;  // down
    case 109: out = {0x51, true}; return true;  // page down
    case 110: out = {0x52, true}; return true;  // insert
    case 111: out = {0x53, true}; return true;  // delete
    case 113: out = {0x20, true}; return true;  // mute
    case 114: out = {0x2E, true}; return true;  // volume down
    case 115: out = {0x30, true}; return true;  // volume up
    case 117: out = {0x59, false}; return true; // KP equal
    case 121: out = {0x7E, false}; return true; // KP comma
    case 124: out = {0x7D, false}; return true; // yen
    case 89: out = {0x73, false}; return true;  // ro
    case 92: out = {0x79, false}; return true;  // henkan
    case 94: out = {0x7B, false}; return true;  // muhenkan
    case 122: out = {0x72, false}; return true; // hangeul
    case 123: out = {0x71, false}; return true; // hanja
    case 125: out = {0x5B, true}; return true;  // left win
    case 126: out = {0x5C, true}; return true;  // right win
    case 127: out = {0x5D, true}; return true;  // menu
    }
    if (e >= 183 && e <= 190) { out = {(uint16_t)(0x64 + (e - 183)), false}; return true; }  // F13..F20
    return false;
}

uint32_t scanToEvdev(uint16_t sc, bool ext) {
    static uint32_t table[2][256];
    static bool built = false;
    if (!built) {
        for (uint32_t e = 1; e < 256; e++) {
            Sc s;
            if (evdevToScan(e, s) && s.sc < 256 && !table[s.ext][s.sc]) table[s.ext][s.sc] = e;
        }
        table[1][0x45] = 69;  // num lock reports E0 on some keyboards
        table[0][0x45] = 69;
        table[0][0x54] = 99;  // Alt+PrintScreen
        built = true;
    }
    return sc < 256 ? table[ext ? 1 : 0][sc] : 0;
}

// ---- hooks

void edgeCheck(POINT pt) {
    if (!hubOn || controlled || nowMs() < quietUntil || nowMs() - lastEdge < 250) return;
    int x0 = vx, y0 = vy, x1 = vx + vw - 1, y1 = vy + vh - 1;
    int dir = -1;
    if (edges[0] && pt.x <= x0) dir = 0;
    else if (edges[1] && pt.x >= x1) dir = 1;
    else if (edges[2] && pt.y <= y0) dir = 2;
    else if (edges[3] && pt.y >= y1) dir = 3;
    if (dir < 0) return;
    lastEdge = nowMs();
    double pos = dir < 2 ? double(pt.y - y0) / std::max(1, (int)vh) : double(pt.x - x0) / std::max(1, (int)vw);
    PostMessageW(g.hwnd, WM_APP_EDGE, (WPARAM)dir, (LPARAM)(std::min(1.0, std::max(0.0, pos)) * 10000));
}

LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    auto *m = (MSLLHOOKSTRUCT *)lp;
    if ((m->flags & LLMHF_INJECTED) && m->dwExtraInfo == kMarker) return CallNextHookEx(nullptr, code, wp, lp);
    if (!captured) {
        if (wp == WM_MOUSEMOVE && !(m->flags & LLMHF_INJECTED)) edgeCheck(m->pt);
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    // Capturing: swallow everything; the pointer stays parked on the anchor.
    double s = scale;
    switch (wp) {
    case WM_MOUSEMOVE: {
        double dx = (m->pt.x - anchor.x) / s, dy = (m->pt.y - anchor.y) / s;
        if (dx != 0 || dy != 0) enqueueMotion(dx, dy);
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: enqueue(JsonOut().s("t", "button").n("button", 1).b("down", wp == WM_LBUTTONDOWN).str()); break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: enqueue(JsonOut().s("t", "button").n("button", 2).b("down", wp == WM_RBUTTONDOWN).str()); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: enqueue(JsonOut().s("t", "button").n("button", 3).b("down", wp == WM_MBUTTONDOWN).str()); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
        int b = HIWORD(m->mouseData) == XBUTTON1 ? 4 : 5;
        enqueue(JsonOut().s("t", "button").n("button", b).b("down", wp == WM_XBUTTONDOWN).str());
        break;
    }
    case WM_MOUSEWHEEL: enqueue(JsonOut().s("t", "wheel").n("dx", 0).n("dy", (short)HIWORD(m->mouseData)).str()); break;
    case WM_MOUSEHWHEEL: enqueue(JsonOut().s("t", "wheel").n("dx", -(short)HIWORD(m->mouseData)).n("dy", 0).str()); break;
    }
    return 1;
}

LRESULT CALLBACK keyProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || !captured) return CallNextHookEx(nullptr, code, wp, lp);
    auto *k = (KBDLLHOOKSTRUCT *)lp;
    if ((k->flags & LLKHF_INJECTED) && k->dwExtraInfo == kMarker) return CallNextHookEx(nullptr, code, wp, lp);
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    uint32_t e = scanToEvdev((uint16_t)k->scanCode, k->flags & LLKHF_EXTENDED);
    if (!e) {
        // Some keys arrive without a scan code; fall back to the virtual key.
        UINT sc = MapVirtualKeyW(k->vkCode, MAPVK_VK_TO_VSC_EX);
        e = scanToEvdev(sc & 0xFF, (sc & 0xFF00) == 0xE000);
    }
    static std::set<uint32_t> held;
    if (e) {
        // Auto-repeat would look like extra key presses on the target: forward only state changes.
        bool was = held.count(e) > 0;
        if (down && !was) { held.insert(e); enqueue(JsonOut().s("t", "key").n("code", e).b("down", true).str()); }
        if (!down && was) { held.erase(e); enqueue(JsonOut().s("t", "key").n("code", e).b("down", false).str()); }
    }
    if (!captured) held.clear();
    return 1;
}

void hookLoop() {
    hookThreadId = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
    keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, GetModuleHandleW(nullptr), 0);
    bool ok = mouseHook && keyHook;
    PostMessageW(g.hwnd, WM_APP_INPUT, ok ? 1 : 0, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
    if (mouseHook) UnhookWindowsHookEx(mouseHook);
    if (keyHook) UnhookWindowsHookEx(keyHook);
}

// ---- injection

std::set<uint32_t> heldKeys;
std::set<int> heldButtons;
bool blockedReported = false;

void sendInputs(INPUT *in, UINT n) {
    for (UINT i = 0; i < n; i++) {
        if (in[i].type == INPUT_MOUSE) in[i].mi.dwExtraInfo = kMarker;
        else if (in[i].type == INPUT_KEYBOARD) in[i].ki.dwExtraInfo = kMarker;
    }
    if (SendInput(n, in, sizeof(INPUT)) != n) {
        if (!blockedReported) {
            blockedReported = true;
            DWORD err = GetLastError();
            PostMessageW(g.hwnd, WM_APP_INPUT, 2, (LPARAM)err);
        }
    } else {
        blockedReported = false;
    }
}

LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

void showOverlay(bool on) {
    if (!overlay) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LzcMeshCaptureOverlay";
        wc.hCursor = nullptr;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassW(&wc);
        overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                                  0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        SetLayeredWindowAttributes(overlay, 0, 1, LWA_ALPHA);
    }
    if (on) {
        SetWindowPos(overlay, HWND_TOPMOST, vx, vy, vw, vh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(overlay, SW_HIDE);
    }
}
}  // namespace

bool isElevated() {
    HANDLE token;
    TOKEN_ELEVATION e{};
    DWORD len;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof e, &len)) r = e.TokenIsElevated;
        CloseHandle(token);
    }
    return r;
}

void refreshScreen() {
    vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    scale = GetDpiForSystem() / 96.0;
    g.screenX = vx; g.screenY = vy; g.screenW = vw; g.screenH = vh;
    g.dpiScale = scale;
}

void start() {
    refreshScreen();
    running = true;
    sendThread = std::thread(sender);
    hookThread = std::thread(hookLoop);
}

void stop() {
    if (captured) capture(false);
    leave();
    if (hookThreadId) PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
    if (hookThread.joinable()) hookThread.join();
    { std::lock_guard<std::mutex> l(qmu); running = false; }
    qcv.notify_all();
    if (sendThread.joinable()) sendThread.join();
}

void setEdges(bool l, bool r, bool u, bool d) { edges[0] = l; edges[1] = r; edges[2] = u; edges[3] = d; }
void setHubConnected(bool on) { hubOn = on; }

void capture(bool on) {
    if (on) {
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon, &mi);
        anchor = POINT{(mi.rcMonitor.left + mi.rcMonitor.right) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2};
        showOverlay(true);
        SetCursorPos(anchor.x, anchor.y);
        captured = true;
    } else {
        captured = false;
        showOverlay(false);
        quietUntil = nowMs() + 400;
    }
}

void warp(double nx, double ny) {
    quietUntil = nowMs() + 400;
    SetCursorPos(vx + (int)(nx * (vw - 1)), vy + (int)(ny * (vh - 1)));
}

void move(double nx, double ny) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(std::min(1.0, std::max(0.0, nx)) * 65535);
    in.mi.dy = (LONG)(std::min(1.0, std::max(0.0, ny)) * 65535);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    sendInputs(&in, 1);
}

void enter(double nx, double ny) {
    controlled = true;
    quietUntil = nowMs() + 400;
    move(nx, ny);
}

void button(int b, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (b) {
    case 1: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case 3: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 4: case 5:
        in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        in.mi.mouseData = b == 4 ? XBUTTON1 : XBUTTON2;
        break;
    default: return;
    }
    if (down) heldButtons.insert(b); else heldButtons.erase(b);
    sendInputs(&in, 1);
}

void wheel(double dx, double dy) {
    INPUT in[2]{};
    UINT n = 0;
    if (dy != 0) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_WHEEL; in[n].mi.mouseData = (DWORD)(LONG)dy; n++; }
    if (dx != 0) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL; in[n].mi.mouseData = (DWORD)(LONG)(-dx); n++; }
    if (n) sendInputs(in, n);
}

namespace {
void sendKey(const Sc &s, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = s.sc;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (s.ext ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    sendInputs(&in, 1);
}

// Key repeat. Windows only auto-repeats keys of a physical keyboard, so a held
// remote key is repeated here with the user's own delay/rate settings.
UINT_PTR repeatTimer = 0;
uint32_t repeatKey = 0;
bool repeating = false;

bool noRepeat(uint32_t e) {
    switch (e) {
    case 29: case 97: case 42: case 54: case 56: case 100: case 125: case 126:  // ctrl shift alt win
    case 58: case 69: case 70:                                                  // caps / num / scroll lock
        return true;
    }
    return false;
}

UINT repeatDelayMs() {
    int d = 1;  // 0..3 -> 250..1000 ms
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &d, 0);
    return 250 * (std::min(3, std::max(0, d)) + 1);
}

UINT repeatIntervalMs() {
    DWORD s = 31;  // 0..31 -> ~2.5..~30 repeats per second
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &s, 0);
    double cps = 2.5 + std::min<DWORD>(s, 31) * (30.0 - 2.5) / 31.0;
    return (UINT)(1000.0 / cps);
}

void stopRepeat() {
    if (repeatTimer) KillTimer(nullptr, repeatTimer);
    repeatTimer = 0;
    repeatKey = 0;
    repeating = false;
}

void CALLBACK repeatProc(HWND, UINT, UINT_PTR id, DWORD) {
    Sc s;
    if (id != repeatTimer || !heldKeys.count(repeatKey) || !evdevToScan(repeatKey, s)) {
        KillTimer(nullptr, id);
        if (id == repeatTimer) stopRepeat();
        return;
    }
    if (!repeating) {
        // First fire after the initial delay: switch to the repeat rate.
        KillTimer(nullptr, repeatTimer);
        repeatTimer = SetTimer(nullptr, 0, repeatIntervalMs(), repeatProc);
        repeating = true;
    }
    sendKey(s, true);
}

void startRepeat(uint32_t e) {
    stopRepeat();
    repeatKey = e;
    repeatTimer = SetTimer(nullptr, 0, repeatDelayMs(), repeatProc);
}
}  // namespace

void key(uint32_t e, bool down) {
    Sc s;
    if (!evdevToScan(e, s)) return;
    if (down) heldKeys.insert(e);
    else if (!heldKeys.erase(e)) return;
    sendKey(s, down);
    if (down && !noRepeat(e)) startRepeat(e);   // the newest key repeats, like a real keyboard
    else if (!down && e == repeatKey) stopRepeat();
}

void leave() {
    stopRepeat();
    for (uint32_t k : std::set<uint32_t>(heldKeys)) key(k, false);
    for (int b : std::set<int>(heldButtons)) button(b, false);
    controlled = false;
    quietUntil = nowMs() + 400;
}
}  // namespace input
