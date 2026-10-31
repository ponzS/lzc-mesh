// LazyCat network status and sign-in credentials for the box, obtained from the
// LazyCat client signed in on this computer, then a check that the LZC Mesh LPK answers on lzc-mesh.<box>.
#include "lzc.h"

#include <thread>

#include "app.h"
#include "net.h"

namespace {
std::wstring boxStatusText(int s) {
    switch (s) {
    case 0: return L"未激活";
    case 1: return L"连接中";
    case 2: return L"已连接";
    case 3: return L"连接失败";
    case 4: return L"正在激活";
    default: return L"未知";
    }
}

bool readFile(const std::wstring &path, std::string &out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD n;
    out.clear();
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(f);
    return true;
}

std::wstring errorOf(const HttpResult &r) { return r.status ? fmt(L"HTTP %d", r.status) : r.error; }

void probe(Probe *p) {
    p->net = {L_UNKNOWN, L"等待懒猫微服客户端"};
    p->login = {L_UNKNOWN, L"等待懒猫网络"};
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    std::wstring cfgPath = std::wstring(home, n) + L"\\.config\\lzc-client-desktop\\core.cfg";
    std::string text;
    Json cfg;
    if (!readFile(cfgPath, text) || !Json::parse(text, cfg) || !cfg.isObj()) {
        p->client = {L_ERROR, L"未找到懒猫微服客户端配置 " + cfgPath + L"，请安装并登录懒猫微服客户端"};
        return;
    }
    int port = (int)cfg["http_port"].num();
    std::string token = cfg["extra_headers"]["LZC_MISC_AUTH_TOKEN"].str();
    if (port <= 0 || token.empty()) {
        p->client = {L_ERROR, L"懒猫微服客户端配置不完整，请重启懒猫微服客户端"};
        return;
    }
    std::wstring base = fmt(L"http://127.0.0.1:%d", port);
    std::wstring auth = L"LZC_MISC_AUTH_TOKEN: " + widen(token) + L"\r\n";
    HttpResult r = httpGet(base + L"/default-box", auth);
    Json box;
    if (r.status != 200 || !Json::parse(r.body, box) || !box.isObj()) {
        p->client = {L_ERROR, fmt(L"懒猫微服客户端未运行（本地接口 127.0.0.1:%d 不可达：%ls）", port, errorOf(r).c_str())};
        return;
    }
    p->client = {L_OK, fmt(L"运行中（本地接口 127.0.0.1:%d）", port)};
    p->boxName = box["boxName"].str();
    p->boxDomain = box["boxDomain"].str();
    p->loginUser = box["loginUser"].str();
    int status = (int)box["status"].num(-1);
    bool useOverride = !p->override.empty() && p->override != p->boxName;
    if (useOverride) {
        p->boxName = p->override;
        p->boxDomain = p->override + ".heiyu.space";
        p->loginUser.clear();
        status = 2;
    }
    if (p->boxDomain.empty()) {
        p->net = {L_ERROR, L"懒猫微服客户端没有默认微服，请先在客户端中登录一台微服"};
        return;
    }
    p->net = {status == 2 ? L_OK : (status == 1 || status == 4) ? L_WARN : L_ERROR,
              fmt(L"%ls · %ls · 用户 %ls%ls", widen(p->boxDomain).c_str(), boxStatusText(status).c_str(),
                  p->loginUser.empty() ? L"未登录" : widen(p->loginUser).c_str(), useOverride ? L"" : L"（懒猫客户端默认微服）")};
    if (status != 2) return;

    r = httpGet(base + L"/box-ext-info?url=https%3A%2F%2F" + widen(p->boxDomain) + L"%2F", auth);
    Json ext;
    if (r.status == 200 && Json::parse(r.body, ext))
        for (auto &c : ext["cookies"].a)
            if (c["name"].str() == "Lzc-Auth-Token") p->cookie = c["value"].str();
    if (!p->cookie.empty() && useOverride) {
        HttpResult w = httpGet(L"https://" + widen(p->boxDomain) + L"/sys/whoami", L"Cookie: Lzc-Auth-Token=" + widen(p->cookie) + L"\r\n");
        Json who;
        if (w.status == 200 && Json::parse(w.body, who) && who.isObj()) {
            p->loginUser = who["uid"].str();
            p->net = {L_OK, fmt(L"%ls · 已连接 · 用户 %ls（手动指定的微服）", widen(p->boxDomain).c_str(), widen(p->loginUser).c_str())};
        } else {
            p->net = {L_ERROR, fmt(L"%ls 不可达或会话无效（%ls），请确认懒猫微服客户端已登录该微服", widen(p->boxDomain).c_str(), errorOf(w).c_str())};
            p->cookie.clear();
        }
    }
    if (p->cookie.empty()) {
        p->login = {L_ERROR, L"没有拿到登录会话（" + errorOf(r) + L"），请在懒猫微服客户端中重新登录"};
        return;
    }
    p->login = {L_OK, L"已获取 " + widen(p->boxDomain) + L" 的登录会话"};
    if (!p->checkService) return;
    r = httpGet(L"https://lzc-mesh." + widen(p->boxDomain) + L"/api/whoami", L"Cookie: Lzc-Auth-Token=" + widen(p->cookie) + L"\r\n");
    Json who;
    p->hasService = true;
    if (r.status == 200 && Json::parse(r.body, who) && who.isObj()) {
        p->service = {L_OK, fmt(L"lzc-mesh.%ls · 版本 %ls", widen(p->boxDomain).c_str(), widen(who["version"].str("?")).c_str())};
    } else {
        bool missing = r.status == 404 || r.status == 502 || r.status == 503;
        p->service = {L_ERROR, fmt(L"%ls（%ls）", missing ? L"微服上未安装或未启动 LZC Mesh 应用" : L"无法访问 LZC Mesh 应用", errorOf(r).c_str())};
    }
}
}  // namespace

void lzcProbeAsync(bool checkService) {
    Probe *p = new Probe;
    p->override = g.boxOverride;
    p->checkService = checkService;
    HWND hwnd = g.hwnd;
    std::thread([p, hwnd] {
        probe(p);
        if (!PostMessageW(hwnd, WM_APP_PROBE, 0, (LPARAM)p)) delete p;
    }).detach();
}
