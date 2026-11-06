// 懒猫设备接力 (LZC Mesh) for macOS: wires the LazyCat probe, hub connection, input, clipboard and window.
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    let state = AppState.shared
    let lzc = Lzc()
    let hub = Hub()
    let input = MacInput()
    let clipboard = Clipboard()
    var ui: MainWindow!
    var bar: MenuBar!
    var timer: Timer?

    func applicationDidFinishLaunching(_ n: Notification) {
        ui = MainWindow()
        bar = MenuBar()
        state.onStatus = { [weak self] in self?.ui.refreshStatus(); self?.bar.refreshIcon() }
        state.onDevices = { [weak self] in self?.ui.refreshLayout(); self?.bar.refreshIcon() }
        state.onLog = { [weak self] in self?.ui.appendLog($0) }
        hub.input = input
        hub.clipboard = clipboard
        clipboard.hub = hub
        state.onFiles = { [weak self] in self?.ui.filesView.update(AppState.shared.files) }
        state.onActivity = { [weak self] on in
            guard let self else { return }
            self.input.sharingEnabled = on && self.state.sharingEnabled
            if on && !self.input.permissions().ok { self.input.requestPermissions() }
            self.input.setActive(on)
            self.clipboard.setActive(on)
            self.ui.filesView.setActive(on)
            if !on { self.state.captured = false; self.state.controlled = false }
            self.ui.refreshStatus(); self.bar.refreshIcon()
        }
        input.hub = hub
        input.sharingEnabled = false
        hub.onCookieRejected = { [weak self] in self?.lzc.invalidate() }
        lzc.onReady = { [weak self] ready, changed in
            guard let self = self else { return }
            if changed && (self.state.hubConnected || self.hub.connecting) { self.state.log("懒猫微服或登录会话变化，重新连接"); self.hub.disconnect() }
            if ready && !self.state.hubConnected && !self.hub.connecting { self.hub.connect() }
            if !ready && (self.state.hubConnected || self.hub.connecting) { self.hub.disconnect() }
        }
        ui.layoutView.onMove = { [weak self] id, x, y in self?.hub.send(["t": "layout", "id": id, "x": x, "y": y]) }
        let shared: [String: () -> Void] = [
            "sharing": { [weak self] in self?.toggleSharing() },
            "client": { [weak self] in
                guard let self, self.state.hubConnected, self.state.clientControl else { return }
                self.hub.send(["t": "device_enabled", "id": self.state.deviceID, "enabled": !self.state.clientEnabled])
            },
            "clipboard": { [weak self] in self?.hub.send(["t": "settings", "clipboard": !(self?.state.clipboardEnabled ?? false)]) },
            "autostart": { [weak self] in self?.toggleAutostart() },
            "open": { [weak self] in
                guard let d = self?.state.boxDomain, !d.isEmpty else { return }
                NSWorkspace.shared.open(URL(string: "https://lzc-mesh.\(d)/")!)
            },
            "show": { [weak self] in self?.showWindow() },
            "quit": { NSApp.terminate(nil) },
        ]
        ui.actions = shared.merging([
            "retry": { [weak self] in self?.state.log("手动重新检测"); self?.lzc.invalidate(); self?.hub.disconnect(); self?.lzc.refresh() },
            "perm": { [weak self] in self?.input.requestPermissions() },
            "settings": { MacInput.openSettings(AXIsProcessTrusted() ? "Privacy_ListenEvent" : "Privacy_Accessibility") },
            "copy": { [weak self] in self?.copyDiagnostics() },
            "transport": { [weak self] in
                guard let self else { return }
                self.hub.send(["t": "settings", "transport_mode": self.ui.selectedTransportMode])
            },
            "swap": { [weak self] in self?.hub.send(["t": "settings", "swap_cmd_ctrl": !(self?.state.swapCmdCtrl ?? true)]) },
            "autocenter": { [weak self] in
                guard let self else { return }
                self.hub.setAutoCenter(!self.state.autoCenter)
                self.ui.refreshSettings()
            },
            "box": { [weak self] in self?.applyBox() },
        ]) { a, _ in a }
        bar.actions = shared
        state.log("懒猫设备接力 macOS \(AppState.version) 启动，设备 ID \(state.deviceID)")
        clipboard.start()
        updateScreen()
        NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification, object: nil, queue: .main) { [weak self] _ in self?.updateScreen() }
        // A dormant client does not install input monitors or request permissions.
        input.refreshPermissionStatus()
        ui.refreshStatus()
        ui.refreshSettings()
        // Started at login: stay in the menu bar without opening a window.
        if !CommandLine.arguments.contains("--background") { showWindow() }
        lzc.refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            self?.updateScreen()
            self?.lzc.refresh()
            self?.input.refreshPermissionStatus()
        }
    }

    func showWindow() {
        ui.window.makeKeyAndOrderFront(nil)
        ui.window.makeFirstResponder(nil)   // don't open with a blinking cursor in the box field
        NSApp.activate(ignoringOtherApps: true)
    }

    func toggleSharing() {
        guard state.operational else { return }
        state.sharingEnabled.toggle()
        state.save()
        input.sharingEnabled = state.sharingEnabled
        // Give the cursor back at once if this Mac is controlling another device.
        if !state.sharingEnabled && state.captured { hub.send(["t": "release"]) }
        hub.send(["t": "sharing", "enabled": state.sharingEnabled])
        state.log(state.sharingEnabled ? "键鼠共享已开启" : "键鼠共享已暂停")
        ui.refreshSettings()
        bar.refreshIcon()
    }

    func toggleAutostart() {
        if let err = LoginItem.set(!LoginItem.isEnabled) { state.log("设置开机启动失败：\(err)") }
        else { state.log(LoginItem.isEnabled ? "已开启开机自动启动" : "已关闭开机自动启动") }
        ui.refreshSettings()
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
        var name = ui.boxText.trimmingCharacters(in: .whitespaces).lowercased()
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
        var s = "懒猫设备接力 macOS \(AppState.version)\n设备 ID: \(state.deviceID)\n微服: \(state.boxDomain)\n"
        for k in StatusKey.allCases { s += "\(k.label): \(state.status[k]?.1 ?? "")\n" }
        s += "\n" + ui.logText
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
        state.log("诊断信息已复制到剪贴板")
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showWindow()
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
// Menu bar app: no Dock icon. The window opens from the menu bar item.
app.setActivationPolicy(.accessory)
let menu = NSMenu()
let appItem = NSMenuItem()
menu.addItem(appItem)
let appMenu = NSMenu()
appMenu.addItem(withTitle: "退出懒猫设备接力", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
appItem.submenu = appMenu
app.mainMenu = menu
app.run()
