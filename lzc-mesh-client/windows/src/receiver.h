// Ordered receive-side input executor. Never waits for the main window to pump.
#pragma once
#include "json.h"
#include <cstdint>
#include <functional>

namespace receiver {
struct Notice { Json message; uint64_t epoch; };
void start();
void shutdown();
void setEnabled(bool enabled); // UI-owned sharing/auth decision, copied atomically
void bindHub(unsigned generation);
void closeHub(unsigned generation);
void bindNative(uint64_t generation);
void closeNative();
// Called directly on the authenticated network / native transport callbacks.
// Returns true for legacy input messages consumed here instead of by the UI.
bool hub(const Json &message, unsigned generation);
void native(const Json &message, uint64_t generation);
void localActivity(uint64_t receiveEpoch); // hook thread: enqueue only
bool current(const Notice &notice);
bool sourceCurrent(uint64_t role);
bool acceptingInput(); // repeat timers must stop on revoke before queued cleanup
// Marshal low-level injection helpers and their timers to this executor. A
// false result means the caller already is the executor and should run inline.
bool defer(std::function<void()> task);
}
