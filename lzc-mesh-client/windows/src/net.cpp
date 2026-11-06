#include "app.h"
#include "net.h"

#include <winhttp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "outbox.h"
#include "receiver.h"
#include "direct.h"

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
struct Connection {
    explicit Connection(unsigned value) : generation(value) {}
    const unsigned generation;
    HINTERNET session=nullptr, connection=nullptr, request=nullptr, socket=nullptr;
    std::mutex handles, writes;
    HubOutbox outbox;
    std::atomic<bool> stopping{false}, opened{false}, finished{false}, notified{false}, closing{false};
    ~Connection() {
        // Reader, writer and closer retain this object until all calls finish.
        for (auto h : {socket, request, connection, session}) if (h) WinHttpCloseHandle(h);
    }
};
std::mutex currentMutex;
std::shared_ptr<Connection> current;
unsigned nextGeneration=0;
std::shared_ptr<Connection> getCurrent() { std::lock_guard<std::mutex> lock(currentMutex); return current; }

void post(UINT msg, WPARAM w, LPARAM l) {
    if (!PostMessageW(g.hwnd,msg,w,l)) {
        if (msg==WM_APP_HUB_TEXT) delete reinterpret_cast<Text *>(l);
        if (msg==WM_APP_HUB_CLOSED) delete reinterpret_cast<Closed *>(l);
        if (msg==WM_APP_NET_LOG) delete reinterpret_cast<std::wstring *>(l);
    }
}
void stop(const std::shared_ptr<Connection> &c, int status, const std::wstring &why) {
    c->stopping=true; c->outbox.stop();
    receiver::closeHub(c->generation);
    if (!c->notified.exchange(true)) post(WM_APP_HUB_CLOSED,0,reinterpret_cast<LPARAM>(new Closed{c->generation,status,why}));
    if (c->closing.exchange(true)) return;
    // The protocol close may wait for the peer or an in-progress send. Neither
    // the UI nor an input callback ever waits on this mutex or this operation.
    std::thread([c] {
        std::lock_guard<std::mutex> write(c->writes);
        HINTERNET socket;
        { std::lock_guard<std::mutex> lock(c->handles); socket=c->socket; }
        if (socket) WinHttpWebSocketClose(socket,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,nullptr,0);
    }).detach();
}
void writer(const std::shared_ptr<Connection> &c) {
    HubOutbox::Frame frame;
    ULONGLONG lastReport=0;
    while (c->outbox.take(frame)) {
        if (c->stopping) break;
        const auto begin=std::chrono::steady_clock::now();
        DWORD result;
        {
            std::lock_guard<std::mutex> write(c->writes);
            if (c->stopping) break;
            result=WinHttpWebSocketSend(c->socket,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                const_cast<char *>(frame.text.data()),static_cast<DWORD>(frame.text.size()));
        }
        const auto now=std::chrono::steady_clock::now();
        const auto queued=std::chrono::duration_cast<std::chrono::milliseconds>(begin-frame.queued).count();
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-begin).count();
        if ((queued>=50 || elapsed>=50) && GetTickCount64()-lastReport>=4000) {
            lastReport=GetTickCount64();
            post(WM_APP_NET_LOG,c->generation,reinterpret_cast<LPARAM>(new std::wstring(
                fmt(L"hub_send_metrics queue_ms=%lld write_ms=%lld",static_cast<long long>(queued),static_cast<long long>(elapsed)))));
        }
        if (result!=NO_ERROR) { stop(c,0,fmt(L"发送失败（%lu）",result)); break; }
    }
}
void run(const std::shared_ptr<Connection> &c, std::wstring host, std::wstring cookie) {
    int status=0; std::wstring why=L"连接已关闭";
    c->session=openSession();
    if (c->session && !c->stopping) c->connection=WinHttpConnect(c->session,host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0);
    if (c->connection && !c->stopping) c->request=WinHttpOpenRequest(c->connection,L"GET",L"/api/ws",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    auto q=c->request;
    if (q) {
        DWORD disabled=WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(q,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof disabled);
        WinHttpSetOption(q,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0);
    }
    const auto headers=L"Cookie: Lzc-Auth-Token="+cookie+L"\r\n";
    HINTERNET socket=nullptr;
    if (!c->stopping && q && WinHttpSendRequest(q,headers.c_str(),static_cast<DWORD>(-1L),WINHTTP_NO_REQUEST_DATA,0,0,0) && !c->stopping && WinHttpReceiveResponse(q,nullptr)) {
        DWORD code=0,len=sizeof code;
        WinHttpQueryHeaders(q,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&code,&len,nullptr);
        status=static_cast<int>(code);
        if (code==101 && !c->stopping) socket=WinHttpWebSocketCompleteUpgrade(q,0);
        if (!socket) why=code==101?lastError(L"WebSocket 升级"):fmt(L"服务端返回 HTTP %lu",code);
    } else if (!c->stopping) why=lastError(L"连接");
    { std::lock_guard<std::mutex> lock(c->handles); c->socket=socket; }
    if (socket && !c->stopping) {
        DWORD keepalive=15000;
        WinHttpSetOption(socket,WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,&keepalive,sizeof keepalive);
        c->opened=true;
        std::thread(writer,c).detach();
        post(WM_APP_HUB_OPEN,c->generation,0);
        std::string message; std::vector<char> bytes(64*1024);
        while (!c->stopping) {
            DWORD read=0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            const DWORD error=WinHttpWebSocketReceive(socket,bytes.data(),static_cast<DWORD>(bytes.size()),&read,&type);
            if (error!=NO_ERROR) { why=fmt(L"读取失败（%lu）",error); break; }
            if (type==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { why=L"服务端关闭连接"; break; }
            message.append(bytes.data(),read);
            if (message.size()>(1u<<20)) { why=L"中枢消息超限"; break; }
            if (type==WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
            if (type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && !c->stopping) {
                Json value;
                const bool parsed = Json::parse(message, value);
                // Button-up, release and native relay/lease processing cannot
                // wait for DefWindowProc's caption/menu tracking on the UI.
                const bool consumed = parsed && receiver::hub(value, c->generation);
                const uint64_t forwarded = consumed ? 0 : direct::network(message);
                if (!consumed && (!forwarded || value["t"].str() == "state"))
                    post(WM_APP_HUB_TEXT,c->generation,reinterpret_cast<LPARAM>(new Text{std::move(message), forwarded}));
            }
            message.clear();
        }
    }
    stop(c,status,why); c->finished=true;
}
} // namespace

unsigned generation() { auto c=getCurrent(); return c ? c->generation : 0; }
bool busy() { auto c=getCurrent(); return c && !c->finished && (!c->opened || c->stopping); }
bool isCurrent(unsigned generation, bool allowStopped) {
    auto c=getCurrent(); return c && c->generation==generation && (allowStopped || !c->stopping);
}
void connect(const std::wstring &host,const std::wstring &cookie) {
    std::shared_ptr<Connection> c, retired;
    {
        std::lock_guard<std::mutex> lock(currentMutex);
        if (current && !current->finished) return;
        retired=std::move(current);
        current=c=std::make_shared<Connection>(++nextGeneration);
    }
    if (retired) std::thread([old=std::move(retired)]() mutable { old.reset(); }).detach();
    receiver::bindHub(c->generation);
    std::thread(run,c,host,cookie).detach();
}
void close() { if (auto c=getCurrent()) stop(c,0,L"客户端关闭管理连接"); }
void send(const std::string &text) { sendFor(generation(), text); }
void sendFor(unsigned generation, const std::string &text) {
    auto c=getCurrent();
    if (!c || c->generation != generation || !c->opened || c->stopping) return;
    if (!c->outbox.push(text) && !c->stopping) stop(c,0,L"中枢发送队列超限，连接已安全停止");
}
} // namespace hubws
