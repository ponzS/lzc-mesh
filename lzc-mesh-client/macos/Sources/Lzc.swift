// LazyCat network status and sign-in credentials for the box, obtained from the
// LazyCat client signed in on this computer. Requests to the box then travel over
// the LazyCat network.
import Foundation

final class Lzc {
    static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 8
        // heiyu.space is routed by the LazyCat virtual network; a system proxy must not intercept it.
        c.connectionProxyDictionary = [:]
        c.httpShouldSetCookies = false
        c.httpCookieAcceptPolicy = .never
        return URLSession(configuration: c, delegate: NoRedirect(), delegateQueue: nil)
    }()

    final class NoRedirect: NSObject, URLSessionTaskDelegate {
        func urlSession(_ s: URLSession, task: URLSessionTask, willPerformHTTPRedirection r: HTTPURLResponse, newRequest: URLRequest,
                        completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }
    }

    struct Probe {
        var client: (Level, String) = (.unknown, "")
        var net: (Level, String) = (.unknown, "等待懒猫微服客户端")
        var login: (Level, String) = (.unknown, "等待懒猫网络")
        var service: (Level, String)? = nil
        var boxName = "", boxDomain = "", loginUser = "", cookie = ""
    }

    private var running = false
    private var serviceCheckedAt = Date.distantPast
    var onReady: ((Bool, Bool) -> Void)?  // (ready, identityChanged)

    static func getJSON(_ url: String, headers: [String: String]) -> (Any?, String?) {
        guard let u = URL(string: url) else { return (nil, "无效地址") }
        var req = URLRequest(url: u)
        headers.forEach { req.setValue($1, forHTTPHeaderField: $0) }
        let sem = DispatchSemaphore(value: 0)
        var out: (Any?, String?) = (nil, nil)
        session.dataTask(with: req) { data, resp, err in
            defer { sem.signal() }
            if let err = err { out = (nil, err.localizedDescription); return }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code != 200 { out = (nil, "HTTP \(code)"); return }
            guard let data = data, let obj = try? JSONSerialization.jsonObject(with: data) else { out = (nil, "响应不是 JSON"); return }
            out = (obj, nil)
        }.resume()
        sem.wait()
        return out
    }

    static func boxStatusText(_ s: Int) -> String {
        switch s { case 0: return "未激活"; case 1: return "连接中"; case 2: return "已连接"; case 3: return "连接失败"; case 4: return "正在激活"; default: return "未知" }
    }

    private func probe(override: String, checkService: Bool) -> Probe {
        var p = Probe()
        let cfgURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".config/lzc-client-desktop/core.cfg")
        guard let data = try? Data(contentsOf: cfgURL),
              let cfg = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            p.client = (.error, "未找到懒猫微服客户端配置 \(cfgURL.path)，请安装并登录懒猫微服客户端")
            return p
        }
        let port = cfg["http_port"] as? Int ?? 0
        let token = (cfg["extra_headers"] as? [String: Any])?["LZC_MISC_AUTH_TOKEN"] as? String ?? ""
        guard port > 0, !token.isEmpty else { p.client = (.error, "懒猫微服客户端配置不完整，请重启懒猫微服客户端"); return p }
        let base = "http://127.0.0.1:\(port)"
        let auth = ["LZC_MISC_AUTH_TOKEN": token]
        let (boxObj, err) = Lzc.getJSON("\(base)/default-box", headers: auth)
        guard let box = boxObj as? [String: Any] else {
            p.client = (.error, "懒猫微服客户端未运行（本地接口 127.0.0.1:\(port) 不可达：\(err ?? "无数据")）")
            return p
        }
        p.client = (.ok, "运行中（本地接口 127.0.0.1:\(port)）")
        p.boxName = box["boxName"] as? String ?? ""
        p.boxDomain = box["boxDomain"] as? String ?? ""
        p.loginUser = box["loginUser"] as? String ?? ""
        var status = box["status"] as? Int ?? -1
        let useOverride = !override.isEmpty && override != p.boxName
        if useOverride {
            p.boxName = override; p.boxDomain = "\(override).heiyu.space"; p.loginUser = ""; status = 2
        }
        if p.boxDomain.isEmpty { p.net = (.error, "懒猫微服客户端没有默认微服，请先在客户端中登录一台微服"); return p }
        p.net = (status == 2 ? .ok : (status == 1 || status == 4) ? .warn : .error,
                 "\(p.boxDomain) · \(Lzc.boxStatusText(status)) · 用户 \(p.loginUser.isEmpty ? "未登录" : p.loginUser)\(useOverride ? "" : "（懒猫客户端默认微服）")")
        if status != 2 { return p }

        let q = "https://\(p.boxDomain)/".addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? ""
        let (extObj, extErr) = Lzc.getJSON("\(base)/box-ext-info?url=\(q)", headers: auth)
        if let ext = extObj as? [String: Any], let cookies = ext["cookies"] as? [[String: Any]] {
            p.cookie = cookies.first { $0["name"] as? String == "Lzc-Auth-Token" }?["value"] as? String ?? ""
        }
        if !p.cookie.isEmpty && useOverride {
            let (who, whoErr) = Lzc.getJSON("https://\(p.boxDomain)/sys/whoami", headers: ["Cookie": "Lzc-Auth-Token=\(p.cookie)"])
            if let w = who as? [String: Any] {
                p.loginUser = w["uid"] as? String ?? ""
                p.net = (.ok, "\(p.boxDomain) · 已连接 · 用户 \(p.loginUser)（手动指定的微服）")
            } else {
                p.net = (.error, "\(p.boxDomain) 不可达或会话无效（\(whoErr ?? "?")），请确认懒猫微服客户端已登录该微服")
                p.cookie = ""
            }
        }
        if p.cookie.isEmpty {
            p.login = (.error, "没有拿到登录会话\(extErr.map { "：\($0)" } ?? "")，请在懒猫微服客户端中重新登录")
            return p
        }
        p.login = (.ok, "已获取 \(p.boxDomain) 的登录会话")
        if !checkService { return p }
        let (who, whoErr) = Lzc.getJSON("https://lzc-mesh.\(p.boxDomain)/api/whoami", headers: ["Cookie": "Lzc-Auth-Token=\(p.cookie)"])
        if let w = who as? [String: Any] {
            p.service = (.ok, "lzc-mesh.\(p.boxDomain) · 版本 \(w["version"] as? String ?? "?")")
        } else {
            let e = whoErr ?? "?"
            let missing = e.hasPrefix("HTTP 404") || e.hasPrefix("HTTP 502") || e.hasPrefix("HTTP 503")
            p.service = (.error, "\(missing ? "微服上未安装或未启动懒猫设备接力应用" : "无法访问懒猫设备接力应用")（\(e)）")
        }
        return p
    }

    func refresh() {
        if running { return }
        running = true
        let s = AppState.shared
        let override = s.boxOverride
        let checkService = !s.hubConnected || Date().timeIntervalSince(serviceCheckedAt) > 30
        DispatchQueue.global().async {
            let p = self.probe(override: override, checkService: checkService)
            DispatchQueue.main.async {
                self.running = false
                s.set(.lzcClient, p.client.0, p.client.1)
                s.set(.lzcNetwork, p.net.0, p.net.1)
                s.set(.login, p.login.0, p.login.1)
                if let svc = p.service { self.serviceCheckedAt = Date(); s.set(.service, svc.0, svc.1) }
                let changed = s.boxDomain != p.boxDomain || s.cookie != p.cookie
                s.boxName = p.boxName; s.boxDomain = p.boxDomain; s.loginUser = p.loginUser; s.cookie = p.cookie
                let ready = p.net.0 == .ok && p.login.0 == .ok && s.status[.service]?.0 == .ok
                if !ready { s.set(.hub, .unknown, "等待懒猫设备接力应用可用") }
                self.onReady?(ready, changed)
            }
        }
    }

    func invalidate() { AppState.shared.cookie = ""; serviceCheckedAt = .distantPast }
}
