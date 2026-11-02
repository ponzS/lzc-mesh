// macOS input: a CGEventTap for edge detection and for capturing local input
// while this Mac controls another device, and CGEvent posting while it is
// controlled. Same technique as lightos-remote-desktop's macOS host
// (CGEvent injection gated by Accessibility / Input Monitoring consent).
import AppKit
import ApplicationServices

// Hiding the cursor while the app is in the background needs this private
// connection property (Synergy/Barrier use the same one).
@_silgen_name("_CGSDefaultConnection") private func _CGSDefaultConnection() -> Int32
@_silgen_name("CGSSetConnectionProperty") private func CGSSetConnectionProperty(_ c: Int32, _ t: Int32, _ k: CFString, _ v: CFTypeRef) -> Int32

/// Marks events we post ourselves so the tap never forwards them back.
private let injectedMarker: Int64 = 0x4C5A_4D45

private func tapCallback(proxy: CGEventTapProxy, type: CGEventType, event: CGEvent, refcon: UnsafeMutableRawPointer?) -> Unmanaged<CGEvent>? {
    let me = Unmanaged<MacInput>.fromOpaque(refcon!).takeUnretainedValue()
    return me.handle(type: type, event: event)
}

final class MacInput {
    weak var hub: Hub?
    private var tap: CFMachPort?
    private var edges = [false, false, false, false]  // left right up down
    /// Mirrors the tray switch: when off, no edge pushes are reported.
    var sharingEnabled = true
    private var quietUntil = Date.distantPast
    private var lastEdge = Date.distantPast
    private var flags: UInt64 = 0           // modifier state of injected keys
    private var heldKeys = Set<UInt16>()    // injected keys still down
    private var heldButtons = Set<Int>()
    private var cursorHidden = false
    private var clickState = (time: Date.distantPast, button: 0, count: 0)
    private var source = CGEventSource(stateID: .hidSystemState)

    // MARK: permissions

    struct Permissions { var accessibility: Bool; var listen: Bool; var post: Bool
        var ok: Bool { accessibility && listen && post } }

    func permissions() -> Permissions {
        Permissions(accessibility: AXIsProcessTrusted(), listen: CGPreflightListenEventAccess(), post: CGPreflightPostEventAccess())
    }

    /// Asks macOS for every missing permission. The system shows its own consent
    /// prompt (or adds the app to System Settings) for each call.
    func requestPermissions() {
        let p = permissions()
        if !p.accessibility {
            let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
            _ = AXIsProcessTrustedWithOptions(opts)
        }
        if !p.listen { _ = CGRequestListenEventAccess() }
        if !p.post { _ = CGRequestPostEventAccess() }
        AppState.shared.log("已向系统请求权限：\(p.accessibility ? "" : "辅助功能 ")\(p.listen ? "" : "输入监控 ")\(p.post ? "" : "事件发送")")
    }

    static func openSettings(_ pane: String) {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)")!)
    }

    func refreshPermissionStatus() {
        let p = permissions()
        let s = AppState.shared
        if p.ok {
            if tap == nil { installTap() }
            if tap != nil { s.set(.permission, .ok, "辅助功能、输入监控已授权") }
        } else {
            var missing: [String] = []
            if !p.accessibility { missing.append("辅助功能") }
            if !p.listen { missing.append("输入监控") }
            if !p.post && p.accessibility { missing.append("事件发送") }
            s.set(.permission, .error, "缺少权限：\(missing.joined(separator: "、"))。请在弹出的系统对话框或“系统设置 › 隐私与安全性”中允许“懒猫设备接力”")
        }
    }

    // MARK: event tap

    private func installTap() {
        let types: [CGEventType] = [.mouseMoved, .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp, .otherMouseDown, .otherMouseUp,
                                    .leftMouseDragged, .rightMouseDragged, .otherMouseDragged, .scrollWheel, .keyDown, .keyUp, .flagsChanged]
        let mask = types.reduce(CGEventMask(0)) { $0 | (1 << $1.rawValue) }
        guard let t = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .defaultTap, eventsOfInterest: mask,
                                        callback: tapCallback, userInfo: Unmanaged.passUnretained(self).toOpaque()) else {
            AppState.shared.set(.permission, .error, "无法创建事件监听（CGEventTap），请确认已授予辅助功能和输入监控权限后重启懒猫设备接力")
            return
        }
        tap = t
        let src = CFMachPortCreateRunLoopSource(nil, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        AppState.shared.log("事件监听已启动")
    }

    fileprivate func handle(type: CGEventType, event: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let t = tap { CGEvent.tapEnable(tap: t, enable: true) }
            AppState.shared.log("事件监听被系统暂停，已恢复")
            return Unmanaged.passUnretained(event)
        }
        if event.getIntegerValueField(.eventSourceUserData) == injectedMarker { return Unmanaged.passUnretained(event) }
        let s = AppState.shared
        if s.captured {
            forward(type: type, event: event)
            return nil  // swallow: this Mac's input belongs to the remote device now
        }
        if type == .mouseMoved || type == .leftMouseDragged { checkEdge(event) }
        return Unmanaged.passUnretained(event)
    }

    // MARK: edges

    /// Desktop bounds in global display coordinates (top-left origin, points).
    static func desktopBounds() -> CGRect {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var n: UInt32 = 0
        CGGetActiveDisplayList(16, &ids, &n)
        return ids.prefix(Int(n)).map { CGDisplayBounds($0) }.reduce(CGRect.null) { $0.union($1) }
    }

    private func checkEdge(_ event: CGEvent) {
        let s = AppState.shared
        guard sharingEnabled, s.hubConnected, !s.controlled, Date() > quietUntil, Date().timeIntervalSince(lastEdge) > 0.25 else { return }
        let b = MacInput.desktopBounds()
        let p = event.location
        let dx = event.getDoubleValueField(.mouseEventDeltaX), dy = event.getDoubleValueField(.mouseEventDeltaY)
        // The system clamps the cursor to the desktop; a push is "at the border and still moving outwards".
        var dir: String?
        if edges[0] && p.x <= b.minX + 0.5 && dx < 0 { dir = "left" }
        else if edges[1] && p.x >= b.maxX - 1.5 && dx > 0 { dir = "right" }
        else if edges[2] && p.y <= b.minY + 0.5 && dy < 0 { dir = "up" }
        else if edges[3] && p.y >= b.maxY - 1.5 && dy > 0 { dir = "down" }
        guard let d = dir else { return }
        lastEdge = Date()
        let pos = (d == "left" || d == "right") ? (p.y - b.minY) / b.height : (p.x - b.minX) / b.width
        hub?.send(["t": "edge", "dir": d, "pos": min(max(pos, 0), 1)])
    }

    func setEdges(left: Bool, right: Bool, up: Bool, down: Bool) { edges = [left, right, up, down] }

    // MARK: capture (this Mac controls another device)

    private func setCursorHidden(_ hide: Bool) {
        guard hide != cursorHidden else { return }
        cursorHidden = hide
        _ = CGSSetConnectionProperty(_CGSDefaultConnection(), _CGSDefaultConnection(), "SetsCursorInBackground" as CFString, kCFBooleanTrue)
        if hide { CGDisplayHideCursor(CGMainDisplayID()) } else { CGDisplayShowCursor(CGMainDisplayID()) }
    }

    // Read deltas directly. Repeated cursor warps can suppress hardware mouse
    // events on macOS; neither the initial capture nor drift recovery needs a
    // warp when absolute cursor coordinates are not used for movement.
    func capture(_ on: Bool) {
        if on {
            source?.localEventsSuppressionInterval = 0
            let result = CGAssociateMouseAndMouseCursorPosition(0)
            setCursorHidden(true)
            AppState.shared.log("pointer_capture mode=relative cursor_warps=0 associate_result=\(result.rawValue)")
        } else {
            CGAssociateMouseAndMouseCursorPosition(1)
            setCursorHidden(false)
            quietUntil = Date().addingTimeInterval(0.4)
        }
    }

    func warp(_ nx: Double, _ ny: Double) {
        let b = MacInput.desktopBounds()
        quietUntil = Date().addingTimeInterval(0.4)
        CGWarpMouseCursorPosition(CGPoint(x: b.minX + nx * (b.width - 1), y: b.minY + ny * (b.height - 1)))
        CGAssociateMouseAndMouseCursorPosition(1)
    }

    private var lastFlags = CGEventFlags()

    private func forward(type: CGEventType, event: CGEvent) {
        guard let hub = hub else { return }
        switch type {
        case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            let captured = mesh_direct_now_us()
            let dx = event.getDoubleValueField(.mouseEventDeltaX), dy = event.getDoubleValueField(.mouseEventDeltaY)
            if dx != 0 || dy != 0 { hub.send(["t": "motion", "dx": dx, "dy": dy, "_capture_us": captured]) }
        case .leftMouseDown, .leftMouseUp: hub.send(["t": "button", "button": 1, "down": type == .leftMouseDown])
        case .rightMouseDown, .rightMouseUp: hub.send(["t": "button", "button": 2, "down": type == .rightMouseDown])
        case .otherMouseDown, .otherMouseUp:
            let n = event.getIntegerValueField(.mouseEventButtonNumber)
            let b = n == 2 ? 3 : n == 3 ? 4 : n == 4 ? 5 : 0
            if b > 0 { hub.send(["t": "button", "button": b, "down": type == .otherMouseDown]) }
        case .scrollWheel:
            // macOS supplies both finger motion and the subsequent momentum. Keep
            // this one continuous stream; a receiver must not start a second fling.
            let continuous = event.getIntegerValueField(.scrollWheelEventIsContinuous) != 0
            let phase = event.getIntegerValueField(.scrollWheelEventScrollPhase)
            let momentum = event.getIntegerValueField(.scrollWheelEventMomentumPhase)
            let vy: Double, vx: Double
            if continuous {
                vy = event.getDoubleValueField(.scrollWheelEventPointDeltaAxis1) * 12
                vx = event.getDoubleValueField(.scrollWheelEventPointDeltaAxis2) * 12
            } else {
                vy = Double(event.getIntegerValueField(.scrollWheelEventDeltaAxis1)) * 120
                vx = Double(event.getIntegerValueField(.scrollWheelEventDeltaAxis2)) * 120
            }
            if vx != 0 || vy != 0 || phase != 0 || momentum != 0 {
                var message: [String: Any] = ["t": "wheel", "dx": vx, "dy": vy,
                                               "source": continuous ? "continuous" : "wheel"]
                if let name = Self.scrollPhase(phase) { message["phase"] = name }
                if let name = Self.momentumPhase(momentum) { message["momentum_phase"] = name }
                if phase & 12 != 0 || momentum == 3 {
                    message["stop_x"] = true
                    message["stop_y"] = true
                }
                hub.send(message)
            }
        case .keyDown, .keyUp:
            if event.getIntegerValueField(.keyboardEventAutorepeat) != 0 { return }
            let k = UInt16(event.getIntegerValueField(.keyboardEventKeycode))
            if let code = macToEvdev[k] { hub.send(["t": "key", "code": code, "down": type == .keyDown]) }
        case .flagsChanged:
            let k = UInt16(event.getIntegerValueField(.keyboardEventKeycode))
            guard let code = macToEvdev[k] else { return }
            let f = event.flags.rawValue
            let down: Bool
            if k == macCapsLock { down = true; hub.send(["t": "key", "code": code, "down": true]); hub.send(["t": "key", "code": code, "down": false]); return }
            else if let (bit, _) = macModifiers[k] { down = f & bit != 0 }
            else { down = event.flags.contains(.maskSecondaryFn) }
            hub.send(["t": "key", "code": code, "down": down])
        default: break
        }
    }

    // MARK: injection (another device controls this Mac)

    private var cursor = CGPoint.zero

    private func post(_ e: CGEvent?) {
        guard let e = e else { return }
        e.setIntegerValueField(.eventSourceUserData, value: injectedMarker)
        e.post(tap: .cghidEventTap)
    }

    func enter(_ nx: Double, _ ny: Double) {
        quietUntil = Date().addingTimeInterval(0.4)
        move(nx, ny)
    }

    func move(_ nx: Double, _ ny: Double) {
        let b = MacInput.desktopBounds()
        cursor = CGPoint(x: b.minX + nx * (b.width - 1), y: b.minY + ny * (b.height - 1))
        let type: CGEventType = heldButtons.contains(1) ? .leftMouseDragged : heldButtons.contains(2) ? .rightMouseDragged :
            heldButtons.isEmpty ? .mouseMoved : .otherMouseDragged
        let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: cursor, mouseButton: .left)
        post(e)
    }

    func button(_ b: Int, down: Bool) {
        let (type, btn): (CGEventType, CGMouseButton)
        switch b {
        case 1: (type, btn) = (down ? .leftMouseDown : .leftMouseUp, .left)
        case 2: (type, btn) = (down ? .rightMouseDown : .rightMouseUp, .right)
        case 3: (type, btn) = (down ? .otherMouseDown : .otherMouseUp, .center)
        case 4: (type, btn) = (down ? .otherMouseDown : .otherMouseUp, CGMouseButton(rawValue: 3)!)
        case 5: (type, btn) = (down ? .otherMouseDown : .otherMouseUp, CGMouseButton(rawValue: 4)!)
        default: return
        }
        if down {
            heldButtons.insert(b)
            let now = Date()
            if clickState.button == b && now.timeIntervalSince(clickState.time) < NSEvent.doubleClickInterval { clickState.count += 1 }
            else { clickState.count = 1 }
            clickState.time = now
            clickState.button = b
        } else {
            heldButtons.remove(b)
        }
        let e = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: cursor, mouseButton: btn)
        e?.setIntegerValueField(.mouseEventClickState, value: Int64(clickState.count))
        if b >= 3 { e?.setIntegerValueField(.mouseEventButtonNumber, value: Int64(btn.rawValue)) }
        e?.flags = CGEventFlags(rawValue: flags)
        post(e)
    }

    private static func scrollPhase(_ value: Int64) -> String? {
        switch value {
        case 1: return "began"
        case 2: return "changed"
        case 4: return "ended"
        case 8: return "cancelled"
        case 128: return "may_begin"
        default: return nil
        }
    }

    private static func momentumPhase(_ value: Int64) -> String? {
        switch value {
        case 1: return "began"
        case 2: return "changed"
        case 3: return "ended"
        default: return nil
        }
    }

    func wheel(_ dx: Double, _ dy: Double, scrollSource: String? = nil,
               phase: String? = nil, momentumPhase: String? = nil, stop: Bool = false) {
        // Old senders omit source; preserve their existing pixel-based behavior.
        let wheel = scrollSource == "wheel"
        let unit = wheel ? 120.0 : 12.0
        let e = CGEvent(scrollWheelEvent2Source: source, units: wheel ? .line : .pixel, wheelCount: 2,
                        wheel1: Int32(clamping: Int64((dy / unit).rounded())),
                        wheel2: Int32(clamping: Int64((dx / unit).rounded())), wheel3: 0)
        if !wheel {
            // The constructor takes integers. These fields retain sub-point motion.
            e?.setDoubleValueField(.scrollWheelEventFixedPtDeltaAxis1, value: dy / 12)
            e?.setDoubleValueField(.scrollWheelEventFixedPtDeltaAxis2, value: dx / 12)
            let phases: [String: Int64] = ["began": 1, "changed": 2, "ended": 4, "cancelled": 8, "may_begin": 128]
            let momentum: [String: Int64] = ["began": 1, "changed": 2, "ended": 3]
            e?.setIntegerValueField(.scrollWheelEventScrollPhase, value: phase.flatMap { phases[$0] } ?? (stop ? 4 : 0))
            e?.setIntegerValueField(.scrollWheelEventMomentumPhase, value: momentumPhase.flatMap { momentum[$0] } ?? 0)
        }
        post(e)
    }

    func key(_ code: UInt32, down: Bool) {
        guard let k = evdevToMac[code] else { return }
        if down { heldKeys.insert(k) } else if heldKeys.remove(k) == nil { return }
        if let (bit, generic) = macModifiers[k] {
            if down { flags |= bit | generic } else {
                flags &= ~bit
                // Clear the generic flag only if neither side is still held.
                let sameGroup = macModifiers.filter { $0.value.1 == generic && $0.key != k }.map { $0.key }
                if !sameGroup.contains(where: heldKeys.contains) { flags &= ~generic }
            }
            let e = CGEvent(keyboardEventSource: source, virtualKey: k, keyDown: down)
            e?.type = .flagsChanged
            e?.flags = CGEventFlags(rawValue: flags)
            post(e)
            return
        }
        let e = CGEvent(keyboardEventSource: source, virtualKey: k, keyDown: down)
        e?.flags = CGEventFlags(rawValue: flags)
        post(e)
        // The newest key repeats, like a real keyboard. Caps Lock and Fn never repeat.
        if down && k != macCapsLock && k != macFn { startRepeat(k) } else if !down && k == repeatKey { stopRepeat() }
    }

    // MARK: key repeat
    // macOS only auto-repeats keys of a physical keyboard, so a held remote key is
    // repeated here with the user's own "delay until repeat" / "key repeat rate".

    private var repeatTimer: Timer?
    private var repeatKey: UInt16?

    private func stopRepeat() {
        repeatTimer?.invalidate()
        repeatTimer = nil
        repeatKey = nil
    }

    private func schedule(_ interval: TimeInterval, repeats: Bool, _ body: @escaping () -> Void) -> Timer {
        let t = Timer(timeInterval: interval, repeats: repeats) { _ in body() }
        RunLoop.main.add(t, forMode: .common)  // keep firing while menus or window drags track
        return t
    }

    private func startRepeat(_ k: UInt16) {
        stopRepeat()
        repeatKey = k
        repeatTimer = schedule(NSEvent.keyRepeatDelay, repeats: false) { [weak self] in
            guard let self = self, self.repeatKey == k else { return }
            self.repeatOnce(k)
            self.repeatTimer = self.schedule(max(NSEvent.keyRepeatInterval, 0.015), repeats: true) { [weak self] in self?.repeatOnce(k) }
        }
    }

    private func repeatOnce(_ k: UInt16) {
        guard heldKeys.contains(k), repeatKey == k else { stopRepeat(); return }
        let e = CGEvent(keyboardEventSource: source, virtualKey: k, keyDown: true)
        e?.setIntegerValueField(.keyboardEventAutorepeat, value: 1)
        e?.flags = CGEventFlags(rawValue: flags)
        post(e)
    }

    /// Releases everything a remote controller still holds so nothing gets stuck.
    func leave() {
        stopRepeat()
        for k in heldKeys { key(macToEvdev[k] ?? 0, down: false) }
        heldKeys.removeAll()
        for b in heldButtons { button(b, down: false) }
        heldButtons.removeAll()
        flags = 0
        quietUntil = Date().addingTimeInterval(0.4)
    }
}
