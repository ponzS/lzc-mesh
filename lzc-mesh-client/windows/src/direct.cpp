// Native input and protocol acknowledgements bypass the main window's modal
// loops. Only presentation and source-role changes are posted to the UI.
#include "app.h"
#include "direct.h"
#include "bluetooth.h"
#include "input_queue.h"
#include "net.h"
#include "receiver.h"
#include "../../transport/include.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace direct {
namespace {
struct Session {
    MeshDirect *handle = nullptr;
    std::atomic<bool> accepting{true};
    uint64_t generation = 0;
};
struct Event { Json value; std::string key; uint64_t generation; };
std::mutex handleMutex, queueMutex;
std::unique_ptr<Session> session;
std::deque<Event> events;
uint64_t nextGeneration = 0;
std::atomic<bool> legacyAllowed{false};
std::atomic<uint64_t> acceptingGeneration{0};
std::atomic<unsigned> hubGeneration{0};
bool scheduled = false;

void callback(void *context, const char *text) {
    // The callback token is a generation value, not a pointer to a session that
    // may already have been destroyed while its runtime is shutting down.
    const auto generation=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
    if (!generation || acceptingGeneration.load()!=generation || !text) return;
    Json value;
    if (!Json::parse(text, value) || !value.isObj()) return;
    const auto kind = value["_lan"].str();
    if (kind == "inject") { receiver::native(value["message"], generation); return; }
    if (kind == "hub") {
        const auto hub = hubGeneration.load();
        if (acceptingGeneration == generation) hubws::sendFor(hub, value["message"].dump());
        return;
    }
    if (kind.rfind("bt_", 0) == 0) { bluetooth::event(value, generation); return; }
    Event e{std::move(value), "", generation};
    e.key = transportMotionKey(e.value);
    std::lock_guard<std::mutex> lock(queueMutex);
    if (acceptingGeneration.load()!=generation) return;
    if (!e.key.empty() && !events.empty() && events.back().generation == e.generation && events.back().key == e.key)
        events.pop_back();
    events.push_back(std::move(e));
    if (!scheduled) {
        scheduled = true;
        if (!PostMessageW(g.hwnd, WM_APP_DIRECT, 0, 0)) { events.clear(); scheduled = false; }
    }
}
}

void connected() {
    disconnect();
    std::lock_guard<std::mutex> lock(handleMutex);
    session = std::make_unique<Session>();
    session->generation = ++nextGeneration;
    hubGeneration=hubws::generation();
    acceptingGeneration=session->generation;
    receiver::bindNative(session->generation);
    bluetooth::bind(session->generation);
    session->handle = mesh_direct_new(g.deviceId.c_str(), callback, reinterpret_cast<void *>(static_cast<uintptr_t>(session->generation)));
    if (session->handle) mesh_direct_hub(session->handle, "{\"t\":\"hub_connected\"}");
    else {
        acceptingGeneration=0;
        session.reset();
        logLine(L"无法启动原生传输核心；仅可使用兼容中枢转发");
    }
}
void hub(const std::string &json) {
    Json value;
    if (Json::parse(json, value) && value["t"].str() == "state") {
        const auto &settings = value["settings"];
        const auto mode = settings["transport_mode"].str("auto");
        bool enabled = false;
        for (const auto &device : value["devices"].a) if (device["id"].str() == g.deviceId) enabled = device["enabled"].boolean(true) && device["trusted"].boolean(false) && g.authVerified;
        legacyAllowed = enabled && settings["input_enabled"].boolean(true) && (mode == "auto" || mode == "relay");
    }
    std::lock_guard<std::mutex> lock(handleMutex);
    if (session && session->accepting) mesh_direct_hub(session->handle, json.c_str());
}
bool isCurrent(uint64_t generation) { return generation && acceptingGeneration == generation; }
uint64_t network(const std::string &json) {
    Json value;
    if (!Json::parse(json, value)) return false;
    const auto type = value["t"].str();
    if (type != "state" && type.rfind("direct_", 0) != 0) return false;
    std::lock_guard<std::mutex> lock(handleMutex);
    if (!session || !session->accepting || hubGeneration != hubws::generation()) return false; // bootstrap on UI first
    if (type == "state") {
        const auto &settings = value["settings"];
        const auto mode = settings["transport_mode"].str("auto");
        bool enabled = false;
        for (const auto &device : value["devices"].a) if (device["id"].str() == g.deviceId)
            enabled = device["enabled"].boolean(true) && device["trusted"].boolean(false);
        legacyAllowed = enabled && settings["input_enabled"].boolean(true) && (mode == "auto" || mode == "relay");
    }
    mesh_direct_hub(session->handle, json.c_str());
    return session->generation;
}
void applied(const Json &trace) {
    std::lock_guard<std::mutex> lock(handleMutex);
    if (session && session->accepting) mesh_direct_applied(session->handle, trace.dump().c_str());
}
void nativeEvent(const std::string &json, uint64_t generation) {
    std::lock_guard<std::mutex> lock(handleMutex);
    if (session && session->accepting && session->generation == generation)
        mesh_direct_hub(session->handle, json.c_str());
}
bool input(const std::string &json) {
    std::lock_guard<std::mutex> lock(handleMutex);
    return session && session->accepting && mesh_direct_input(session->handle, json.c_str());
}
void send(const std::string &json) {
    if (!input(json) && legacyAllowed) hubws::send(json);
}
uint64_t nowUs() { return mesh_direct_now_us(); }

void drain() {
    // Bounded batches keep paint/keyboard/UI timers responsive during motion.
    for (int n = 0; n < 256; n++) {
        Event e;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (events.empty()) { scheduled = false; return; }
            e = std::move(events.front()); events.pop_front();
        }
        if (!session || !session->accepting || session->generation != e.generation) continue;
        const auto kind = e.value["_lan"].str();
        if (kind == "metrics") logLine(widen(e.value["text"].str()));
        else if (kind == "status") setStatus(S_TRANSPORT, L_OK, widen(e.value["text"].str()));
        else if (kind == "channel_status")
            setStatus(e.value["channel"].str() == "pointer" ? S_POINTER_CHANNEL : S_KEYBOARD_CHANNEL, L_OK, widen(e.value["text"].str()));
    }
    PostMessageW(g.hwnd, WM_APP_DIRECT, 0, 0);
}
void disconnect() {
    acceptingGeneration=0;
    receiver::closeNative();
    bluetooth::stop();
    legacyAllowed = false;
    // Stop accepting old callbacks before destroying a runtime. A later hub
    // connection gets a new generation, so queued input cannot cross accounts.
    {
        std::lock_guard<std::mutex> lock(handleMutex);
        if (session) {
            session->accepting = false;
            mesh_direct_disconnect(session->handle);
            mesh_direct_free(session->handle);
            session.reset();
        }
    }
    std::lock_guard<std::mutex> lock(queueMutex);
    events.clear(); scheduled = false;
}
void shutdown() { disconnect(); bluetooth::shutdown(); }
}
