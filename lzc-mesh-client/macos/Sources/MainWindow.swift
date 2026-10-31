// Main window: header with live connection status and the sharing switch, then two
// columns. Left: screen layout and shared files. Right: connection status, settings
// and the event log. Black / white / grey, same look as the management page.
import AppKit

final class MainWindow: NSObject, NSWindowDelegate {
    let window: NSWindow
    let layoutView = LayoutView()
    let filesView = FilesView()
    var actions: [String: () -> Void] = [:]

    private var dots: [StatusKey: StatusDot] = [:]
    private var texts: [StatusKey: NSTextField] = [:]
    private let pillDot = StatusDot()
    private let pillText = makeLabel("正在连接…", size: 12, color: Theme.muted)
    private let controlText = makeLabel("", size: 12, weight: .medium)
    private let sharingSwitch = ToggleSwitch()
    private let swapSwitch = ToggleSwitch()
    private let autostartSwitch = ToggleSwitch()
    private let boxField = NSTextField()
    private let logView = NSTextView()
    private var permRow: NSView!

    // MARK: setup

    override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1080, height: 700),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        window.title = "懒猫设备接力"
        window.titleVisibility = .hidden
        window.titlebarAppearsTransparent = true
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.appearance = NSAppearance(named: .aqua)
        window.backgroundColor = Theme.bg
        window.minSize = NSSize(width: 860, height: 560)
        window.setFrameAutosaveName("LZCMeshMain")
        build()
        if !window.setFrameUsingName("LZCMeshMain") { window.center() }
    }

    private func button(_ title: String, _ key: String, primary: Bool = false) -> FlatButton {
        let b = FlatButton(title: title, primary: primary, target: self, action: #selector(clicked(_:)))
        b.identifier = NSUserInterfaceItemIdentifier(key)
        return b
    }

    @objc private func clicked(_ b: NSButton) { actions[b.identifier?.rawValue ?? ""]?() }
    @objc private func toggled(_ s: ToggleSwitch) { actions[s.identifier?.rawValue ?? ""]?() }

    private func titled(_ title: String, _ content: NSView, trailing: NSView? = nil) -> Panel {
        let p = Panel()
        let head = NSStackView(views: [makeLabel(title, size: 13, weight: .semibold)] + (trailing.map { [NSView(), $0] } ?? []))
        head.spacing = 8
        let stack = NSStackView(views: [head, content])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        p.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: p.topAnchor, constant: 14), stack.bottomAnchor.constraint(equalTo: p.bottomAnchor, constant: -14),
            stack.leadingAnchor.constraint(equalTo: p.leadingAnchor, constant: 14), stack.trailingAnchor.constraint(equalTo: p.trailingAnchor, constant: -14),
            head.widthAnchor.constraint(equalTo: stack.widthAnchor), content.widthAnchor.constraint(equalTo: stack.widthAnchor),
        ])
        return p
    }

    private func settingRow(_ title: String, _ detail: String?, _ toggle: ToggleSwitch, key: String) -> NSView {
        toggle.identifier = NSUserInterfaceItemIdentifier(key)
        toggle.target = self
        toggle.action = #selector(toggled(_:))
        let texts = NSStackView(views: [makeLabel(title, size: 12)] + (detail.map { [makeLabel($0, size: 11, color: Theme.muted, lines: 2)] } ?? []))
        texts.orientation = .vertical
        texts.alignment = .leading
        texts.spacing = 2
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        spacer.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let row = NSStackView(views: [texts, spacer, toggle])
        row.alignment = .top
        row.spacing = 12
        return row
    }

    private func build() {
        // Header
        let title = makeLabel("懒猫设备接力", size: 15, weight: .semibold)
        let pill = NSStackView(views: [pillDot, pillText])
        pill.spacing = 7
        pill.alignment = .centerY
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        sharingSwitch.identifier = NSUserInterfaceItemIdentifier("sharing")
        sharingSwitch.target = self
        sharingSwitch.action = #selector(toggled(_:))
        let sharingLabel = makeLabel("键鼠共享", size: 12, weight: .medium)
        let header = NSStackView(views: [title, pill, controlText, spacer, sharingLabel, sharingSwitch])
        header.spacing = 14
        header.alignment = .centerY
        header.edgeInsets = NSEdgeInsets(top: 0, left: 84, bottom: 0, right: 24)

        // Left column: layout + files
        let layoutHint = makeLabel("拖动屏幕调整相对位置，松手后自动贴边并同步到所有设备。控制其他设备时按 Ctrl+Option+Esc 收回光标。", size: 11, color: Theme.muted, lines: 2)
        layoutView.translatesAutoresizingMaskIntoConstraints = false
        let layoutBox = NSStackView(views: [layoutHint, layoutView])
        layoutBox.orientation = .vertical
        layoutBox.alignment = .leading
        layoutBox.spacing = 8
        layoutHint.widthAnchor.constraint(equalTo: layoutBox.widthAnchor).isActive = true
        layoutView.widthAnchor.constraint(equalTo: layoutBox.widthAnchor).isActive = true
        layoutView.heightAnchor.constraint(greaterThanOrEqualToConstant: 200).isActive = true
        let layoutPanel = titled("屏幕布局", layoutBox)
        filesView.translatesAutoresizingMaskIntoConstraints = false
        let filesPanel = titled("文件共享", filesView)
        let left = NSStackView(views: [layoutPanel, filesPanel])
        left.orientation = .vertical
        left.spacing = 14
        layoutPanel.widthAnchor.constraint(equalTo: left.widthAnchor).isActive = true
        filesPanel.widthAnchor.constraint(equalTo: left.widthAnchor).isActive = true
        layoutPanel.setContentHuggingPriority(.defaultLow, for: .vertical)
        filesPanel.heightAnchor.constraint(greaterThanOrEqualToConstant: 190).isActive = true

        // Right column: status, settings, log
        let grid = NSGridView()
        grid.rowSpacing = 7
        grid.columnSpacing = 9
        for k in StatusKey.allCases {
            let dot = StatusDot()
            let text = makeLabel("检测中…", size: 12, lines: 3)
            text.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            dots[k] = dot
            texts[k] = text
            // The dot sits in a wrapper whose top inset lines it up with the first text line.
            let slot = NSView()
            dot.translatesAutoresizingMaskIntoConstraints = false
            slot.addSubview(dot)
            NSLayoutConstraint.activate([
                dot.centerXAnchor.constraint(equalTo: slot.centerXAnchor), dot.topAnchor.constraint(equalTo: slot.topAnchor, constant: 4),
                dot.widthAnchor.constraint(equalToConstant: 8), dot.heightAnchor.constraint(equalToConstant: 8),
            ])
            grid.addRow(with: [slot, makeLabel(k.label, size: 12, color: Theme.muted), text])
        }
        grid.column(at: 0).xPlacement = .center
        grid.column(at: 0).width = 10
        grid.column(at: 1).width = 84
        for r in 0..<grid.numberOfRows { grid.row(at: r).yPlacement = .top }
        let permBtn = button("请求系统权限", "perm", primary: true)
        let permSettings = button("打开权限设置", "settings")
        let permBar = NSStackView(views: [permBtn, permSettings])
        permBar.spacing = 8
        permRow = permBar
        let retry = button("重新检测", "retry"), open = button("打开管理页面", "open"), copy = button("复制诊断信息", "copy")
        let bar = NSStackView(views: [retry, open, copy])
        bar.spacing = 8
        let statusStack = NSStackView(views: [grid, permBar, bar])
        statusStack.orientation = .vertical
        statusStack.alignment = .leading
        statusStack.spacing = 10
        grid.widthAnchor.constraint(equalTo: statusStack.widthAnchor).isActive = true
        let statusPanel = titled("连接状态", statusStack)

        boxField.placeholderString = "默认使用懒猫客户端的微服，例如 ponzs"
        boxField.stringValue = AppState.shared.boxOverride
        boxField.font = Theme.font(12)
        boxField.bezelStyle = .roundedBezel
        boxField.focusRingType = .none
        let boxRow = NSStackView(views: [boxField, button("应用", "box")])
        boxRow.spacing = 8
        let boxCol = NSStackView(views: [makeLabel("使用的微服", size: 12), boxRow])
        boxCol.orientation = .vertical
        boxCol.alignment = .leading
        boxCol.spacing = 5
        boxRow.widthAnchor.constraint(equalTo: boxCol.widthAnchor).isActive = true
        let settingsStack = NSStackView(views: [
            settingRow("开机自动启动", "登录系统后自动在菜单栏运行", autostartSwitch, key: "autostart"),
            settingRow("交换 Command 和 Ctrl", "Mac 与 Windows/Linux 互控时对调这两个键", swapSwitch, key: "swap"),
            boxCol,
        ])
        settingsStack.orientation = .vertical
        settingsStack.alignment = .leading
        settingsStack.spacing = 12
        for v in settingsStack.arrangedSubviews { v.widthAnchor.constraint(equalTo: settingsStack.widthAnchor).isActive = true }
        let settingsPanel = titled("设置", settingsStack)

        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.drawsBackground = false
        logView.isEditable = false
        logView.drawsBackground = false
        logView.font = Theme.mono(11)
        logView.textColor = NSColor(white: 0.25, alpha: 1)
        logView.textContainerInset = NSSize(width: 0, height: 2)
        logView.autoresizingMask = [.width]
        scroll.documentView = logView
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 110).isActive = true
        let logPanel = titled("事件日志", scroll)

        let right = NSStackView(views: [statusPanel, settingsPanel, logPanel])
        right.orientation = .vertical
        right.spacing = 14
        for p in [statusPanel, settingsPanel, logPanel] { p.widthAnchor.constraint(equalTo: right.widthAnchor).isActive = true }
        logPanel.setContentHuggingPriority(.defaultLow, for: .vertical)

        let columns = NSStackView(views: [left, right])
        columns.spacing = 14
        columns.alignment = .top
        columns.distribution = .fill
        left.setContentHuggingPriority(.defaultLow, for: .horizontal)
        left.heightAnchor.constraint(equalTo: columns.heightAnchor).isActive = true
        right.heightAnchor.constraint(equalTo: columns.heightAnchor).isActive = true
        right.widthAnchor.constraint(equalToConstant: 380).isActive = true
        columns.edgeInsets = NSEdgeInsets(top: 0, left: 24, bottom: 24, right: 24)

        let content = NSView()
        for v in [header, columns] { v.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(v) }
        NSLayoutConstraint.activate([
            header.topAnchor.constraint(equalTo: content.topAnchor, constant: 8), header.heightAnchor.constraint(equalToConstant: 44),
            header.leadingAnchor.constraint(equalTo: content.leadingAnchor), header.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            columns.topAnchor.constraint(equalTo: header.bottomAnchor, constant: 6), columns.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            columns.leadingAnchor.constraint(equalTo: content.leadingAnchor), columns.trailingAnchor.constraint(equalTo: content.trailingAnchor),
        ])
        window.contentView = content
        refreshStatus()
    }

    // MARK: updates

    func refreshStatus() {
        let s = AppState.shared
        for k in StatusKey.allCases {
            let (level, text) = s.status[k] ?? (.unknown, "")
            dots[k]?.level = level
            texts[k]?.stringValue = text
            texts[k]?.font = Theme.font(12, level == .error ? .semibold : .regular)
        }
        let (lv, line) = s.summary()
        pillDot.level = lv
        pillText.stringValue = line
        controlText.stringValue = s.controlLine().map { "· " + $0 } ?? ""
        sharingSwitch.isOn = s.sharingEnabled
        autostartSwitch.isOn = LoginItem.isEnabled
        permRow.isHidden = s.status[.permission]?.0 == .ok
    }

    func refreshLayout() {
        layoutView.needsDisplay = true
        swapSwitch.isOn = AppState.shared.swapCmdCtrl
        refreshStatus()
    }

    func refreshSettings() {
        sharingSwitch.isOn = AppState.shared.sharingEnabled
        autostartSwitch.isOn = LoginItem.isEnabled
    }

    var boxText: String { boxField.stringValue }

    func appendLog(_ line: String) {
        let storage = logView.textStorage!
        storage.append(NSAttributedString(string: line + "\n", attributes: [.font: logView.font!, .foregroundColor: logView.textColor!]))
        if storage.length > 60_000 { storage.deleteCharacters(in: NSRange(location: 0, length: 20_000)) }
        logView.scrollToEndOfDocument(nil)
    }

    var logText: String { logView.string }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        // Keep sharing input in the background; the menu bar icon reopens the window.
        sender.orderOut(nil)
        return false
    }
}
