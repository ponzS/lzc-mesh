// Drag-to-arrange screen layout. Same model as the web page: devices are drawn
// scaled to fit; releasing a block sends its new position to the hub, which snaps it
// and broadcasts the result to every device.
import AppKit

final class LayoutView: NSView {
    var onMove: ((String, Double, Double) -> Void)?
    private var scale = 1.0, ox = 0.0, oy = 0.0
    private var dragIndex: Int?
    private var dragOffset = CGPoint.zero      // pointer position inside the dragged block
    private var dragOrigin = CGPoint.zero      // top-left of the dragged block, in view points
    private var moved = false

    override var isFlipped: Bool { true }
    override var mouseDownCanMoveWindow: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    private func compute() {
        let ds = AppState.shared.devices
        guard !ds.isEmpty else { return }
        let x0 = ds.map { $0.x }.min()!, y0 = ds.map { $0.y }.min()!
        let x1 = ds.map { $0.x + $0.w }.max()!, y1 = ds.map { $0.y + $0.h }.max()!
        let pad = 28.0, W = bounds.width - 2 * pad, H = bounds.height - 2 * pad
        // Leave room around the group so a block can be dragged next to any side.
        scale = max(0.001, min(W / ((x1 - x0) * 1.5), H / ((y1 - y0) * 1.5), 0.25))
        ox = pad + (W - (x1 - x0) * scale) / 2 - x0 * scale
        oy = pad + (H - (y1 - y0) * scale) / 2 - y0 * scale
    }

    /// Where device `i` is drawn, ignoring any drag in progress.
    private func restingRect(_ i: Int) -> CGRect {
        let d = AppState.shared.devices[i]
        return CGRect(x: ox + d.x * scale, y: oy + d.y * scale, width: d.w * scale, height: d.h * scale)
    }

    private func rect(_ i: Int) -> CGRect {
        var r = restingRect(i)
        if i == dragIndex { r.origin = dragOrigin }
        return r
    }

    override func draw(_ dirty: NSRect) {
        Theme.screen.withAlphaComponent(0.6).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 8, yRadius: 8).fill()
        NSColor(white: 0.86, alpha: 1).setFill()
        for x in stride(from: 8.0, to: bounds.width, by: 16) { for y in stride(from: 8.0, to: bounds.height, by: 16) { NSRect(x: x, y: y, width: 1, height: 1).fill() } }
        let s = AppState.shared
        let para = NSMutableParagraphStyle()
        para.alignment = .center
        para.lineBreakMode = .byTruncatingTail
        if s.devices.isEmpty {
            let t = s.hubConnected ? "暂无设备" : "连接后显示屏幕布局"
            let size = (t as NSString).size(withAttributes: [.font: Theme.font(12)])
            (t as NSString).draw(at: NSPoint(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2),
                                 withAttributes: [.foregroundColor: Theme.muted, .font: Theme.font(12)])
            return
        }
        if dragIndex == nil { compute() }
        // Draw the dragged block last so it stays on top.
        let order = s.devices.indices.sorted { ($0 == dragIndex ? 1 : 0) < ($1 == dragIndex ? 1 : 0) }
        for i in order {
            let d = s.devices[i]
            let r = rect(i)
            let me = d.id == s.deviceID
            let role = !d.enabled ? "已关闭" : d.id == s.activeTarget ? "受控中" : d.id == s.activeSource ? "控制中" : (d.online && !d.sharing ? "已暂停" : (d.online ? "" : "离线"))
            let active = d.id == s.activeSource || d.id == s.activeTarget
            let path = NSBezierPath(roundedRect: r, xRadius: 6, yRadius: 6)
            if i == dragIndex {
                NSGraphicsContext.saveGraphicsState()
                let sh = NSShadow()
                sh.shadowColor = NSColor.black.withAlphaComponent(0.18)
                sh.shadowBlurRadius = 10
                sh.shadowOffset = NSSize(width: 0, height: -3)
                sh.set()
                Theme.panel.setFill(); path.fill()
                NSGraphicsContext.restoreGraphicsState()
            }
            (me ? Theme.selfScreen : Theme.screen).setFill()
            path.fill()
            (active ? Theme.text : d.online ? Theme.muted : NSColor(white: 0.74, alpha: 1)).setStroke()
            path.lineWidth = active ? 2.5 : 1.5
            if !d.online { path.setLineDash([4, 3], count: 2, phase: 0) }
            path.stroke()

            let compact = r.width < 120 || r.height < 64
            let plat = ["macos": "macOS", "windows": "Windows", "linux": "Linux"][d.platform] ?? d.platform
            let name = d.name + (me && !compact ? "（本机）" : "")
            let titleColor = d.online ? Theme.text : Theme.muted
            let text = NSMutableAttributedString(string: name + "\n", attributes: [.font: Theme.font(compact ? 11 : 13, .semibold), .foregroundColor: titleColor, .paragraphStyle: para])
            text.append(NSAttributedString(string: compact ? plat : "\(plat) · \(Int(d.w))×\(Int(d.h))", attributes: [.font: Theme.font(compact ? 9 : 11), .foregroundColor: Theme.muted, .paragraphStyle: para]))
            let inner = r.insetBy(dx: 4, dy: 2)
            let th = min(inner.height, text.boundingRect(with: CGSize(width: inner.width, height: inner.height), options: [.usesLineFragmentOrigin]).height)
            text.draw(with: CGRect(x: inner.minX, y: r.midY - th / 2, width: inner.width, height: th), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
            if !role.isEmpty {
                let a: [NSAttributedString.Key: Any] = [.font: Theme.font(compact ? 9 : 10), .foregroundColor: active ? Theme.text : Theme.muted]
                let w = (role as NSString).size(withAttributes: a).width
                (role as NSString).draw(at: NSPoint(x: r.maxX - w - 6, y: r.minY + 4), withAttributes: a)
            }
        }
    }

    override func mouseDown(with e: NSEvent) {
        guard AppState.shared.operational else { return }
        guard AppState.shared.hubConnected else { return }
        let p = convert(e.locationInWindow, from: nil)
        for i in AppState.shared.devices.indices.reversed() {
            let r = restingRect(i)
            guard r.contains(p) else { continue }
            // Compute the offset from the resting rectangle BEFORE marking the drag:
            // rect(i) would otherwise return the (stale) drag origin.
            dragOrigin = r.origin
            dragOffset = CGPoint(x: p.x - r.minX, y: p.y - r.minY)
            dragIndex = i
            moved = false
            NSCursor.closedHand.push()
            needsDisplay = true
            return
        }
    }

    override func mouseDragged(with e: NSEvent) {
        guard dragIndex != nil else { return }
        let p = convert(e.locationInWindow, from: nil)
        dragOrigin = CGPoint(x: p.x - dragOffset.x, y: p.y - dragOffset.y)
        moved = true
        needsDisplay = true
    }

    override func mouseUp(with e: NSEvent) {
        guard let i = dragIndex else { return }
        dragIndex = nil
        NSCursor.pop()
        if moved, i < AppState.shared.devices.count {
            let d = AppState.shared.devices[i]
            onMove?(d.id, (dragOrigin.x - ox) / scale, (dragOrigin.y - oy) / scale)
        }
        needsDisplay = true
    }

    override func resetCursorRects() { addCursorRect(bounds, cursor: .arrow) }
    override func layout() { super.layout(); if dragIndex == nil { needsDisplay = true } }
}
