#pragma once
#include "json.h"
#include <cstdint>
#include <string>

namespace direct {
using Injector = void (*)(const Json &);
void connected(Injector inject);
void hub(const std::string &json);
bool input(const std::string &json);
void send(const std::string &json); // direct input if supported, otherwise legacy hub input
void drain();                     // UI thread, ordered native handoff
void disconnect();
void shutdown();
uint64_t nowUs();
void nativeEvent(const std::string &json, uint64_t generation); // RFCOMM worker to TLS core
}
