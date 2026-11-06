// Runs the production receiver executor without OS input or network access.
// The UI deliberately never pumps: remote releases must still be processed.
#undef NDEBUG
#include "../lzc-mesh-client/windows/src/app.h"
#include "../lzc-mesh-client/windows/src/receiver.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>

App g;
namespace {
std::mutex observedMutex;
std::vector<std::string> observed;
std::atomic<bool> receiving{false};
std::atomic<uint64_t> receiveEpoch{0};
DWORD controller;
void record(const std::string &s) {
    assert(GetCurrentThreadId() != controller);
    std::lock_guard<std::mutex> lock(observedMutex); observed.push_back(s);
}
Json parse(const std::string &s) { Json v; assert(Json::parse(s, v)); return v; }
void sync() {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    assert(receiver::defer([=] { SetEvent(done); }));
    assert(WaitForSingleObject(done, 3000) == WAIT_OBJECT_0); CloseHandle(done);
}
void clear() { sync(); std::lock_guard<std::mutex> lock(observedMutex); observed.clear(); }
std::vector<std::string> snapshot() { sync(); std::lock_guard<std::mutex> lock(observedMutex); return observed; }
void native(const char *text, uint64_t generation = 1) { receiver::native(parse(text), generation); }
void state(bool enabled = true, const char *id = "one") {
    receiver::hub(parse(std::string("{\"t\":\"state\",\"devices\":[{\"id\":\"fixture\",\"enabled\":") + (enabled ? "true" : "false") +
        ",\"trusted\":true}],\"settings\":{\"input_enabled\":true},\"active\":{\"control_id\":\"" + id + "\"}}"), 1);
}
void begin(const char *scope = "one", uint64_t generation = 1) {
    native((std::string("{\"t\":\"direct_active\",\"source\":\"peer\",\"target\":\"fixture\",\"_control_session\":\"") + scope + "\"}").c_str(), generation);
    native((std::string("{\"t\":\"enter\",\"_control_session\":\"") + scope + "\"}").c_str(), generation);
}
}
namespace input {
void cancelCapture() {}
void warp(double,double) { record("warp"); }
void leave() { receiving = false; record("leave"); }
void enter(double, double) { receiving = true; ++::receiveEpoch; record("enter"); }
bool receiving() { return ::receiving; }
uint64_t receiveEpoch() { return ::receiveEpoch; }
void completeLocalActivity(uint64_t, bool replay) { record(replay ? "replay" : "discard"); }
void move(double x, double) { record(x == .1 ? "move1" : "move2"); }
void button(int, bool down) { record(down ? "down" : "up"); }
void key(uint32_t, bool down) { record(down ? "key_down" : "key_up"); }
void wheel(double, double) { record("wheel"); }
}
namespace direct {
void applied(const Json &) { record("applied"); }
void send(const std::string &) { record("takeover"); }
}
int main() {
    controller = GetCurrentThreadId(); g.deviceId = "fixture";
    receiver::start(); receiver::bindHub(1); receiver::bindNative(1); receiver::setEnabled(true); state(); sync();
    clear(); begin();
    native(R"({"t":"move","x":0.1,"_control_session":"one"})");
    native(R"({"t":"button","button":1,"down":true,"_control_session":"one"})");
    native(R"({"t":"move","x":0.2,"_control_session":"one"})");
    native(R"({"t":"button","button":1,"down":false,"_control_session":"one"})");
    native(R"({"t":"key","code":30,"down":true,"_control_session":"one"})");
    native(R"({"t":"key","code":30,"down":false,"_control_session":"one"})");
    assert((snapshot() == std::vector<std::string>{"leave","enter","move1","down","move2","up","key_down","key_up"}));

    // Wrong scopes/generations and a canceled receive cannot replay old input.
    clear(); native(R"({"t":"button","down":true,"_control_session":"old"})");
    native(R"({"t":"button","down":true,"_control_session":"one"})", 99);
    assert(snapshot().empty());
    receiver::localActivity(::receiveEpoch); sync();
    assert((snapshot() == std::vector<std::string>{"leave","takeover","replay"}));
    clear(); begin(); native(R"({"t":"button","down":true,"_control_session":"one"})"); assert(snapshot().empty());
    begin("two"); sync(); assert(input::receiving());
    clear(); state(false); sync(); assert(!input::receiving());
    clear(); native(R"({"t":"button","down":true,"_control_session":"two"})"); assert(snapshot().empty());

    // Legacy hub input also bypasses UI; connection generations fence it.
    receiver::setEnabled(true); state(true, "legacy"); sync(); clear();
    receiver::hub(parse(R"({"t":"enter"})"), 1);
    receiver::hub(parse(R"({"t":"button","down":true})"), 1);
    receiver::hub(parse(R"({"t":"button","down":false})"), 1);
    assert((snapshot() == std::vector<std::string>{"leave","enter","down","up"}));
    // The legacy hub emits enter before broadcasting its new control ID.
    clear(); receiver::hub(parse(R"({"t":"enter"})"), 1); state(true, "legacy-next"); sync();
    assert(input::receiving());
    clear(); receiver::closeHub(1); sync(); assert(!input::receiving());
    clear(); receiver::hub(parse(R"({"t":"button","down":true})"), 1); assert(snapshot().empty());

    // Queue saturation fails closed and releases, rather than dropping an up.
    receiver::bindHub(1); receiver::setEnabled(true); state(); sync();
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr), unblock = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    receiver::defer([=] { SetEvent(entered); assert(WaitForSingleObject(unblock, 3000) == WAIT_OBJECT_0); });
    assert(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0);
    for (int i=0; i<600; ++i) native(R"({"t":"button","down":true,"_control_session":"one"})");
    SetEvent(unblock); sync(); assert(!input::receiving()); CloseHandle(entered); CloseHandle(unblock);
    clear(); receiver::setEnabled(true); begin("after-overflow"); native(R"({"t":"button","down":true,"_control_session":"after-overflow"})"); assert(snapshot().empty());
    receiver::bindHub(2); receiver::setEnabled(true);
    receiver::hub(parse(R"({"t":"state","devices":[{"id":"fixture","enabled":true,"trusted":true}],"settings":{"input_enabled":true},"active":{"control_id":"new"}})"),2);
    receiver::bindNative(2); sync(); clear();
    begin("new", 1); assert(snapshot().empty()); // retired native runtime
    begin("new", 2); sync(); assert(input::receiving());
    receiver::closeHub(1); sync(); assert(input::receiving()); // retired close
    receiver::hub(parse(R"({"t":"device_auth","status":"revoked"})"),2); sync(); assert(!input::receiving());
    clear(); native(R"({"t":"button","down":true,"_control_session":"new"})",2); assert(snapshot().empty());
    receiver::shutdown();
    puts("PASS: production receiver FIFO without UI pumping, scope/generation rejection, local takeover, pause/disconnect release, legacy relay, bounded overflow");
}
