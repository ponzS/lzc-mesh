#include "app.h"
#include "receiver.h"
#include "input.h"
#include "direct.h"
#include "../../transport/input_gate.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace receiver {
namespace {
enum class Kind { Input, State, Reset, Local, Task };
struct Work {
    Kind kind = Kind::Input;
    Json message;
    uint64_t epoch = 0, local = 0;
    std::function<void()> task;
    Work() = default;
    Work(Kind kind, Json message = {}, uint64_t epoch = 0, uint64_t local = 0)
        : kind(kind), message(std::move(message)), epoch(epoch), local(local) {}
};
std::mutex mutex;
std::condition_variable ready;
std::deque<Work> queue;
std::thread worker;
HANDLE wake = nullptr;
bool running = false;
thread_local bool executing = false;
std::atomic<uint64_t> epoch{1}, nativeGeneration{0}, roleRevision{0};
std::atomic<bool> sourceDesired{false};
std::atomic<unsigned> hubGeneration{0};
std::atomic<bool> enabled{false}, remoteAllowed{false};
bool overflowed = false; // protected by mutex; cleared only for a new hub
thread_local uint64_t applyingEpoch = 0;
// Only the executor owns these strings and the gate. Never read App from here.
MeshInputGate gate{};
std::string control, blocked;
bool legacy = false;

void notify(Json value) {
    auto *notice = new Notice{std::move(value), applyingEpoch ? applyingEpoch : epoch.load()};
    if (!PostMessageW(g.hwnd, WM_APP_RECEIVER, 0, (LPARAM)notice)) delete notice;
}
void post(const char *type) { Json value; Json::parse(JsonOut().s("t", type).str(), value); notify(std::move(value)); }
void reset() {
    input::leave();
    input::completeLocalActivity(input::receiveEpoch(), false);
    gate = {}; control.clear(); blocked.clear(); legacy = false;
}
void invalidateLocked() {
    ++epoch; ++roleRevision; sourceDesired = false;
    input::cancelCapture();
    queue.clear();
    queue.push_back(Work{Kind::Reset});
    if (wake) SetEvent(wake);
}
void pushLocked(Work work) {
    if (!running) return;
    // Absolute positions may replace only adjacent positions in the same scope.
    if (work.kind == Kind::Input && work.message["t"].str() == "move" && !queue.empty()) {
        const auto &back = queue.back();
        if (back.kind == Kind::Input && back.epoch == work.epoch && back.message["t"].str() == "move" &&
            back.message["_control_session"].str() == work.message["_control_session"].str()) queue.pop_back();
    }
    if (queue.size() >= 512) {
        // Do not lose an up while keeping its down or replay an old backlog.
        enabled = false; overflowed = true;
        invalidateLocked();
        post("receiver_overflow");
        return;
    }
    queue.push_back(std::move(work));
    SetEvent(wake);
}
void roleNotice(const Json &message, bool source = false) {
    sourceDesired = source;
    Json notice = message;
    Json revision; revision.type = Json::Num; revision.n = (double)++roleRevision;
    notice.o["_receiver_role"] = std::move(revision);
    notify(std::move(notice));
}
void apply(const Json &o) {
    const auto type = o["t"].str(), scope = o["_control_session"].str();
    const bool begin = type == "direct_active" && !o["source"].str().empty();
    if (!(begin ? mesh_input_gate_begin(&gate, scope.c_str()) : mesh_input_gate_accept(&gate, scope.c_str()))) return;
    if (scope.empty() && !blocked.empty() && blocked == control) return;
    if (type == "direct_active") {
        if (begin) {
            if (!enabled || !remoteAllowed) return;
            control = scope; legacy = false;
        }
        notify(o);
    } else if (type == "leave") {
        input::leave(); roleNotice(o);
    } else if (type == "release" || type == "capture") {
        if (type == "capture" && (!enabled || !remoteAllowed)) return;
        // Sender capture / overlay remains on the UI. Receiver state must be
        // released first, before a new source role can be announced there.
        input::leave();
        if (type == "release") {
            input::cancelCapture();
            if (o["warp"].boolean()) input::warp(o["x"].num(.5), o["y"].num(.5));
        }
        roleNotice(o, type == "capture");
    } else if (enabled && remoteAllowed) {
        if (type == "enter") {
            legacy = scope.empty();
            if (legacy) control.clear(); // hub sends enter before its new state
            input::leave();
            input::enter(o["x"].num(.5), o["y"].num(.5));
            roleNotice(o);
        } else if (input::receiving()) {
            if (type == "move") input::move(o["x"].num(.5), o["y"].num(.5));
            else if (type == "button") input::button((int)o["button"].num(1), o["down"].boolean());
            else if (type == "wheel") input::wheel(o["dx"].num(), o["dy"].num());
            else if (type == "key") input::key((uint32_t)o["code"].num(), o["down"].boolean());
            if (o["_motion_trace"].isObj()) direct::applied(o["_motion_trace"]);
        }
    }
}
void run() {
    executing = true;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    { std::lock_guard<std::mutex> lock(mutex); running = true; }
    ready.notify_one();
    for (;;) {
        Work work;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running) break;
            if (!queue.empty()) { work = std::move(queue.front()); queue.pop_front(); found = true; }
            else ResetEvent(wake);
        }
        if (found) {
            applyingEpoch = work.epoch;
            if (work.kind == Kind::Reset) reset();
            else if (work.epoch != epoch) { applyingEpoch = 0; continue; }
            else if (work.kind == Kind::Task) work.task();
            else if (work.kind == Kind::State) {
                const auto next = work.message["active"]["control_id"].str();
                if (legacy && !control.empty() && next != control) input::leave();
                control = next;
            } else if (work.kind == Kind::Local) {
                if (work.local != input::receiveEpoch()) { input::completeLocalActivity(work.local, false); applyingEpoch = 0; continue; }
                mesh_input_gate_cancel(&gate); blocked = control;
                input::leave();
                if (!control.empty()) direct::send(JsonOut().s("t", "local_activity").s("control_id", control).str());
                input::completeLocalActivity(work.local, true);
                Json notice; Json::parse("{\"t\":\"local_activity\"}", notice); roleNotice(notice);
            } else apply(work.message);
            applyingEpoch = 0;
        } else MsgWaitForMultipleObjectsEx(1, &wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        // Repeat-key timers live here too; never starve them behind motion.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
    reset();
    executing = false;
}
bool inputType(const std::string &t) {
    return t == "direct_active" || t == "enter" || t == "leave" || t == "capture" ||
           t == "release" || t == "move" || t == "button" || t == "wheel" || t == "key";
}
}
void start() {
    std::unique_lock<std::mutex> lock(mutex);
    if (worker.joinable()) return;
    wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake) return; // sharing remains fail-closed if executor cannot start
    worker = std::thread(run);
    ready.wait(lock, [] { return running; });
}
void shutdown() {
    { std::lock_guard<std::mutex> lock(mutex); if (!worker.joinable()) return; enabled = false; ++epoch; running = false; queue.clear(); SetEvent(wake); }
    worker.join(); CloseHandle(wake); wake = nullptr;
}
void setEnabled(bool on) {
    std::lock_guard<std::mutex> lock(mutex);
    on = on && !overflowed;
    if (enabled.exchange(on) && !on) invalidateLocked();
}
void bindHub(unsigned generation) {
    std::lock_guard<std::mutex> lock(mutex);
    hubGeneration = generation; remoteAllowed = false; overflowed = false;
    invalidateLocked();
}
void closeHub(unsigned generation) {
    std::lock_guard<std::mutex> lock(mutex);
    if (hubGeneration != generation) return;
    hubGeneration = 0; remoteAllowed = false; enabled = false; invalidateLocked();
}
void bindNative(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex); nativeGeneration = generation;
}
void closeNative() {
    std::lock_guard<std::mutex> lock(mutex);
    if (nativeGeneration.exchange(0)) invalidateLocked();
}
bool hub(const Json &value, unsigned generation) {
    const auto type = value["t"].str();
    std::lock_guard<std::mutex> lock(mutex);
    if (hubGeneration != generation) return inputType(type);
    if (type == "state") {
        bool allowed = false;
        for (const auto &device : value["devices"].a) if (device["id"].str() == g.deviceId)
            allowed = device["enabled"].boolean(true) && device["trusted"].boolean(false) && device["sharing"].boolean(true);
        allowed = allowed && value["settings"]["input_enabled"].boolean(true);
        if (remoteAllowed.exchange(allowed) && !allowed) invalidateLocked();
        pushLocked(Work{Kind::State, value, epoch.load()});
    } else if (type == "device_auth" && value["status"].str() != "approved") {
        remoteAllowed = false; enabled = false; invalidateLocked();
    } else if (inputType(type)) {
        if ((enabled && remoteAllowed) || type == "leave" || type == "release")
            pushLocked(Work{Kind::Input, value, epoch.load()});
        return true;
    }
    return false;
}
void native(const Json &value, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto type = value["t"].str();
    if (generation && nativeGeneration == generation &&
        ((enabled && remoteAllowed) || type == "leave" || type == "release" ||
         (type == "direct_active" && value["source"].str().empty())))
        pushLocked(Work{Kind::Input, value, epoch.load()});
}
void localActivity(uint64_t local) {
    std::lock_guard<std::mutex> lock(mutex);
    pushLocked(Work{Kind::Local, {}, epoch.load(), local});
}
bool current(const Notice &notice) {
    const auto &role = notice.message["_receiver_role"];
    return notice.epoch == epoch && (role.type == Json::Null || (uint64_t)role.num() == roleRevision);
}
bool sourceCurrent(uint64_t role) { return sourceDesired && role == roleRevision; }
bool acceptingInput() { return enabled && remoteAllowed; }
bool defer(std::function<void()> task) {
    if (executing) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!running) return true; // never fall back to UI-thread injection
    Work work; work.kind = Kind::Task; work.epoch = epoch; work.task = std::move(task);
    pushLocked(std::move(work));
    return true;
}
}
