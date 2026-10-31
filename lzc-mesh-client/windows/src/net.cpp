#include "app.h"
#include "net.h"

#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "app.h"

namespace {
std::wstring lastError(const wchar_t *what) {
    DWORD e = GetLastError();
    const wchar_t *hint = L"";
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: hint = L"域名无法解析，懒猫网络可能未连接"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: hint = L"无法建立连接"; break;
    case ERROR_WINHTTP_TIMEOUT: hint = L"连接超时"; break;
    case ERROR_WINHTTP_SECURE_FAILURE: hint = L"TLS 校验失败"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: hint = L"连接被重置"; break;
    }
    return fmt(L"%ls 失败（%lu%ls%ls）", what, e, *hint ? L"，" : L"", hint);
}

struct Url {
    bool https = false;
    std::wstring host, path;
    INTERNET_PORT port = 0;
};

bool crack(const std::wstring &url, Url &out) {
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof c;
    wchar_t host[256], path[2048];
    c.lpszHostName = host;
    c.dwHostNameLength = 256;
    c.lpszUrlPath = path;
    c.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    c.lpszExtraInfo = extra;
    c.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &c)) return false;
    out.https = c.nScheme == INTERNET_SCHEME_HTTPS;
    out.host.assign(host, c.dwHostNameLength);
    out.path.assign(path, c.dwUrlPathLength);
    out.path.append(extra, c.dwExtraInfoLength);
    out.port = c.nPort;
    return true;
}

// heiyu.space is routed by the LazyCat virtual network (TUN); a system proxy
// would send it to the internet instead, so never use one.
HINTERNET openSession() {
    HINTERNET s = WinHttpOpen(L"lzc-mesh-windows/" LZC_MESH_VERSION, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 8000, 8000, 8000, 8000);
    return s;
}
}  // namespace

HttpResult httpGet(const std::wstring &url, const std::wstring &headers) {
    HttpResult r;
    Url u;
    if (!crack(url, u)) {
        r.error = L"无效地址";
        return r;
    }
    HINTERNET s = openSession(), c = nullptr, q = nullptr;
    if (!s) { r.error = lastError(L"WinHttpOpen"); return r; }
    c = WinHttpConnect(s, u.host.c_str(), u.port, 0);
    if (c) q = WinHttpOpenRequest(c, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0);
    if (q) {
        DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(q, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof noRedirect);
        DWORD noCookies = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(q, WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof noCookies);
    }
    if (!q || !WinHttpSendRequest(q, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(q, nullptr)) {
        r.error = lastError(L"请求");
    } else {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
        r.status = (int)code;
        DWORD avail;
        while (WinHttpQueryDataAvailable(q, &avail) && avail) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(q, chunk.data(), avail, &read) || !read) break;
            r.body.append(chunk.data(), read);
            if (r.body.size() > (1 << 20)) break;
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}

namespace hubws {
namespace {
std::mutex mu;
HINTERNET session, connection, request, socket;
std::atomic<bool> connecting{false};
std::atomic<unsigned> generation{0};

void closeHandles() {
    std::lock_guard<std::mutex> l(mu);
    for (HINTERNET *h : {&socket, &request, &connection, &session}) {
        if (*h) WinHttpCloseHandle(*h);
        *h = nullptr;
    }
}

void post(UINT msg, WPARAM w, LPARAM l) {
    if (!PostMessageW(g.hwnd, msg, w, l)) {
        if (msg == WM_APP_HUB_TEXT) delete (std::string *)l;
        if (msg == WM_APP_HUB_CLOSED) delete (std::wstring *)l;
    }
}

void run(std::wstring host, std::wstring cookie, unsigned gen) {
    int httpStatus = 0;
    std::wstring why;
    HINTERNET s = openSession(), c = nullptr, q = nullptr, w = nullptr;
    if (s) c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) q = WinHttpOpenRequest(c, L"GET", L"/api/ws", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (q) {
        DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(q, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof noRedirect);
        WinHttpSetOption(q, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    }
    std::wstring headers = L"Cookie: Lzc-Auth-Token=" + cookie + L"\r\n";
    if (!q || !WinHttpSendRequest(q, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(q, nullptr)) {
        why = lastError(L"连接");
    } else {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
        httpStatus = (int)code;
        if (code == 101) w = WinHttpWebSocketCompleteUpgrade(q, 0);
        if (!w) why = code == 101 ? lastError(L"WebSocket 升级") : fmt(L"服务端返回 HTTP %lu", code);
    }
    {
        std::lock_guard<std::mutex> l(mu);
        session = s; connection = c; request = q; socket = w;
    }
    connecting = false;
    if (w && gen == generation) {
        DWORD ka = 15000;
        WinHttpSetOption(w, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &ka, sizeof ka);
        post(WM_APP_HUB_OPEN, 0, 0);
        std::string msg;
        std::vector<char> buf(64 * 1024);
        while (true) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            DWORD err = WinHttpWebSocketReceive(w, buf.data(), (DWORD)buf.size(), &read, &type);
            if (err != NO_ERROR) { why = fmt(L"读取失败（%lu）", err); break; }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { why = L"服务端关闭连接"; break; }
            msg.append(buf.data(), read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) post(WM_APP_HUB_TEXT, 0, (LPARAM) new std::string(std::move(msg)));
            msg.clear();
        }
    }
    if (gen == generation) {
        closeHandles();
        post(WM_APP_HUB_CLOSED, (WPARAM)httpStatus, (LPARAM) new std::wstring(why));
    }
}
}  // namespace

bool busy() { return connecting; }

void connect(const std::wstring &host, const std::wstring &cookie) {
    if (connecting) return;
    connecting = true;
    unsigned gen = ++generation;
    std::thread(run, host, cookie, gen).detach();
}

void close() {
    std::lock_guard<std::mutex> l(mu);
    if (socket) WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    // Closing the request aborts a pending receive; the reader posts WM_APP_HUB_CLOSED.
    if (request) WinHttpCloseHandle(request), request = nullptr;
}

void send(const std::string &text) {
    std::lock_guard<std::mutex> l(mu);
    if (socket) WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), (DWORD)text.size());
}
}  // namespace hubws
