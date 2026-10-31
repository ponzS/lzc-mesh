// LZC Mesh for macOS: wires the LazyCat probe, hub connection, input and window.
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    let state = AppState.shared
    let lzc = Lzc()
    let hub = Hub()
    let input = MacInput()
    var ui: MainWindow!
    var timer: Timer?

    func applicationDidFinishLaunching(_ n: Notification) {
        ui = MainWindow()
        state.onStatus = { [weak self] in self?.ui.refreshStatus() }
        state.onDevices = { [weak self] in self?.ui.refreshLayout() }
        state.onLog = { [weak self] in self?.ui.appendLog($0) }
        hub.input = input
        input.hub = hub
        hub.onCookieRejected = { [weak self] in self?.lzc.invalidate() }
        lzc.onReady = { [weak self] ready, changed in
            guard let self = self else { return }
            if changed && self.state.hubConnected { self.state.log("懒猫微服或登录会话变化，重新连接"); self.hub.disconnect() }
            if ready && !self.state.hubConnected && !self.hub.connecting { self.hub.connect() }
            if !ready && self.state.hubConnected { self.hub.disconnect() }
        }
        ui.layoutView.onMove = { [weak self] id, x, y in self?.hub.send(["t": "layout", "id": id, "x": x, "y": y]) }
        ui.actions = [
            "retry": { [weak self] in self?.state.log("手动重新检测"); self?.lzc.invalidate(); self?.hub.disconnect(); self?.lzc.refresh() },
            "perm": { [weak self] in self?.input.requestPermissions() },
            "settings": { MacInput.openSettings(AXIsProcessTrusted() ? "Privacy_ListenEvent" : "Privacy_Accessibility") },
            "open": { [weak self] in
                guard let d = self?.state.boxDomain, !d.isEmpty else { return }
                NSWorkspace.shared.open(URL(string: "https://lzc-mesh.\(d)/")!)
            },
            "copy": { [weak self] in self?.copyDiagnostics() },
            "quit": { NSApp.terminate(nil) },
            "swap": { [weak self] in self?.hub.send(["t": "settings", "swap_cmd_ctrl": self?.ui.swap.state == .on]) },
            "box": { [weak self] in self?.applyBox() },
        ]
        state.log("LZC Mesh macOS \(AppState.version) 启动，设备 ID \(state.deviceID)")
        updateScreen()
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in self?.updateScreen() }
        // Ask for missing permissions right away; the system shows its own prompts.
        if !input.permissions().ok { input.requestPermissions() }
        input.refreshPermissionStatus()
        ui.refreshStatus()
        ui.window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        lzc.refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.lzc.refresh()
            self?.input.refreshPermissionStatus()
        }
    }

    func updateScreen() {
        let b = MacInput.desktopBounds()
        if b.width != state.screenW || b.height != state.screenH {
            state.screenW = b.width
            state.screenH = b.height
            state.set(.screen, .ok, "\(Int(b.width))×\(Int(b.height))（\(NSScreen.screens.count) 个显示器，逻辑点）")
            if state.hubConnected { hub.sendHello() }
        }
    }

    func applyBox() {
        var name = ui.boxField.stringValue.trimmingCharacters(in: .whitespaces).lowercased()
        if name.hasSuffix(".heiyu.space") { name = String(name.dropLast(".heiyu.space".count)) }
        guard name.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "-" }) else { state.log("微服名只能包含字母、数字和 -"); return }
        state.boxOverride = name
        state.save()
        state.log("使用微服：\(name.isEmpty ? "懒猫客户端默认微服" : name)")
        lzc.invalidate()
        hub.disconnect()
        lzc.refresh()
    }

    func copyDiagnostics() {
        var s = "LZC Mesh macOS \(AppState.version)\n设备 ID: \(state.deviceID)\n微服: \(state.boxDomain)\n"
        for k in StatusKey.allCases { s += "\(k.label): \(state.status[k]?.1 ?? "")\n" }
        s += "\n" + ui.logText
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
        state.log("诊断信息已复制到剪贴板")
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        ui.window.makeKeyAndOrderFront(nil)
        return true
    }

    func applicationWillTerminate(_ n: Notification) {
        if state.captured { input.capture(false) }
        input.leave()
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
let menu = NSMenu()
let appItem = NSMenuItem()
menu.addItem(appItem)
let appMenu = NSMenu()
appMenu.addItem(withTitle: "退出 LZC Mesh", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
appItem.submenu = appMenu
app.mainMenu = menu
app.run()
