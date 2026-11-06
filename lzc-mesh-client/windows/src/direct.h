#pragma once
#include "json.h"
#include <cstdint>
#include <string>

namespace direct {
void connected();
void hub(const std::string &json);
uint64_t network(const std::string &json); // returns native generation, 0 if not forwarded
bool isCurrent(uint64_t generation);
void applied(const Json &trace);       // ordered receiver submission acknowledgement
bool input(const std::string &json);
void send(const std::string &json); // direct input if supported, otherwise legacy hub input
void drain();                     // UI thread, ordered native handoff
void disconnect();
void shutdown();
uint64_t nowUs();
void nativeEvent(const std::string &json, uint64_t generation); // RFCOMM worker to TLS core
}
