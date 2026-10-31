// Black / white / grey palette shared by the macOS views, matching the web page.
import AppKit

enum Theme {
    static let bg = NSColor(white: 0.961, alpha: 1)        // #f5f5f5
    static let panel = NSColor.white
    static let line = NSColor(white: 0.886, alpha: 1)      // #e2e2e2
    static let text = NSColor(white: 0.067, alpha: 1)      // #111
    static let muted = NSColor(white: 0.467, alpha: 1)     // #777
    static let faint = NSColor(white: 0.70, alpha: 1)
    static let screen = NSColor(white: 0.980, alpha: 1)
    static let selfScreen = NSColor(white: 0.93, alpha: 1)

    static func font(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont { .systemFont(ofSize: size, weight: weight) }
    static func mono(_ size: CGFloat) -> NSFont { .monospacedSystemFont(ofSize: size, weight: .regular) }
}

extension NSColor {
    /// Resolves to a plain sRGB colour for CALayer use.
    var cg: CGColor { usingColorSpace(.sRGB)?.cgColor ?? cgColor }
}

/// Flat bordered surface used for every panel.
final class Panel: NSView {
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Theme.panel.cg
        layer?.cornerRadius = 10
        layer?.borderWidth = 1
        layer?.borderColor = Theme.line.cg
    }
    required init?(coder: NSCoder) { fatalError() }
}

/// Black / white pill switch (the system switch uses the accent colour).
final class ToggleSwitch: NSControl {
    var isOn = false { didSet { needsDisplay = true } }
    override var intrinsicContentSize: NSSize { NSSize(width: 34, height: 20) }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var isEnabled: Bool { didSet { needsDisplay = true } }

    override func draw(_ dirty: NSRect) {
        let r = bounds.insetBy(dx: 1, dy: 1)
        let track = NSBezierPath(roundedRect: r, xRadius: r.height / 2, yRadius: r.height / 2)
        (isOn ? Theme.text : NSColor(white: 0.80, alpha: 1)).withAlphaComponent(isEnabled ? 1 : 0.45).setFill()
        track.fill()
        let d = r.height - 4
        let x = isOn ? r.maxX - d - 2 : r.minX + 2
        NSColor.white.setFill()
        NSBezierPath(ovalIn: NSRect(x: x, y: r.minY + 2, width: d, height: d)).fill()
    }

    override func mouseDown(with event: NSEvent) {
        guard isEnabled else { return }
        isOn.toggle()
        sendAction(action, to: target)
    }
    override func keyDown(with event: NSEvent) {
        if event.characters == " " && isEnabled { isOn.toggle(); sendAction(action, to: target) } else { super.keyDown(with: event) }
    }
}

/// Small status dot: filled = ok, grey = warning, ring = error, light = unknown.
final class StatusDot: NSView {
    var level: Level = .unknown { didSet { needsDisplay = true } }
    override var intrinsicContentSize: NSSize { NSSize(width: 8, height: 8) }
    override func draw(_ dirty: NSRect) {
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        let p = NSBezierPath(ovalIn: r)
        switch level {
        case .ok: Theme.text.setFill(); p.fill()
        case .warn: NSColor(white: 0.53, alpha: 1).setFill(); p.fill()
        case .error: NSColor.white.setFill(); p.fill(); Theme.text.setStroke(); p.lineWidth = 1.6; p.stroke()
        case .unknown: NSColor(white: 0.80, alpha: 1).setFill(); p.fill()
        }
    }
}

/// Grey caption / body labels.
func makeLabel(_ s: String, size: CGFloat = 12, weight: NSFont.Weight = .regular, color: NSColor = Theme.text, lines: Int = 1) -> NSTextField {
    let l = NSTextField(labelWithString: s)
    l.font = Theme.font(size, weight)
    l.textColor = color
    l.lineBreakMode = lines == 1 ? .byTruncatingTail : .byWordWrapping
    l.maximumNumberOfLines = lines
    l.cell?.truncatesLastVisibleLine = true
    return l
}

/// Flat rounded button: filled black (primary) or outlined (secondary).
final class FlatButton: NSButton {
    private let primary: Bool
    private var hovering = false
    init(title: String, primary: Bool, target: AnyObject?, action: Selector?) {
        self.primary = primary
        super.init(frame: .zero)
        self.title = title
        self.target = target
        self.action = action
        isBordered = false
        setButtonType(.momentaryPushIn)
        font = Theme.font(12)
        wantsLayer = true
        layer?.cornerRadius = 6
        layer?.borderWidth = 1
        updateStyle()
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var intrinsicContentSize: NSSize {
        let w = (title as NSString).size(withAttributes: [.font: Theme.font(12)]).width
        return NSSize(width: ceil(w) + 22, height: 26)
    }
    override var title: String { didSet { updateStyle() } }
    override var isEnabled: Bool { didSet { updateStyle() } }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func mouseEntered(with event: NSEvent) { hovering = true; updateStyle() }
    override func mouseExited(with event: NSEvent) { hovering = false; updateStyle() }

    func updateStyle() {
        let on = isEnabled
        let fg: NSColor = primary ? .white : Theme.text
        attributedTitle = NSAttributedString(string: title, attributes: [.font: Theme.font(12), .foregroundColor: fg.withAlphaComponent(on ? 1 : 0.4)])
        if primary {
            layer?.backgroundColor = (hovering ? NSColor(white: 0.25, alpha: 1) : Theme.text).cg
            layer?.borderColor = Theme.text.cg
        } else {
            layer?.backgroundColor = (hovering ? NSColor(white: 0.95, alpha: 1) : NSColor.white).cg
            layer?.borderColor = (hovering ? NSColor(white: 0.62, alpha: 1) : Theme.line).cg
        }
        layer?.opacity = on ? 1 : 0.6
    }
}
