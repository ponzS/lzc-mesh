// WebSocket connection to the 懒猫设备接力 hub (URLSessionWebSocketTask).
import Foundation

final class Hub: NSObject, URLSessionWebSocketDelegate {
    private var task: URLSessionWebSocketTask?
    private var session: URLSession!
    private(set) var connecting = false
    private var pingTimer: Timer?
    private var closing = false
    private var identity: DeviceIdentity?
    private var lastMoveLog = Date.distantPast
    private var inputGate = MeshInputGate()
    private var activeControlID = "", receiveControlID = "", blockedControlID = "", sourceControlID = ""
    private lazy var direct = DirectTransport(hub: self)
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
        closing = false
        s.set(.hub, .warn, "正在连接 \(url)")
        let t = session.webSocketTask(with: req)
        t.maximumMessageSize = 1 << 20
        task = t
        t.resume()
        receive(t)
    }

    func disconnect() {
        closing = true
        let s = AppState.shared
        s.authVerified = false; s.fileToken = ""
        s.updateClientState(nil)
        direct.disconnected()
        task?.cancel(with: .normalClosure, reason: nil)
    }

    func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol p: String?) {
        guard webSocketTask === task, !closing else { return }
        connecting = false
        let s = AppState.shared
        s.hubConnected = true
        s.authVerified = false; s.fileToken = ""; s.updateClientState(nil)
        s.set(.hub, .ok, "已连接")
        s.log("已连接设备接力中枢")
        sendHello()
        direct.received(["t": "hub_connected"])
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
        sourceControlID = ""
        let s = AppState.shared
        let wasConnected = s.hubConnected
        task = nil
        connecting = false
        pingTimer?.invalidate(); pingTimer = nil
        s.hubConnected = false
        s.authVerified = false; s.fileToken = ""
        s.updateClientState(nil)
        direct.disconnected()
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
            guard let self = self, t === self.task, !self.closing else { return }
            if case .success(let msg) = result {
                if case .string(let text) = msg, let data = text.data(using: .utf8),
                   let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    self.direct.received(obj)
                    self.handle(obj)
                }
                self.receive(t)
            }
        }
    }

    func send(_ obj: [String: Any]) {
        if !AppState.shared.operational && ["edge", "motion", "button", "wheel", "key", "clipboard", "layout", "forget"].contains(obj["t"] as? String ?? "") { return }
        var outgoing = obj
        if !sourceControlID.isEmpty && ["motion", "key", "button", "wheel", "release"].contains(obj["t"] as? String ?? "") {
            outgoing["_control_session"] = sourceControlID
        }
        if direct.input(outgoing) { return }
        sendRaw(outgoing)
    }

    func sendRaw(_ obj: [String: Any]) {
        if !AppState.shared.operational && ["direct_relay", "direct_begin", "direct_route", "direct_legacy"].contains(obj["t"] as? String ?? "") { return }
        guard let t = task, AppState.shared.hubConnected,
              let data = try? JSONSerialization.data(withJSONObject: obj), let text = String(data: data, encoding: .utf8) else { return }
        t.send(.string(text)) { _ in }
    }

    func sendHello() {
        let s = AppState.shared
        if identity == nil {
            do { identity = try DeviceIdentity() }
            catch { s.set(.deviceTrust, .error, "设备凭证不可用；共享已停止，未重置原凭证"); return }
        }
        guard let identity else { return }
        if !s.authVerified { s.set(.deviceTrust, .unknown, "等待批准 · 指纹 \(identity.fingerprint)") }
        send(["t": s.authVerified ? "hello" : "device_hello", "id": s.deviceID, "name": Host.current().localizedName ?? "Mac", "platform": "macos",
              "w": s.screenW, "h": s.screenH, "version": AppState.version, "public_key": identity.publicKey])
        if s.authVerified && !s.sharingEnabled { send(["t": "sharing", "enabled": false]) }
    }

    func localActivity() {
        let s = AppState.shared
        guard s.operational, s.controlled else { return }
        let id = receiveControlID.isEmpty ? activeControlID : receiveControlID
        mesh_input_gate_cancel(&inputGate)
        blockedControlID = id
        s.controlled = false
        input.localTakeover()
        if !id.isEmpty { send(["t": "local_activity", "control_id": id]) }
        s.log("检测到本机实体输入，已收回控制并释放远程按键和按钮")
        s.updateControlStatus()
    }

    func handle(_ o: [String: Any]) {
        let s = AppState.shared
        let t = o["t"] as? String ?? ""
        let x = o["x"] as? Double ?? 0.5, y = o["y"] as? Double ?? 0.5
        let scope = o["_control_session"] as? String ?? ""
        let begin = t == "direct_active" && !(o["source"] as? String ?? "").isEmpty
        let accepted = scope.withCString { begin ? mesh_input_gate_begin(&inputGate, $0) : mesh_input_gate_accept(&inputGate, $0) }
        guard accepted else { return }
        if scope.isEmpty && !blockedControlID.isEmpty && blockedControlID == activeControlID && ["capture", "enter", "move", "button", "key", "wheel"].contains(t) { return }
        if !s.operational && ["capture", "enter", "move", "button", "wheel", "key", "clipboard", "files"].contains(t) { return }
        switch t {
        case "device_challenge":
            guard let identity, let user = o["user"] as? String, user == s.hubUser,
                  let nonce = o["nonce"] as? String, nonce.utf8.count == 44 else { disconnect(); return }
            let proof = identity.proof(user: user, deviceID: s.deviceID, nonce: nonce)
            if !proof.isEmpty { sendRaw(["t": "device_proof", "signature": proof]) }
        case "device_auth":
            let status = o["status"] as? String ?? ""
            let token = o["token"] as? String ?? ""
            s.authVerified = identity != nil && o["fingerprint"] as? String == identity?.fingerprint && status == "approved" && Data(base64Encoded: token)?.count == 32
            s.fileToken = s.authVerified ? token : ""
            let message = s.authVerified ? "已批准" : status == "key_mismatch" ? "凭证不匹配，拒绝绑定" : status == "revoked" ? "信任已撤销" : "等待管理页核对并批准"
            s.set(.deviceTrust, s.authVerified ? .ok : .warn, "\(message) · 指纹 \(identity?.fingerprint ?? "")")
            let current = s.clientReady ? s.devices.first { $0.id == s.deviceID } : nil
            s.updateClientState(current, supported: s.clientControl)
            s.updateControlStatus(); s.updateClipboardStatus()
            if !s.authVerified { direct.disconnected() }
            if s.authVerified && !s.sharingEnabled { sendRaw(["t": "sharing", "enabled": false]) }
        case "welcome":
            if o["auth_version"] as? Int != 1 { s.set(.deviceTrust, .error, "需要支持设备凭证的新版中枢；共享保持停用") }
            s.hubUser = o["user"] as? String ?? ""
            if !(o["id"] as? String ?? "").isEmpty { s.log("已加入 \(s.hubUser) 的设备组") }
        case "direct_active":
            s.activeSource = (o["source"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            s.activeTarget = (o["target"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            s.updateControlStatus()
        case "state":
            s.devices = (o["devices"] as? [[String: Any]] ?? []).map {
                MeshDevice(id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "", platform: $0["platform"] as? String ?? "",
                           version: $0["version"] as? String ?? "", x: $0["x"] as? Double ?? 0, y: $0["y"] as? Double ?? 0,
                           w: $0["w"] as? Double ?? 0, h: $0["h"] as? Double ?? 0, online: $0["online"] as? Bool ?? false,
                           sharing: $0["sharing"] as? Bool ?? true, enabled: $0["enabled"] as? Bool ?? true, trusted: $0["trusted"] as? Bool ?? false)
            }
            let own = (o["devices"] as? [[String: Any]])?.first { $0["id"] as? String == s.deviceID }
            s.updateClientState(s.devices.first { $0.id == s.deviceID }, supported: own?["enabled"] is Bool)
            let active = o["active"] as? [String: Any]
            s.activeSource = active?["source"] as? String
            s.activeTarget = active?["target"] as? String
            activeControlID = active?["control_id"] as? String ?? ""
            if s.controlled && receiveControlID.isEmpty { receiveControlID = activeControlID }
            let settings = o["settings"] as? [String: Any]
            s.transportMode = settings?["transport_mode"] as? String ?? "auto"
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
            sourceControlID = scope
            if s.controlled { input.leave() }
            s.controlled = false
            s.captured = true
            s.log("开始控制其他设备")
            input.capture(true)
            s.updateControlStatus()
        case "release":
            sourceControlID = ""
            s.captured = false
            input.capture(false)
            if o["warp"] as? Bool ?? false { input.warp(x, y) }
            s.log("控制已收回本机")
            s.updateControlStatus()
        case "enter":
            sourceControlID = ""
            // Undo source capture even during a fast reversal before receiving.
            input.capture(false)
            s.captured = false
            receiveControlID = scope.isEmpty ? activeControlID : scope
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
            guard s.controlled else { return }
            input.move(x, y)
            if Date().timeIntervalSince(lastMoveLog) > 2 {
                lastMoveLog = Date()
                s.log(String(format: "收到远程光标 (%.3f, %.3f)", x, y))
            }
        case "button":
            guard s.controlled else { return }
            input.button(o["button"] as? Int ?? 1, down: o["down"] as? Bool ?? false)
        case "wheel":
            guard s.controlled else { return }
            input.wheel(o["dx"] as? Double ?? 0, o["dy"] as? Double ?? 0,
                        scrollSource: o["source"] as? String, phase: o["phase"] as? String,
                        momentumPhase: o["momentum_phase"] as? String,
                        stop: o["stop_x"] as? Bool == true || o["stop_y"] as? Bool == true)
        case "key":
            guard s.controlled else { return }
            let code = UInt32(o["code"] as? Int ?? 0), down = o["down"] as? Bool ?? false
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
