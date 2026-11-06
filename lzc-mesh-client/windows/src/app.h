// Shared state and small helpers for the Windows client. All state is owned
// by the UI thread; worker threads post results back with PostMessage.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "json.h"

#define LZC_MESH_VERSION "0.3.3"

enum Level { L_UNKNOWN, L_OK, L_WARN, L_ERROR };

enum StatusKey { S_LZC_CLIENT, S_LZC_NETWORK, S_LOGIN, S_SERVICE, S_HUB, S_INPUT, S_CONTROL, S_SCREEN, S_CLIPBOARD,
                 S_TRANSPORT, S_POINTER_CHANNEL, S_KEYBOARD_CHANNEL, S_TRUST, S_COUNT };

struct Status {
    Level level = L_UNKNOWN;
    std::wstring text = L"检测中…";
};

struct SharedFile {
    std::string name, uploader;
    long long size = 0;
    double modified = 0;
};

// Control ids shared between main.cpp and files.cpp.
enum { ID_FILES_LIST = 300, ID_FILES_STATUS, ID_FILES_UPLOAD, ID_FILES_DOWNLOAD, ID_FILES_DELETE };

struct MeshDevice {
    std::string id, name, platform, version;
    double x = 0, y = 0, w = 0, h = 0;
    bool online = false;
    bool sharing = true;
    bool enabled = true;
};

// Messages posted to the main window from other threads.
enum {
    WM_APP_PROBE = WM_APP + 1,  // lParam: Probe*
    WM_APP_HUB_TEXT,            // lParam: hubws::Text* (management/presentation only)
    WM_APP_HUB_OPEN,
    WM_APP_HUB_CLOSED,          // lParam: hubws::Closed*, includes connection generation
    WM_APP_EDGE,                // wParam: dir, lParam: pos*10000
    WM_APP_INPUT,               // lParam: std::string* (JSON frame to send)
    WM_APP_FILES_STATUS,        // lParam: std::wstring* status text
    WM_APP_FILES_REVEAL,        // lParam: std::wstring* downloaded path
    WM_APP_TRAY,                // notification-area icon callback
    WM_APP_DIRECT,              // drain ordered native transport callbacks on the UI thread
    WM_APP_NET_LOG,             // bounded network diagnostics, no message contents
    WM_APP_RECEIVER,            // receiver::Notice*, presentation only
};

struct App {
    HWND hwnd = nullptr;
    std::string deviceId;
    std::string boxOverride;
    std::string boxName, boxDomain, loginUser, cookie;
    bool hubConnected = false;
    std::string hubUser;
    std::vector<MeshDevice> devices;
    std::string activeSource, activeTarget, activeControlId, receiveControlId;
    bool swapCmdCtrl = true;
    bool autoCenter = true;
    bool clipboardEnabled = false;
    bool sharingEnabled = true;   // keyboard/mouse sharing switch (tray / header), persisted
    bool inputEnabled = true;     // account-wide keyboard/mouse pause
    bool deviceReady = false, deviceEnabled = true, deviceControl = false, deviceTrusted = false, authVerified = false;
    std::string fileToken;
    bool operational() const { return hubConnected && deviceReady && deviceEnabled && deviceTrusted && authVerified; }
    bool nativeProtocol = false;
    std::string transportMode = "auto";
    bool captured = false, controlled = false;
    int screenX = 0, screenY = 0, screenW = 0, screenH = 0;  // virtual desktop in physical px
    double dpiScale = 1.0;
    Status status[S_COUNT];
    bool elevatedForeground = false;
};

extern App g;

std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);
std::wstring fmt(const wchar_t *f, ...);
void setStatus(int key, Level level, const std::wstring &text);
void logLine(const std::wstring &text);
void devicesChanged();
const char *statusName(int key);
