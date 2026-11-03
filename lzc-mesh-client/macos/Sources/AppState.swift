import AppKit

enum Level { case unknown, ok, warn, error }

enum StatusKey: Int, CaseIterable {
    case lzcClient, lzcNetwork, login, service, hub, permission, control, screen, clipboard, transport, pointerChannel, keyboardChannel, deviceTrust
    var label: String {
        switch self {
        case .lzcClient: return "懒猫微服客户端"
        case .lzcNetwork: return "懒猫网络"
        case .login: return "登录会话"
        case .service: return "设备接力应用"
        case .hub: return "中枢连接"
        case .permission: return "系统权限"
        case .control: return "控制状态"
        case .screen: return "本机屏幕"
        case .clipboard: return "共享剪贴板"
        case .transport: return "输入通道"
        case .pointerChannel: return "光标通道"
        case .keyboardChannel: return "键盘通道"
        case .deviceTrust: return "设备信任"
        }
    }
}

struct MeshDevice {
    var id: String, name: String, platform: String, version: String
    var x: Double, y: Double, w: Double, h: Double
    var online: Bool
    var sharing: Bool = true
    var enabled: Bool = true
    var trusted: Bool = false
}

/// Main-thread state shared by all components.
final class AppState {
    static let shared = AppState()
    static let version = "0.2.2"

    var status: [StatusKey: (Level, String)] = Dictionary(uniqueKeysWithValues: StatusKey.allCases.map { ($0, (.unknown, "检测中…")) })
    var deviceID: String
    var boxOverride: String
    /// Keyboard/mouse sharing switched on (tray / settings switch). Persisted.
    var sharingEnabled = true
    var clientReady = false, clientEnabled = true, clientControl = false, clientTrusted = false
    var authVerified = false, fileToken = ""
    var operational: Bool { hubConnected && clientReady && clientEnabled && clientTrusted && authVerified }
    private var runtimeActive = false

    var boxName = "", boxDomain = "", loginUser = "", cookie = ""
    var hubConnected = false
    var hubUser = ""
    var devices: [MeshDevice] = []
    var activeSource: String?, activeTarget: String?
    var transportMode = "auto"
    var swapCmdCtrl = true
    var clipboardEnabled = false
    var files: [SharedFile] = []
    var captured = false, controlled = false
    var screenW = 0.0, screenH = 0.0

    var onStatus: (() -> Void)?
    var onDevices: (() -> Void)?
    var onLog: ((String) -> Void)?
    var onFiles: (() -> Void)?
    var onActivity: ((Bool) -> Void)?

    func updateClientState(_ device: MeshDevice?, supported: Bool = false) {
        clientReady = device != nil
        clientTrusted = device?.trusted ?? false
        if let device { clientEnabled = device.enabled }
        clientControl = supported
        let active = operational
        if active != runtimeActive {
            runtimeActive = active
            onActivity?(active)
            if !active { files = []; onFiles?() }
            log(active ? "客户端已开启" : "客户端已休眠：共享功能已停止，保留远程唤醒连接")
        }
        if !active {
            for key in [StatusKey.transport, .pointerChannel, .keyboardChannel] {
                set(key, .unknown, clientReady && !clientEnabled ? "客户端已关闭 · 仅保留远程唤醒连接" : "等待设备状态确认")
            }
        }
    }

    private let configURL: URL = {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent(".config/lzc-mesh")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("config.json")
    }()

    private let logHandle: FileHandle? = {
        let dir = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/lzc-mesh")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("client.log")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        return try? FileHandle(forWritingTo: url)
    }()

    private func writeLog(_ line: String) {
        guard let data = (line + "\n").data(using: .utf8) else { return }
        logHandle?.write(data)
    }

    private init() {
        var cfg: [String: Any] = [:]
        if let data = try? Data(contentsOf: configURL), let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] { cfg = obj }
        deviceID = (cfg["device_id"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "macos-\(UUID().uuidString.lowercased())"
        boxOverride = cfg["box"] as? String ?? ""
        sharingEnabled = cfg["sharing"] as? Bool ?? true
        save()
    }

    func save() {
        let obj: [String: Any] = ["device_id": deviceID, "box": boxOverride, "sharing": sharingEnabled]
        if let data = try? JSONSerialization.data(withJSONObject: obj, options: .prettyPrinted) { try? data.write(to: configURL) }
    }

    func set(_ key: StatusKey, _ level: Level, _ text: String) {
        if let cur = status[key], cur.0 == level, cur.1 == text { return }
        status[key] = (level, text)
        writeLog("[status] \(key) \(level): \(text)")
        onStatus?()
    }

    func log(_ text: String) {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        let line = "\(f.string(from: Date()))  \(text)"
        writeLog(line)
        onLog?(line)
    }

    func deviceName(_ id: String?) -> String { devices.first { $0.id == id }?.name ?? id ?? "?" }

    /// One-line connection summary for the header pill and the tray menu.
    func summary() -> (Level, String) {
        if hubConnected && !authVerified { return (.warn, "设备未获信任 · 共享停用") }
        if hubConnected && clientReady && !clientEnabled { return (.warn, "客户端已关闭 · 可远程开启") }
        if hubConnected { return (.ok, "已连接 · \(devices.filter { $0.online }.count) 台设备在线") }
        for k in [StatusKey.lzcClient, .lzcNetwork, .login, .service, .hub] {
            if let (lv, t) = status[k], lv == .error || lv == .warn { return (lv, "\(k.label)：\(t)") }
        }
        return (.unknown, "正在连接…")
    }

    /// "正在控制 X" / "X 正在控制本机" while a control session is active.
    func controlLine() -> String? {
        if captured { return "正在控制 \(deviceName(activeTarget))" }
        if controlled { return "\(deviceName(activeSource)) 正在控制本机" }
        return nil
    }

    func updateClipboardStatus() {
        if hubConnected && !authVerified { set(.clipboard, .unknown, "设备未获信任，双向剪贴板已停用") }
        else if clientReady && !clientEnabled { set(.clipboard, .unknown, "客户端已关闭，双向剪贴板已停用") }
        else if !hubConnected { set(.clipboard, .unknown, "等待连接中枢") }
        else if clipboardEnabled { set(.clipboard, .ok, "已开启：复制的文本会同步到其他在线设备") }
        else { set(.clipboard, .unknown, "已关闭（可在管理页面或菜单栏中开启）") }
    }

    func updateControlStatus() {
        if hubConnected && !authVerified { set(.control, .warn, "设备未获信任，共享保持停用；请核对指纹并批准") }
        else if clientReady && !clientEnabled { set(.control, .warn, "客户端已关闭，可在管理页面或本机重新开启") }
        else if captured { set(.control, .ok, "本机正在控制 \(deviceName(activeTarget))（Ctrl+Option+Esc 收回）") }
        else if controlled { set(.control, .ok, "\(deviceName(activeSource)) 正在控制本机") }
        else { set(.control, .unknown, "空闲：把光标移出屏幕边缘即可控制相邻设备") }
    }
}
