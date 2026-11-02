// WebSocket connection to the 懒猫设备接力 hub (URLSessionWebSocketTask).
import Foundation

final class Hub: NSObject, URLSessionWebSocketDelegate {
    private var task: URLSessionWebSocketTask?
    private var session: URLSession!
    private(set) var connecting = false
    private var pingTimer: Timer?
    private var lastMoveLog = Date.distantPast
    var input: MacInput!
    var clipboard: Clipboard!
    var onCookieRejected: (() -> Void)?

    override init() {
        super.init()
        let c = URLSessionConfiguration.ephemeral
        c.connectionProxyDictionary = [:]
        c.httpShouldSetCookies = false
        session = URLSession(configuration: c, delegate: self, delegateQueue: .main)
    }

    func connect() {
        let s = AppState.shared
        guard task == nil, !connecting, !s.cookie.isEmpty, !s.boxDomain.isEmpty, s.screenW > 0 else { return }
        let url = "wss://lzc-mesh.\(s.boxDomain)/api/ws"
        var req = URLRequest(url: URL(string: url)!)
        req.setValue("Lzc-Auth-Token=\(s.cookie)", forHTTPHeaderField: "Cookie")
        connecting = true
        s.set(.hub, .warn, "正在连接 \(url)")
        let t = session.webSocketTask(with: req)
        t.maximumMessageSize = 1 << 20
        task = t
        t.resume()
        receive(t)
    }

    func disconnect() { task?.cancel(with: .normalClosure, reason: nil) }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol p: String?) {
        guard webSocketTask === task else { return }
        connecting = false
        let s = AppState.shared
        s.hubConnected = true
        s.set(.hub, .ok, "已连接")
        s.log("已连接设备接力中枢")
        sendHello()
        pingTimer = Timer.scheduledTimer(withTimeInterval: 20, repeats: true) { [weak self] _ in self?.send(["t": "ping"]) }
    }

    func urlSession(_ session: URLSession, task t: URLSessionTask, didCompleteWithError error: Error?) {
        guard t === task else { return }
        closed(error.map { $0.localizedDescription } ?? "连接关闭", status: (t.response as? HTTPURLResponse)?.statusCode)
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didCloseWith code: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        guard webSocketTask === task else { return }
        closed("服务端关闭（\(code.rawValue)）", status: nil)
    }

    private func closed(_ why: String, status: Int?) {
        let s = AppState.shared
        let wasConnected = s.hubConnected
        task = nil
        connecting = false
        pingTimer?.invalidate(); pingTimer = nil
        s.hubConnected = false
        if wasConnected {
            s.log("与设备接力中枢的连接已断开：\(why)")
            s.set(.hub, .warn, "连接已断开，稍后自动重连")
        } else {
            s.log("连接设备接力中枢失败：\(why)\(status.map { "（HTTP \($0)）" } ?? "")")
            s.set(.hub, .error, "连接失败：\(why)")
            if status == 401 || status == 307 || status == 302 { onCookieRejected?() }
        }
        if s.captured { input.capture(false) }
        if s.controlled { input.leave() }
        s.captured = false; s.controlled = false
        input.setEdges(left: false, right: false, up: false, down: false)
        s.updateControlStatus()
        s.updateClipboardStatus()
    }

    private func receive(_ t: URLSessionWebSocketTask) {
        t.receive { [weak self] result in
            guard let self = self, t === self.task else { return }
            if case .success(let msg) = result {
                if case .string(let text) = msg, let data = text.data(using: .utf8),
                   let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    self.handle(obj)
                }
                self.receive(t)
            }
        }
    }

    func send(_ obj: [String: Any]) {
        guard let t = task, AppState.shared.hubConnected,
              let data = try? JSONSerialization.data(withJSONObject: obj), let text = String(data: data, encoding: .utf8) else { return }
        t.send(.string(text)) { _ in }
    }

    func sendHello() {
        let s = AppState.shared
        send(["t": "hello", "id": s.deviceID, "name": Host.current().localizedName ?? "Mac", "platform": "macos",
              "w": s.screenW, "h": s.screenH, "version": AppState.version])
        if !s.sharingEnabled { send(["t": "sharing", "enabled": false]) }
    }

    private func handle(_ o: [String: Any]) {
        let s = AppState.shared
        let t = o["t"] as? String ?? ""
        let x = o["x"] as? Double ?? 0.5, y = o["y"] as? Double ?? 0.5
        switch t {
        case "welcome":
            s.hubUser = o["user"] as? String ?? ""
            if !(o["id"] as? String ?? "").isEmpty { s.log("已加入 \(s.hubUser) 的设备组") }
        case "state":
            s.devices = (o["devices"] as? [[String: Any]] ?? []).map {
                MeshDevice(id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "", platform: $0["platform"] as? String ?? "",
                           version: $0["version"] as? String ?? "", x: $0["x"] as? Double ?? 0, y: $0["y"] as? Double ?? 0,
                           w: $0["w"] as? Double ?? 0, h: $0["h"] as? Double ?? 0, online: $0["online"] as? Bool ?? false,
                           sharing: $0["sharing"] as? Bool ?? true)
            }
            let active = o["active"] as? [String: Any]
            s.activeSource = active?["source"] as? String
            s.activeTarget = active?["target"] as? String
            let settings = o["settings"] as? [String: Any]
            s.swapCmdCtrl = settings?["swap_cmd_ctrl"] as? Bool ?? true
            let clip = settings?["clipboard"] as? Bool ?? true
            if clip != s.clipboardEnabled { s.log("共享剪贴板已\(clip ? "开启" : "关闭")") }
            s.clipboardEnabled = clip
            s.updateClipboardStatus()
            s.set(.hub, .ok, "已连接 · 用户 \(s.hubUser) · \(s.devices.filter { $0.online }.count) 台设备在线")
            s.updateControlStatus()
            s.onDevices?()
            s.onStatus?()
        case "edges":
            input.setEdges(left: o["left"] as? Bool ?? false, right: o["right"] as? Bool ?? false,
                           up: o["up"] as? Bool ?? false, down: o["down"] as? Bool ?? false)
        case "capture":
            s.captured = true
            s.log("开始控制其他设备")
            input.capture(true)
            s.updateControlStatus()
        case "release":
            s.captured = false
            input.capture(false)
            if o["warp"] as? Bool ?? false { input.warp(x, y) }
            s.log("控制已收回本机")
            s.updateControlStatus()
        case "enter":
            s.controlled = true
            s.log(String(format: "被远程控制：光标进入本机 (%.2f, %.2f)", x, y))
            input.enter(x, y)
            s.updateControlStatus()
        case "leave":
            s.controlled = false
            input.leave()
            s.log("远程控制结束")
            s.updateControlStatus()
        case "move":
            input.move(x, y)
            if Date().timeIntervalSince(lastMoveLog) > 2 {
                lastMoveLog = Date()
                s.log(String(format: "收到远程光标 (%.3f, %.3f)", x, y))
            }
        case "button":
            input.button(o["button"] as? Int ?? 1, down: o["down"] as? Bool ?? false)
        case "wheel":
            input.wheel(o["dx"] as? Double ?? 0, o["dy"] as? Double ?? 0,
                        scrollSource: o["source"] as? String, phase: o["phase"] as? String,
                        momentumPhase: o["momentum_phase"] as? String,
                        stop: o["stop_x"] as? Bool == true || o["stop_y"] as? Bool == true)
        case "key":
            let code = UInt32(o["code"] as? Int ?? 0), down = o["down"] as? Bool ?? false
            s.log("收到按键 code=\(code) \(down ? "按下" : "抬起")")
            input.key(code, down: down)
        case "clipboard":
            clipboard.set(o["text"] as? String ?? "", from: o["from"] as? String ?? "")
        case "files":
            s.files = (o["files"] as? [[String: Any]] ?? []).map {
                SharedFile(name: $0["name"] as? String ?? "", size: ($0["size"] as? NSNumber)?.int64Value ?? 0,
                           modified: $0["modified"] as? Double ?? 0, uploader: $0["uploader"] as? String ?? "")
            }
            s.onFiles?()
        case "error":
            s.log("服务端错误：\(o["message"] as? String ?? "")")
        default: break
        }
    }
}
