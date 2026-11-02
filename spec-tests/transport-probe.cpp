// Stdio adapter to the production transport, used by the integration coordinator.
// It has no desktop injection or authentication bypass in the shipped client.
#include "../lzc-mesh-client/transport/include.h"
#include "../lzc-mesh-client/windows/src/json.h"
#include <iostream>
#include <mutex>
std::mutex output;
void callback(void *, const char *text) { std::lock_guard<std::mutex> lock(output); std::cout << text << std::endl; }
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    auto *handle = mesh_direct_new(argv[1], callback, nullptr);
    if (!handle) return 1;
    std::string line;
    while (std::getline(std::cin, line)) {
        Json value;
        if (!Json::parse(line, value)) continue;
        auto message = value["message"].dump();
        if (value["_test"].str() == "input") mesh_direct_input(handle, message.c_str());
        else if (value["_test"].str() == "applied") mesh_direct_applied(handle, message.c_str());
        else mesh_direct_hub(handle, line.c_str());
    }
    mesh_direct_free(handle);
}
