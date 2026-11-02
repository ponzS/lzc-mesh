// Menu bar item: connection status, the sharing / clipboard switches and quit.
import AppKit

final class MenuBar: NSObject, NSMenuDelegate {
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()
    var actions: [String: () -> Void] = [:]

    override init() {
        super.init()
        menu.delegate = self
        menu.autoenablesItems = false
        item.menu = menu
        item.button?.toolTip = "懒猫设备接力"
        refreshIcon()
    }

    /// Template image drawn in code, so it follows the menu bar's light / dark appearance.
    /// Two small screens with a cursor between them; dimmed when sharing is paused.
    func refreshIcon() {
        let s = AppState.shared
        let img = NSImage(size: NSSize(width: 18, height: 18), flipped: false) { _ in
            NSColor.black.setStroke()
            NSColor.black.setFill()
            let a = NSBezierPath(roundedRect: NSRect(x: 1.5, y: 8.5, width: 10, height: 7), xRadius: 1.5, yRadius: 1.5)
            let b = NSBezierPath(roundedRect: NSRect(x: 6.5, y: 2.5, width: 10, height: 7), xRadius: 1.5, yRadius: 1.5)
            a.lineWidth = 1.4; b.lineWidth = 1.4
            if s.captured || s.controlled { b.fill() }
            a.stroke(); b.stroke()
            return true
        }
        img.isTemplate = true
        item.button?.image = img
        item.button?.appearsDisabled = !s.sharingEnabled || !s.operational
    }

    private func add(_ title: String, key: String? = nil, state: Bool? = nil, enabled: Bool = true) {
        let mi = NSMenuItem(title: title, action: key == nil ? nil : #selector(fire(_:)), keyEquivalent: "")
        mi.target = self
        mi.representedObject = key
        mi.isEnabled = enabled && key != nil
        if let state = state { mi.state = state ? .on : .off }
        if key == nil { mi.attributedTitle = NSAttributedString(string: title, attributes: [.font: NSFont.menuFont(ofSize: 13), .foregroundColor: NSColor.secondaryLabelColor]) }
        menu.addItem(mi)
    }

    @objc private func fire(_ mi: NSMenuItem) { if let k = mi.representedObject as? String { actions[k]?() } }

    func menuNeedsUpdate(_ menu: NSMenu) {
        let s = AppState.shared
        menu.removeAllItems()
        let (_, line) = s.summary()
        add(line)
        if let c = s.controlLine() { add(c) }
        menu.addItem(.separator())
        add("客户端运行", key: "client", state: s.clientEnabled, enabled: s.hubConnected && s.clientReady && s.clientControl)
        add("键鼠共享", key: "sharing", state: s.sharingEnabled, enabled: s.operational)
        add("共享剪贴板", key: "clipboard", state: s.clipboardEnabled, enabled: s.operational)
        add("开机自动启动", key: "autostart", state: LoginItem.isEnabled)
        menu.addItem(.separator())
        add("打开主窗口", key: "show")
        add("打开管理页面", key: "open", enabled: !s.boxDomain.isEmpty)
        menu.addItem(.separator())
        add("退出懒猫设备接力", key: "quit")
    }
}
