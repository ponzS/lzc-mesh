// WinHTTP based networking: plain GET requests for the LazyCat probe and a
// WebSocket to the hub. Runs on worker threads and posts results to the UI.
#pragma once
#include <string>

struct HttpResult {
    int status = 0;          // HTTP status, 0 on transport failure
    std::string body;
    std::wstring error;      // transport error text
};

// headers: "Name: value\r\n..." (may be empty)
HttpResult httpGet(const std::wstring &url, const std::wstring &headers);

// Hub WebSocket. connect() returns immediately; the reader thread posts
// WM_APP_HUB_OPEN / WM_APP_HUB_TEXT / WM_APP_HUB_CLOSED to the main window.
namespace hubws {
bool busy();
void connect(const std::wstring &host, const std::wstring &cookie);
void close();
void send(const std::string &text);  // thread-safe
}
