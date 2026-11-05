#include "app.h"
#include "input.h"
#include "direct.h"
#include "input_queue.h"
#include "pointer_math.h"
#include "capture_pointer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <thread>

#include "app.h"
#include "net.h"

namespace input {
namespace {
const ULONG_PTR kMarker = 0x4C5A4D45;  // dwExtraInfo of events we inject

std::atomic<bool> captured{false}, hubOn{false}, sharingOn{true};
// The low bit is receiver ownership; the remaining bits bind a hardware event
// to the particular enter, so a queued takeover cannot cancel a later enter.
std::atomic<uint64_t> receiverState{0};
std::atomic<uint64_t> mappedKeys{0}, unmappedKeys{0};
constexpr UINT rearmHooks = WM_APP + 41;
std::mutex localMutex;
std::deque<INPUT> localPending;
uint64_t localEpoch = 0;
bool localOverflow = false;
void sendInputs(INPUT *in, UINT n);
bool receiverOn() { return (receiverState.load() & 1) != 0; }
void physicalActivity() {
    auto state = receiverState.load();
    if ((state & 1) && receiverState.compare_exchange_strong(state, state & ~uint64_t(1))) {
        std::lock_guard<std::mutex> lock(localMutex);
        localEpoch = state >> 1; localPending.clear(); localOverflow = false;
        PostMessageW(g.hwnd, WM_APP_INPUT, 4, (LPARAM)localEpoch);
    }
}
std::atomic<uint64_t> captureEpoch{0};
SourcePointerGate sourcePointer;
std::atomic<DWORD> captureBegan{0};
std::atomic<uint64_t> parkObserved{0};
std::atomic<DWORD> parkObservedFlags{0};
UINT_PTR parkTimer=0;
uint64_t parkTimerEpoch=0,parkDeadline=0;
POINT parkExpected{};  // UI thread only, including the timer callback
void CALLBACK parkTimeout(HWND,UINT,UINT_PTR id,DWORD) {
    if (id!=parkTimer || GetTickCount64()<parkDeadline) return;
    KillTimer(nullptr,id);parkTimer=0;
    if (captured && captureEpoch==parkTimerEpoch) {
        POINT actual{};
        GetCursorPos(&actual);
        logLine(fmt(L"pointer_park ready=%d observed=%llu flags=%lu actual=(%ld,%ld)",sourcePointer.ready(parkTimerEpoch)?1:0,(unsigned long long)parkObserved.load(),parkObservedFlags.load(),actual.x,actual.y));
        if (!sourcePointer.ready(parkTimerEpoch) || !GetCursorPos(&actual) || actual.x!=parkExpected.x || actual.y!=parkExpected.y) {
            captured=false;sourcePointer.end();
            PostMessageW(g.hwnd,WM_APP_INPUT,3,(LPARAM)parkTimerEpoch);
        }
    }
}
std::atomic<bool> edges[4];  // left right up down
std::atomic<long long> quietUntil{0}, lastEdge{0};
std::atomic<int> vx{0}, vy{0}, vw{0}, vh{0};
std::atomic<double> scale{1.0};
HHOOK mouseHook, keyHook;
DWORD hookThreadId;
std::thread hookThread, sendThread;
std::mutex hookMutex;
std::condition_variable hookReady;
bool hookStarted = false, started = false;
HWND overlay;

// ---- outgoing queue: hook callbacks must return fast, so a sender thread does the network I/O.
std::mutex qmu;
std::condition_variable qcv;
InputQueue queue;
std::string sourceSession;  // protected by qmu, like the capture queue
bool running = false;

void checkQueueLimit() {
    if (queue.size() <= 512) return;
    queue.clear();
    captured = false; sourcePointer.end();
    const auto failedEpoch=++captureEpoch;
    PostMessageW(g.hwnd, WM_APP_INPUT, 3, (LPARAM)failedEpoch);
}
void enqueue(std::string s, uint64_t epoch) {
    {
        std::lock_guard<std::mutex> l(qmu);
        if (!captured || captureEpoch!=epoch || !sourcePointer.ready(epoch)) return;
        queue.event(std::move(s)); checkQueueLimit();
    }
    qcv.notify_one();
}
void enqueueMotion(double dx, double dy, uint64_t epoch) {
    const auto timestamp = direct::nowUs();
    {
        std::lock_guard<std::mutex> l(qmu);
        if (!captured || captureEpoch!=epoch || !sourcePointer.ready(epoch)) return;
        queue.motion(dx, dy, timestamp); checkQueueLimit();
    }
    qcv.notify_one();
}
void sender() {
    std::unique_lock<std::mutex> l(qmu);
    while (true) {
        qcv.wait(l, [] { return !running || !queue.empty(); });
        if (!running) break;
        const auto epoch = captureEpoch.load();
        auto frame = queue.take(sourceSession);
        l.unlock();
        if (captured && captureEpoch == epoch) direct::send(frame);
        l.lock();
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

// Hook-thread-only forwarded state. Seed modifiers on the first captured
// hardware event so a modifier held while crossing also reaches the target.
std::set<uint32_t> sourceHeld;
uint64_t sourceHeldEpoch=0;
void seedModifiers(uint64_t epoch) {
    if(sourceHeldEpoch==epoch)return;
    sourceHeld.clear();sourceHeldEpoch=epoch;
    const struct {int vk;uint32_t code;} modifiers[]={{VK_LCONTROL,29},{VK_RCONTROL,97},{VK_LMENU,56},{VK_RMENU,100},{VK_LSHIFT,42},{VK_RSHIFT,54},{VK_LWIN,125},{VK_RWIN,126}};
    for(const auto &key:modifiers)if(GetAsyncKeyState(key.vk)&0x8000) {
        sourceHeld.insert(key.code);
        enqueue(JsonOut().s("t","key").n("code",key.code).b("down",true).str(),epoch);
    }
}

// ---- hooks

bool deferLocal(INPUT event) {
    std::lock_guard<std::mutex> lock(localMutex);
    if (!localEpoch) return false;
    const DWORD motion = MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    if (event.type==INPUT_MOUSE && event.mi.dwFlags==motion && !localPending.empty() && localPending.back().type==INPUT_MOUSE && localPending.back().mi.dwFlags==motion)
        localPending.back()=event;
    else if (!localOverflow) {
        if (localPending.size()>=512) {localPending.clear();localOverflow=true;}
        else localPending.push_back(event);
    }
    return true;
}
bool deferMouse(const MSLLHOOKSTRUCT &m, WPARAM message) {
    INPUT event{};event.type=INPUT_MOUSE;
    event.mi.dx=absolutePixel(double(m.pt.x-vx)/std::max(1,(int)vw),vw);
    event.mi.dy=absolutePixel(double(m.pt.y-vy)/std::max(1,(int)vh),vh);
    event.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    switch(message) {
    case WM_MOUSEMOVE:break;
    case WM_LBUTTONDOWN:event.mi.dwFlags|=MOUSEEVENTF_LEFTDOWN;break;
    case WM_LBUTTONUP:event.mi.dwFlags|=MOUSEEVENTF_LEFTUP;break;
    case WM_RBUTTONDOWN:event.mi.dwFlags|=MOUSEEVENTF_RIGHTDOWN;break;
    case WM_RBUTTONUP:event.mi.dwFlags|=MOUSEEVENTF_RIGHTUP;break;
    case WM_MBUTTONDOWN:event.mi.dwFlags|=MOUSEEVENTF_MIDDLEDOWN;break;
    case WM_MBUTTONUP:event.mi.dwFlags|=MOUSEEVENTF_MIDDLEUP;break;
    case WM_XBUTTONDOWN:case WM_XBUTTONUP:event.mi.dwFlags|=message==WM_XBUTTONDOWN?MOUSEEVENTF_XDOWN:MOUSEEVENTF_XUP;event.mi.mouseData=HIWORD(m.mouseData);break;
    case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:event.mi.dwFlags|=message==WM_MOUSEWHEEL?MOUSEEVENTF_WHEEL:MOUSEEVENTF_HWHEEL;event.mi.mouseData=(DWORD)(LONG)(SHORT)HIWORD(m.mouseData);break;
    default:return false;
    }
    return deferLocal(event);
}

void edgeCheck(POINT pt) {
    if (!hubOn || !sharingOn || receiverOn() || nowMs() < quietUntil || nowMs() - lastEdge < 250) return;
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
    if (SourcePointerGate::isParkMarker(m->dwExtraInfo)) {parkObserved=m->dwExtraInfo;parkObservedFlags=m->flags;}
    if ((m->flags & LLMHF_INJECTED) && SourcePointerGate::isParkMarker(m->dwExtraInfo)) {
        const auto epoch=captureEpoch.load();
        if (!captured || m->dwExtraInfo!=SourcePointerGate::parkMarker(epoch)) return 1;
        if (!sourcePointer.parked(epoch)) return 1;
        captureBegan=m->time;
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if ((m->flags & LLMHF_INJECTED) && m->dwExtraInfo == kMarker) {
        // A queued old receiver move must not displace a new source's cursor.
        if (captured && wp==WM_MOUSEMOVE) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (!(m->flags & LLMHF_INJECTED)) {
        physicalActivity();
        if (deferMouse(*m,wp)) return 1;
    }
    const auto epoch=captureEpoch.load();
    if (!captured) {
        if (!(m->flags & LLMHF_INJECTED) && wp == WM_MOUSEMOVE) edgeCheck(m->pt);
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    if (!sourcePointer.ready(epoch) || (int32_t)(m->time-captureBegan.load())<0) return 1;
    // Capturing: swallow local movement, after our marked park has passed.
    seedModifiers(epoch);
    double s = scale;
    switch (wp) {
    case WM_MOUSEMOVE: {
        POINT actual{};
        if (!GetCursorPos(&actual)) return 1;
        const double dx=capturedPointerDelta(m->pt.x,actual.x,s),dy=capturedPointerDelta(m->pt.y,actual.y,s);
        if (dx != 0 || dy != 0) enqueueMotion(dx, dy, epoch);
        break;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: enqueue(JsonOut().s("t", "button").n("button", 1).b("down", wp == WM_LBUTTONDOWN).str(),epoch); break;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: enqueue(JsonOut().s("t", "button").n("button", 2).b("down", wp == WM_RBUTTONDOWN).str(),epoch); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: enqueue(JsonOut().s("t", "button").n("button", 3).b("down", wp == WM_MBUTTONDOWN).str(),epoch); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
        int b = HIWORD(m->mouseData) == XBUTTON1 ? 4 : 5;
        enqueue(JsonOut().s("t", "button").n("button", b).b("down", wp == WM_XBUTTONDOWN).str(),epoch);
        break;
    }
    case WM_MOUSEWHEEL: enqueue(JsonOut().s("t", "wheel").n("dx", 0).n("dy", (short)HIWORD(m->mouseData)).s("source", "wheel").str(),epoch); break;
    case WM_MOUSEHWHEEL: enqueue(JsonOut().s("t", "wheel").n("dx", -(short)HIWORD(m->mouseData)).n("dy", 0).s("source", "wheel").str(),epoch); break;
    }
    return 1;
}

LRESULT CALLBACK keyProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    auto *k = (KBDLLHOOKSTRUCT *)lp;
    if ((k->flags & LLKHF_INJECTED) && k->dwExtraInfo == kMarker) return CallNextHookEx(nullptr, code, wp, lp);
    if (!(k->flags & LLKHF_INJECTED)) {
        physicalActivity();
        INPUT event{};event.type=INPUT_KEYBOARD;
        event.ki.wScan=(WORD)k->scanCode;event.ki.wVk=k->scanCode?0:(WORD)k->vkCode;
        event.ki.dwFlags=(k->scanCode?KEYEVENTF_SCANCODE:0)|((k->flags&LLKHF_EXTENDED)?KEYEVENTF_EXTENDEDKEY:0)|((wp==WM_KEYUP||wp==WM_SYSKEYUP)?KEYEVENTF_KEYUP:0);
        if (deferLocal(event)) return 1;
    }
    const auto epoch=captureEpoch.load();
    if (!captured) return CallNextHookEx(nullptr, code, wp, lp);
    if (!sourcePointer.ready(epoch) || (int32_t)(k->time-captureBegan.load())<0) return 1;
    seedModifiers(epoch);
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    uint32_t e = scanToEvdev((uint16_t)k->scanCode, k->flags & LLKHF_EXTENDED);
    if (!e) {
        // Some keys arrive without a scan code; fall back to the virtual key.
        UINT sc = MapVirtualKeyW(k->vkCode, MAPVK_VK_TO_VSC_EX);
        e = scanToEvdev(sc & 0xFF, (sc & 0xFF00) == 0xE000);
    }
    auto &held=sourceHeld;
    if (e) {
        ++mappedKeys;
        // Auto-repeat would look like extra key presses on the target: forward only state changes.
        bool was = held.count(e) > 0;
        if (down && !was) { held.insert(e); enqueue(JsonOut().s("t", "key").n("code", e).b("down", true).str(),epoch); }
        if (!down && was) { held.erase(e); enqueue(JsonOut().s("t", "key").n("code", e).b("down", false).str(),epoch); }
    }
    if (!e) ++unmappedKeys;
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
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // Ensure a queue exists before stop can post WM_QUIT.
    { std::lock_guard<std::mutex> lock(hookMutex); hookStarted = true; }
    hookReady.notify_one();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == rearmHooks) {
            // Windows can silently remove either timed-out low-level hook.
            // Re-arm both on their owning thread at every new source role.
            if (mouseHook) UnhookWindowsHookEx(mouseHook);
            if (keyHook) UnhookWindowsHookEx(keyHook);
            mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
            keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, GetModuleHandleW(nullptr), 0);
            { std::lock_guard<std::mutex> guard(hookMutex); hookStarted = true; }
            hookReady.notify_one();
        }
    }
    if (mouseHook) UnhookWindowsHookEx(mouseHook);
    if (keyHook) UnhookWindowsHookEx(keyHook);
}

// ---- injection

std::set<uint32_t> heldKeys;
std::set<int> heldButtons;
bool blockedReported = false;
WheelRemainder wheelX, wheelY;

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
    if (started) return;
    started = true;
    { std::lock_guard<std::mutex> lock(hookMutex); hookStarted = false; }
    refreshScreen();
    running = true;
    sendThread = std::thread(sender);
    hookThread = std::thread(hookLoop);
    std::unique_lock<std::mutex> lock(hookMutex);
    hookReady.wait(lock, [] { return hookStarted; });
}

void stop() {
    if (!started) return;
    started = false;
    if (captured) capture(false);
    leave();
    if (hookThreadId) PostThreadMessageW(hookThreadId, WM_QUIT, 0, 0);
    if (hookThread.joinable()) hookThread.join();
    hookThreadId = 0;
    { std::lock_guard<std::mutex> l(qmu); running = false; }
    qcv.notify_all();
    if (sendThread.joinable()) sendThread.join();
}

void setEdges(bool l, bool r, bool u, bool d) { edges[0] = l; edges[1] = r; edges[2] = u; edges[3] = d; }
void setHubConnected(bool on) { hubOn = on; }
void setSharing(bool on) { sharingOn = on; }

void capture(bool on, const std::string &session) {
    const bool wasCaptured=captured.exchange(false);
    if (parkTimer) {KillTimer(nullptr,parkTimer);parkTimer=0;}
    sourcePointer.end();
    const auto epoch=++captureEpoch;
    { std::lock_guard<std::mutex> lock(qmu); queue.clear(); sourceSession = on ? session : ""; }
    if (on) {
        receiverState.fetch_and(~uint64_t(1));
        mappedKeys = unmappedKeys = 0;
        {
            std::unique_lock<std::mutex> lock(hookMutex);
            hookStarted = false;
            const bool posted = PostThreadMessageW(hookThreadId, rearmHooks, 0, 0);
            if (!posted || !hookReady.wait_for(lock, std::chrono::milliseconds(200), [] { return hookStarted; }) || !keyHook || !mouseHook) {
                PostMessageW(g.hwnd, WM_APP_INPUT, 3, (LPARAM)epoch);
                return;
            }
        }
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof mi};
        if (!GetMonitorInfoW(mon, &mi)) {PostMessageW(g.hwnd,WM_APP_INPUT,3,(LPARAM)epoch);return;}
        const POINT anchor{(mi.rcMonitor.left + mi.rcMonitor.right) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2};
        showOverlay(true);
        INPUT park{};park.type=INPUT_MOUSE;
        park.mi.dx=absolutePixel(double(anchor.x-vx)/std::max(1,(int)vw),vw);
        park.mi.dy=absolutePixel(double(anchor.y-vy)/std::max(1,(int)vh),vh);
        park.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
        park.mi.dwExtraInfo=(ULONG_PTR)SourcePointerGate::parkMarker(epoch);
        parkObserved=0;parkObservedFlags=0;
        sourcePointer.begin(epoch);captured=true;
        parkTimerEpoch=epoch;parkDeadline=GetTickCount64()+250;parkExpected=anchor;
        parkTimer=SetTimer(nullptr,0,250,parkTimeout);
        // Unlike unmarked SetCursorPos, our own hook cannot swallow this park
        // as if it were user motion after the source role has already begun.
        if (!parkTimer || SendInput(1,&park,sizeof park)!=1) {
            captured=false;sourcePointer.end();
            PostMessageW(g.hwnd,WM_APP_INPUT,3,(LPARAM)epoch);
        }
        logLine(fmt(L"pointer_capture epoch=%llu park=(%ld,%ld) marked=1",(unsigned long long)epoch,anchor.x,anchor.y));
    } else {
        if (wasCaptured) logLine(fmt(L"keyboard_capture mapped=%llu unmapped=%llu", (unsigned long long)mappedKeys.load(), (unsigned long long)unmappedKeys.load()));
        captured = false;
        showOverlay(false);
        quietUntil = nowMs() + 400;
    }
}

void warp(double nx, double ny) {
    quietUntil = nowMs() + 400;
    move(nx,ny);  // marked, so a late return cannot displace a fresh capture
}

void move(double nx, double ny) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    if (!std::isfinite(nx) || !std::isfinite(ny)) return;
    in.mi.dx = absolutePixel(nx, vw);
    in.mi.dy = absolutePixel(ny, vh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    sendInputs(&in, 1);
}

void completeLocalActivity(uint64_t epoch, bool replay) {
    // A hardware burst may continue while batches are replayed. If a later
    // batch overflows, release any downs already replayed before failing closed.
    std::map<uint64_t,INPUT> recovery;
    for (;;) {
        std::vector<INPUT> batch;
        bool done=false, overflowed=false;
        {
            std::lock_guard<std::mutex> lock(localMutex);
            if (localEpoch!=epoch) return;
            if (!replay || localOverflow || localPending.empty()) {
                done=true;overflowed=localOverflow;
                localPending.clear();localEpoch=0;localOverflow=false;
            } else {batch.assign(localPending.begin(),localPending.end());localPending.clear();}
        }
        if (done) {
            if (overflowed || !replay) {
                std::vector<INPUT> releases;
                for (const auto &entry:recovery) releases.push_back(entry.second);
                if (!releases.empty()) sendInputs(releases.data(),(UINT)releases.size());
            }
            if (overflowed) logLine(L"本机接管输入积压超限，已安全丢弃，未回放按键");
            if (replay && !overflowed) {
                quietUntil=nowMs();
                POINT point{};if(GetCursorPos(&point))edgeCheck(point);
            }
            return;
        }
        for (const auto &event:batch) {
            if (event.type==INPUT_KEYBOARD) {
                uint64_t id=(uint64_t(1)<<48)|(uint64_t(event.ki.wScan)<<16)|event.ki.wVk|((event.ki.dwFlags&KEYEVENTF_EXTENDEDKEY)?(uint64_t(1)<<40):0);
                if(event.ki.dwFlags&KEYEVENTF_KEYUP)recovery.erase(id);
                else {INPUT up=event;up.ki.dwFlags|=KEYEVENTF_KEYUP;recovery[id]=up;}
            } else if(event.type==INPUT_MOUSE) {
                const struct {DWORD down,up;uint64_t id;} buttons[]={{MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_LEFTUP,1},{MOUSEEVENTF_RIGHTDOWN,MOUSEEVENTF_RIGHTUP,2},{MOUSEEVENTF_MIDDLEDOWN,MOUSEEVENTF_MIDDLEUP,3},{MOUSEEVENTF_XDOWN,MOUSEEVENTF_XUP,4}};
                for(const auto &button:buttons) {
                    uint64_t id=(uint64_t(2)<<48)|button.id|(button.id==4?(uint64_t(event.mi.mouseData)<<8):0);
                    if(event.mi.dwFlags&button.up)recovery.erase(id);
                    if(event.mi.dwFlags&button.down){INPUT up{};up.type=INPUT_MOUSE;up.mi.dwFlags=button.up;up.mi.mouseData=event.mi.mouseData;recovery[id]=up;}
                }
            }
        }
        // Never hold localMutex while SendInput invokes low-level callbacks.
        sendInputs(batch.data(),(UINT)batch.size());
    }
}

uint64_t captureGeneration() { return captureEpoch.load(); }
void releaseRemote() {
    std::string id;
    { std::lock_guard<std::mutex> lock(qmu); id = sourceSession; }
    JsonOut message; message.s("t","release");
    if (!id.empty()) message.s("_control_session",id);
    direct::send(message.str());
}

bool receiving() { return receiverOn(); }
uint64_t receiveEpoch() { return receiverState.load() >> 1; }

void enter(double nx, double ny) {
    const auto next = ((receiverState.load() >> 1) + 1) << 1;
    receiverState = next | 1;
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
    const auto y = wheelY.consume(dy), x = wheelX.consume(-dx);
    if (y) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_WHEEL; in[n].mi.mouseData = (DWORD)(LONG)y; n++; }
    if (x) { in[n].type = INPUT_MOUSE; in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL; in[n].mi.mouseData = (DWORD)(LONG)x; n++; }
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
    wheelX.value = wheelY.value = 0;
    for (uint32_t k : std::set<uint32_t>(heldKeys)) key(k, false);
    for (int b : std::set<int>(heldButtons)) button(b, false);
    receiverState.fetch_and(~uint64_t(1));
    quietUntil = nowMs() + 400;
}
}  // namespace input
