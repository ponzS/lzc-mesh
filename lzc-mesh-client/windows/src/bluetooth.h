#pragma once
#include "json.h"
#include <cstdint>

namespace bluetooth {
// The worker owns native RFCOMM sockets only. Rust owns TLS and input grants.
void event(const Json &json, uint64_t generation);
void stop();
void shutdown();
}
