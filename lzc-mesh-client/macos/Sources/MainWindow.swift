// Main window: status of every dependency, permission actions, the layout
// editor and a live event log. Black/white/grey AppKit UI.
import AppKit

final class LayoutView: NSView {
    var onMove: ((String, Double, Double) -> Void)?
    private var scale = 1.0, ox = 0.0, oy = 0.0
    private var dragIndex: Int?
    private var dragOffset = CGPoint.zero, dragOrigin = CGPoint.zero

    override var isFlipped: Bool { true }

    private func compute() {
        let ds = AppState.shared.devices
        guard !ds.isEmpty else { return }
        let x0 = ds.map { $0.x }.min()!, y0 = ds.map { $0.y }.min()!
        let x1 = ds.map { $0.x + $0.w }.max()!, y1 = ds.map { $0.y + $0.h }.max()!
        let pad = 24.0, W = bounds.width - 2 * pad, H = bounds.height - 2 * pad
        scale = min(W / ((x1 - x0) * 1.6), H / ((y1 - y0) * 1.6), 0.25)
        ox = pad + (W - (x1 - x0) * scale) / 2 - x0 * scale
        oy = pad + (H - (y1 - y0) * scale) / 2 - y0 * scale
    }

    private func rect(_ i: Int) -> CGRect {
        let d = AppState.shared.devices[i]
        if i == dragIndex { return CGRect(x: dragOrigin.x, y: dragOrigin.y, width: d.w * scale, height: d.h * scale) }
        return CGRect(x: ox + d.x * scale, y: oy + d.y * scale, width: d.w * scale, height: d.h * scale)
    }

    override func draw(_ dirty: NSRect) {
        NSColor(white: 0.97, alpha: 1).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
        NSColor(white: 0.88, alpha: 1).setFill()
        for x in stride(from: 8.0, to: bounds.width, by: 16) { for y in stride(from: 8.0, to: bounds.height, by: 16) { NSRect(x: x, y: y, width: 1, height: 1).fill() } }
        let s = AppState.shared
        let para = NSMutableParagraphStyle()
        para.alignment = .center
        para.lineBreakMode = .byTruncatingTail
        if s.devices.isEmpty {
            let t = s.hubConnected ? "暂无设备" : "连接 LZC Mesh 服务后显示屏幕布局"
            (t as NSString).draw(in: bounds.insetBy(dx: 0, dy: bounds.height / 2 - 10),
                                 withAttributes: [.foregroundColor: NSColor.gray, .paragraphStyle: para, .font: NSFont.systemFont(ofSize: 12)])
            return
        }
        if dragIndex == nil { compute() }
        for (i, d) in s.devices.enumerated() {
            let r = rect(i)
            let me = d.id == s.deviceID
            let active = d.id == s.activeSource || d.id == s.activeTarget
            let path = NSBezierPath(roundedRect: r, xRadius: 5, yRadius: 5)
            NSColor(white: me ? 0.92 : 1, alpha: 1).setFill()
            path.fill()
            NSColor(white: active ? 0.07 : d.online ? 0.45 : 0.75, alpha: 1).setStroke()
            path.lineWidth = active ? 2.5 : 1.5
            if !d.online { path.setLineDash([4, 3], count: 2, phase: 0) }
            path.stroke()
            let plat = ["macos": "macOS", "windows": "Windows", "linux": "Linux"][d.platform] ?? d.platform
            let title = NSMutableAttributedString(string: d.name + (me ? "（本机）" : "") + "\n",
                                                  attributes: [.font: NSFont.boldSystemFont(ofSize: 12), .foregroundColor: NSColor(white: d.online ? 0.07 : 0.5, alpha: 1), .paragraphStyle: para])
            title.append(NSAttributedString(string: "\(plat) · \(Int(d.w))×\(Int(d.h))\(d.online ? "" : " · 离线")",
                                            attributes: [.font: NSFont.systemFont(ofSize: 10), .foregroundColor: NSColor.gray, .paragraphStyle: para]))
            let th = title.boundingRect(with: CGSize(width: r.width - 8, height: r.height), options: [.usesLineFragmentOrigin]).height
            title.draw(with: CGRect(x: r.minX + 4, y: r.midY - th / 2, width: r.width - 8, height: th), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
        }
    }

    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        guard AppState.shared.hubConnected else { return }
        for i in AppState.shared.devices.indices.reversed() where rect(i).contains(p) {
            dragIndex = i
            dragOrigin = rect(i).origin
            dragOffset = CGPoint(x: p.x - dragOrigin.x, y: p.y - dragOrigin.y)
            return
        }
    }

    override func mouseDragged(with e: NSEvent) {
        guard dragIndex != nil else { return }
        let p = convert(e.locationInWindow, from: nil)
        dragOrigin = CGPoint(x: p.x - dragOffset.x, y: p.y - dragOffset.y)
        needsDisplay = true
    }

    override func mouseUp(with e: NSEvent) {
        guard let i = dragIndex, i < AppState.shared.devices.count else { dragIndex = nil; return }
        let d = AppState.shared.devices[i]
        onMove?(d.id, (dragOrigin.x - ox) / scale, (dragOrigin.y - oy) / scale)
        dragIndex = nil
        needsDisplay = true
    }
}

final class MainWindow: NSObject, NSWindowDelegate {
    let window: NSWindow
    private var dots: [StatusKey: NSView] = [:]
    private var texts: [StatusKey: NSTextField] = [:]
    let layoutView = LayoutView()
    let swap = NSButton(checkboxWithTitle: "Mac 与 Windows/Linux 互控时交换 Command 和 Ctrl", target: nil, action: nil)
    let boxField = NSTextField()
    let logView = NSTextView()
    let permButton = NSButton(title: "请求系统权限", target: nil, action: nil)
    var actions: [String: () -> Void] = [:]

    override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 880, height: 820), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        super.init()
        window.title = "LZC Mesh"
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.appearance = NSAppearance(named: .aqua)
        window.backgroundColor = NSColor(white: 0.96, alpha: 1)
        build()
        window.center()
    }

    private func label(_ s: String, size: CGFloat = 12, color: NSColor = .labelColor, bold: Bool = false) -> NSTextField {
        let l = NSTextField(wrappingLabelWithString: s)
        l.font = bold ? .boldSystemFont(ofSize: size) : .systemFont(ofSize: size)
        l.textColor = color
        l.isSelectable = true
        return l
    }

    private func card(_ title: String, _ views: [NSView]) -> NSView {
        let box = NSStackView(views: [label(title, size: 13, bold: true)] + views)
        box.orientation = .vertical
        box.alignment = .leading
        box.spacing = 8
        box.edgeInsets = NSEdgeInsets(top: 14, left: 14, bottom: 14, right: 14)
        box.wantsLayer = true
        box.layer?.backgroundColor = NSColor.white.cgColor
        box.layer?.cornerRadius = 10
        box.layer?.borderWidth = 1
        box.layer?.borderColor = NSColor(white: 0.88, alpha: 1).cgColor
        for v in views { v.translatesAutoresizingMaskIntoConstraints = false; v.widthAnchor.constraint(equalTo: box.widthAnchor, constant: -28).isActive = true }
        return box
    }

    private func button(_ title: String, _ key: String) -> NSButton {
        let b = NSButton(title: title, target: self, action: #selector(clicked(_:)))
        b.identifier = NSUserInterfaceItemIdentifier(key)
        b.bezelStyle = .rounded
        return b
    }

    @objc private func clicked(_ b: NSButton) { actions[b.identifier?.rawValue ?? ""]?() }

    private func build() {
        let grid = NSGridView()
        grid.rowSpacing = 6
        grid.columnSpacing = 10
        for k in StatusKey.allCases {
            let dot = NSView()
            dot.wantsLayer = true
            dot.layer?.cornerRadius = 4
            dot.translatesAutoresizingMaskIntoConstraints = false
            dot.widthAnchor.constraint(equalToConstant: 8).isActive = true
            dot.heightAnchor.constraint(equalToConstant: 8).isActive = true
            let text = label("检测中…")
            dots[k] = dot
            texts[k] = text
            grid.addRow(with: [dot, label(k.label, color: .darkGray), text])
        }
        grid.column(at: 0).xPlacement = .center
        grid.rowAlignment = .firstBaseline
        for r in 0..<grid.numberOfRows { grid.row(at: r).yPlacement = .center }
        grid.column(at: 2).width = 560

        boxField.placeholderString = "留空使用懒猫客户端默认微服，例如 ponzs"
        boxField.stringValue = AppState.shared.boxOverride
        let boxRow = NSStackView(views: [label("使用的微服", color: .darkGray), boxField, button("应用", "box")])
        boxRow.spacing = 8
        permButton.target = self
        permButton.action = #selector(clicked(_:))
        permButton.identifier = NSUserInterfaceItemIdentifier("perm")
        permButton.bezelStyle = .rounded
        let actionsRow = NSStackView(views: [button("重新检测", "retry"), permButton, button("打开权限设置", "settings"),
                                             button("打开管理页面", "open"), button("复制诊断信息", "copy"), button("退出", "quit")])
        actionsRow.spacing = 8

        let hint = label("拖动屏幕调整相对位置，松手后自动贴边并同步到所有设备。控制其他设备时按 Ctrl+Option+Esc 收回光标。", size: 11, color: .gray)
        layoutView.translatesAutoresizingMaskIntoConstraints = false
        layoutView.heightAnchor.constraint(greaterThanOrEqualToConstant: 240).isActive = true
        swap.target = self
        swap.action = #selector(clicked(_:))
        swap.identifier = NSUserInterfaceItemIdentifier("swap")

        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        logView.isEditable = false
        logView.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        logView.backgroundColor = NSColor(white: 0.98, alpha: 1)
        logView.textColor = NSColor(white: 0.2, alpha: 1)
        logView.autoresizingMask = [.width]
        scroll.documentView = logView
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.heightAnchor.constraint(equalToConstant: 120).isActive = true

        let root = NSStackView(views: [card("状态", [grid, boxRow, actionsRow]), card("屏幕布局", [hint, layoutView, swap]), card("事件日志", [scroll])])
        root.orientation = .vertical
        root.alignment = .leading
        root.spacing = 12
        root.edgeInsets = NSEdgeInsets(top: 16, left: 16, bottom: 16, right: 16)
        root.setHuggingPriority(.defaultLow, for: .vertical)
        for v in root.arrangedSubviews { v.translatesAutoresizingMaskIntoConstraints = false; v.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -32).isActive = true }
        window.contentView = root
    }

    func refreshStatus() {
        let s = AppState.shared
        for k in StatusKey.allCases {
            let (level, text) = s.status[k] ?? (.unknown, "")
            let layer = dots[k]?.layer
            layer?.borderWidth = level == .error ? 2 : 0
            layer?.borderColor = NSColor.black.cgColor
            layer?.backgroundColor = (level == .ok ? NSColor.black : level == .warn ? NSColor.gray : level == .error ? NSColor.white : NSColor(white: 0.8, alpha: 1)).cgColor
            texts[k]?.stringValue = text
            texts[k]?.font = level == .error ? .boldSystemFont(ofSize: 12) : .systemFont(ofSize: 12)
        }
        permButton.isHidden = s.status[.permission]?.0 == .ok
    }

    func refreshLayout() {
        layoutView.needsDisplay = true
        swap.state = AppState.shared.swapCmdCtrl ? .on : .off
    }

    func appendLog(_ line: String) {
        let storage = logView.textStorage!
        storage.append(NSAttributedString(string: line + "\n", attributes: [.font: logView.font!, .foregroundColor: logView.textColor!]))
        if storage.length > 60_000 { storage.deleteCharacters(in: NSRange(location: 0, length: 20_000)) }
        logView.scrollToEndOfDocument(nil)
    }

    var logText: String { logView.string }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        // Keep sharing input in the background; the Dock icon / menu reopens the window.
        sender.orderOut(nil)
        return false
    }
}
