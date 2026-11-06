// Native classic Bluetooth RFCOMM. The sockets carry only TLS ciphertext;
// account certificates and active control grants are checked by the Rust core.
#include <winsock2.h>
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <wincrypt.h>
#include "app.h"
#include "bluetooth.h"
#include "direct.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace bluetooth {
namespace {
using Clock = std::chrono::steady_clock;
struct Command { Json value; uint64_t generation; };
std::mutex lifecycleMutex, commandMutex;
uint64_t acceptingGeneration = 0;
std::condition_variable commandReady;
std::deque<Command> commands;
std::thread worker;
bool started = false;
uint64_t connectionNumber = 0;
const GUID serviceId{0xF4174C12,0x5A3B,0x4429,{0xB2,0x65,0x06,0xEE,0xDC,0xEF,0xE2,0x1A}};

std::string encode(const char *data, DWORD size) {
    DWORD n = 0;
    if (!CryptBinaryToStringA((const BYTE *)data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n)) return "";
    std::string out(n, '\0');
    if (!CryptBinaryToStringA((const BYTE *)data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &n)) return "";
    out.resize(n && out[n - 1] == '\0' ? n - 1 : n);
    return out;
}
bool decode(const std::string &s, std::vector<char> &out) {
    if (s.empty() || s.size() > 90000) return false;
    DWORD n = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), flags, nullptr, &n, nullptr, nullptr) || n > 65536) return false;
    out.resize(n);
    return CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), flags, (BYTE *)out.data(), &n, nullptr, nullptr) && n == out.size();
}
std::string addressText(BTH_ADDR address) {
    char text[18];
    snprintf(text, sizeof text, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(address >> 40) & 255, (unsigned)(address >> 32) & 255,
             (unsigned)(address >> 24) & 255, (unsigned)(address >> 16) & 255, (unsigned)(address >> 8) & 255, (unsigned)address & 255);
    return text;
}
bool parseAddress(const std::string &s, BTH_ADDR &address) {
    if (s.size() != 17) return false;
    address = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (i % 3 == 2) { if (s[i] != ':') return false; continue; }
        const char c = s[i];
        int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (n < 0) return false;
        address = (address << 4) | (unsigned)n;
    }
    return address != 0;
}
void closeSocket(SOCKET &s) {
    if (s != INVALID_SOCKET) { ::shutdown(s, SD_BOTH); closesocket(s); s = INVALID_SOCKET; }
}
bool nonblocking(SOCKET socket) { u_long on = 1; return ioctlsocket(socket, FIONBIO, &on) == 0; }
struct Connection {
    SOCKET socket = INVALID_SOCKET;
    std::string peer;
    bool connecting = false;
    Clock::time_point since = Clock::now();
    std::vector<char> output;
    size_t offset = 0;
};
struct State {
    HANDLE radio = nullptr;
    SOCKET listener = INVALID_SOCKET;
    SOCKADDR_BTH local{};
    bool running = false, registered = false;
    uint64_t generation = 0;
    std::map<std::string, Connection> connections;

    void notify(JsonOut message) { direct::nativeEvent(message.str(), generation); }
    void error(const std::string &message) { notify(JsonOut().s("t", "bt_local").s("error", message)); }
    bool service(WSAESETSERVICEOP op) {
        CSADDR_INFO address{};
        address.LocalAddr.lpSockaddr = (SOCKADDR *)&local;
        address.LocalAddr.iSockaddrLength = sizeof local;
        address.iSocketType = SOCK_STREAM;
        address.iProtocol = BTHPROTO_RFCOMM;
        GUID uuid = serviceId;
        WSAQUERYSETW query{};
        query.dwSize = sizeof query;
        query.dwNameSpace = NS_BTH;
        query.lpServiceClassId = &uuid;
        query.lpszServiceInstanceName = const_cast<wchar_t *>(L"LZC Mesh");
        query.dwNumberOfCsAddrs = 1;
        query.lpcsaBuffer = &address;
        return WSASetServiceW(&query, op, 0) == 0;
    }
    void stop() {
        running = false;
        if (registered) service(RNRSERVICE_DELETE);
        registered = false;
        closeSocket(listener);
        for (auto &entry : connections) closeSocket(entry.second.socket);
        connections.clear();
        if (radio) CloseHandle(radio);
        radio = nullptr;
    }
    void start(uint64_t epoch, bool winsockReady) {
        stop(); generation = epoch;
        if (!winsockReady) { error("无法初始化 Windows 蓝牙套接字"); return; }
        BLUETOOTH_FIND_RADIO_PARAMS params{sizeof params};
        HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
        if (!find) { error("未检测到可用蓝牙适配器，请连接适配器并开启系统蓝牙后重新选择仅蓝牙"); return; }
        BluetoothFindRadioClose(find);
        BLUETOOTH_RADIO_INFO info{sizeof info};
        if (BluetoothGetRadioInfo(radio, &info) != ERROR_SUCCESS) { stop(); error("无法读取系统蓝牙适配器"); return; }
        listener = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        local = {}; local.addressFamily = AF_BTH; local.btAddr = info.address.ullLong; local.port = BT_PORT_ANY;
        int len = sizeof local;
        if (listener == INVALID_SOCKET || !nonblocking(listener) || bind(listener, (SOCKADDR *)&local, sizeof local) != 0 ||
            getsockname(listener, (SOCKADDR *)&local, &len) != 0 || local.port < 1 || local.port > 30 || listen(listener, 8) != 0) {
            stop(); error("无法监听 RFCOMM 蓝牙服务，请检查适配器和系统蓝牙服务"); return;
        }
        if (!service(RNRSERVICE_REGISTER)) { stop(); error("无法发布 RFCOMM 蓝牙服务"); return; }
        registered = true; running = true;
        Json bt;
        Json::parse(JsonOut().s("role", "listener").s("address", addressText(local.btAddr)).n("channel", local.port).b("connector", true).str(), bt);
        notify(JsonOut().s("t", "bt_local").raw("bluetooth", bt));
    }
    bool paired(BTH_ADDR address) {
        BLUETOOTH_DEVICE_INFO info{sizeof info}; info.Address.ullLong = address;
        return radio && BluetoothGetDeviceInfo(radio, &info) == ERROR_SUCCESS && info.fAuthenticated;
    }
    std::string add(SOCKET socket, std::string peer, bool dialing) {
        const auto id = "windows-bt-" + std::to_string(generation) + "-" + std::to_string(++connectionNumber);
        Connection c; c.socket = socket; c.peer = std::move(peer); c.connecting = dialing;
        connections.emplace(id, std::move(c));
        return id;
    }
    void opened(const std::string &id, const Connection &c) {
        JsonOut out; out.s("t", "bt_open").s("connection", id);
        if (!c.peer.empty()) out.s("peer", c.peer);
        notify(std::move(out));
    }
    void close(const std::string &id, const std::string &why) {
        auto it = connections.find(id); if (it == connections.end()) return;
        closeSocket(it->second.socket); connections.erase(it);
        notify(JsonOut().s("t", "bt_closed").s("connection", id).s("error", why));
    }
    void command(const Command &cmd, bool winsockReady) {
        const auto kind = cmd.value["_lan"].str();
        if (kind == "bt_start") { start(cmd.generation, winsockReady); return; }
        if (kind == "_stop") { stop(); return; }
        if (!running || cmd.generation != generation) return;
        if (kind == "bt_stop") { stop(); return; }
        if (kind == "bt_connect") {
            const auto peer = cmd.value["peer"].str();
            const auto &bt = cmd.value["bluetooth"];
            BTH_ADDR address;
            const double channel = bt["channel"].num();
            if (peer.empty() || connections.size() >= 8 || channel < 1 || channel > 30 || channel != (ULONG)channel || !parseAddress(bt["address"].str(), address)) return;
            for (const auto &entry : connections) if (entry.second.peer == peer) return;
            if (!paired(address)) { notify(JsonOut().s("t", "bt_closed").s("error", "蓝牙目标尚未系统配对，请先在 Windows 设置中配对")); return; }
            SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (socket == INVALID_SOCKET || !nonblocking(socket)) { closeSocket(socket); return; }
            SOCKADDR_BTH target{}; target.addressFamily = AF_BTH; target.btAddr = address; target.port = (ULONG)channel;
            int result = connect(socket, (SOCKADDR *)&target, sizeof target);
            const int error = result == 0 ? 0 : WSAGetLastError();
            if (result != 0 && error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
                closeSocket(socket); notify(JsonOut().s("t", "bt_closed").s("error", "蓝牙连接失败，请检查系统配对和距离")); return;
            }
            auto id = add(socket, peer, result != 0);
            if (result == 0) opened(id, connections.at(id));
            return;
        }
        const auto id = cmd.value["connection"].str();
        auto it = connections.find(id); if (it == connections.end()) return;
        if (kind == "bt_close") { close(id, "蓝牙连接已关闭"); return; }
        if (kind == "bt_send") {
            auto &c = it->second;
            if (!c.output.empty() || !decode(cmd.value["data"].str(), c.output)) { close(id, "蓝牙发送缓冲无效或超限"); return; }
            c.offset = 0;
        }
    }
    void poll() {
        fd_set readSet, writeSet, errorSet;
        FD_ZERO(&readSet); FD_ZERO(&writeSet); FD_ZERO(&errorSet);
        if (listener != INVALID_SOCKET) FD_SET(listener, &readSet);
        for (const auto &entry : connections) {
            const auto &c = entry.second;
            if (!c.connecting) FD_SET(c.socket, &readSet);
            if (c.connecting || !c.output.empty()) FD_SET(c.socket, &writeSet);
            FD_SET(c.socket, &errorSet);
        }
        timeval timeout{0, 20000};
        if (select(0, &readSet, &writeSet, &errorSet, &timeout) == SOCKET_ERROR) return;
        if (listener != INVALID_SOCKET && FD_ISSET(listener, &readSet)) {
            SOCKADDR_BTH remote{}; int n = sizeof remote;
            SOCKET socket = accept(listener, (SOCKADDR *)&remote, &n);
            if (socket != INVALID_SOCKET) {
                if (connections.size() >= 8 || !nonblocking(socket) || !paired(remote.btAddr)) closeSocket(socket);
                else { auto id = add(socket, "", false); opened(id, connections.at(id)); }
            }
        }
        std::vector<std::string> remove;
        for (auto &entry : connections) {
            const auto &id = entry.first; auto &c = entry.second;
            if (FD_ISSET(c.socket, &errorSet) || (c.connecting && Clock::now() - c.since > std::chrono::seconds(8))) { remove.push_back(id); continue; }
            if (c.connecting && FD_ISSET(c.socket, &writeSet)) {
                int error = 0, n = sizeof error;
                if (getsockopt(c.socket, SOL_SOCKET, SO_ERROR, (char *)&error, &n) != 0 || error) { remove.push_back(id); continue; }
                c.connecting = false; opened(id, c);
            }
            if (!c.connecting && FD_ISSET(c.socket, &readSet)) {
                char data[16384]; int n = recv(c.socket, data, sizeof data, 0);
                if (n > 0) notify(JsonOut().s("t", "bt_data").s("connection", id).s("data", encode(data, (DWORD)n)));
                else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) { remove.push_back(id); continue; }
            }
            if (!c.connecting && !c.output.empty() && FD_ISSET(c.socket, &writeSet)) {
                int n = send(c.socket, c.output.data() + c.offset, (int)(c.output.size() - c.offset), 0);
                if (n > 0) {
                    c.offset += (size_t)n;
                    if (c.offset == c.output.size()) {
                        c.output.clear(); c.offset = 0;
                        notify(JsonOut().s("t", "bt_written").s("connection", id));
                    }
                } else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) remove.push_back(id);
            }
        }
        for (const auto &id : remove) close(id, "蓝牙连接已断开或连接超时");
    }
};
void run() {
    WSADATA data{};
    const bool ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    State state;
    bool quit = false;
    while (!quit) {
        std::deque<Command> batch;
        {
            std::unique_lock<std::mutex> lock(commandMutex);
            if (!state.running) commandReady.wait(lock, [] { return !commands.empty(); });
            batch.swap(commands);
        }
        for (const auto &cmd : batch) {
            if (cmd.value["_lan"].str() == "_shutdown") { quit = true; break; }
            state.command(cmd, ready);
        }
        if (!quit && state.running) state.poll();
    }
    state.stop();
    if (ready) WSACleanup();
}
void enqueue(Json value, uint64_t generation) {
    { std::lock_guard<std::mutex> lock(commandMutex); commands.push_back(Command{std::move(value), generation}); }
    commandReady.notify_one();
}
Json control(const char *kind) { Json v; Json::parse(JsonOut().s("_lan", kind).str(), v); return v; }
}
void bind(uint64_t generation) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    acceptingGeneration = generation;
}
void event(const Json &value, uint64_t generation) {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    if (!generation || generation != acceptingGeneration) return;
    if (!started) { started = true; worker = std::thread(run); }
    enqueue(value, generation);
}
void stop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    acceptingGeneration = 0;
    if (started) enqueue(control("_stop"), 0);
}
void shutdown() {
    std::thread retired;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        acceptingGeneration = 0;
        if (!started) return;
        enqueue(control("_shutdown"), 0);
        retired = std::move(worker); started = false;
    }
    if (retired.joinable()) retired.join();
    std::lock_guard<std::mutex> lock(commandMutex); commands.clear();
}
}
