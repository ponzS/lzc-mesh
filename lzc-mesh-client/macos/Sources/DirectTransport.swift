import Foundation

/// The Rust worker owns networking and routing. This adapter owns the ordered
/// main-thread handoff; adjacent positions are replaced without crossing actions.
private final class WeakDirectOwner {
    weak var owner: DirectTransport?
    init(_ owner: DirectTransport) { self.owner = owner }
}
private enum DirectCallbacks {
    static let lock = NSLock()
    static var next: UInt = 0
    static var owners: [UInt: WeakDirectOwner] = [:]
    static func insert(_ owner: DirectTransport) -> UInt {
        lock.lock(); defer { lock.unlock() }
        next += 1; owners[next] = WeakDirectOwner(owner); return next
    }
    static func owner(_ token: UInt) -> DirectTransport? {
        lock.lock(); defer { lock.unlock() }; return owners[token]?.owner
    }
    static func remove(_ token: UInt) {
        lock.lock(); defer { lock.unlock() }; owners.removeValue(forKey: token)
    }
    static let disposal = DispatchQueue(label: "cloud.lazycat.lzcmesh.transport-disposal")
}

final class DirectTransport: @unchecked Sendable {
    private lazy var bluetooth = BluetoothTransport(transport: self)
    private weak var hub: Hub?
    private var handle: OpaquePointer?
    private let lock = NSLock()
    private var queue: [(message: [String: Any], key: String?, generation: UInt)] = []
    private var generation: UInt = 0
    private var scheduled = false
    private var accepting = false

    init(hub: Hub) {
        self.hub = hub
    }
    deinit {
        DirectCallbacks.remove(generation)
        if let handle {
            // A callback can temporarily retain its owner on a runtime thread.
            // Dispose the foreign runtime elsewhere rather than blocking it.
            let address = UInt(bitPattern: handle)
            DirectCallbacks.disposal.async { mesh_direct_free(OpaquePointer(bitPattern: address)) }
        }
    }

    func input(_ object: [String: Any]) -> Bool {
        guard let handle, let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else { return false }
        return text.withCString { mesh_direct_input(handle, $0) }
    }
    func received(_ object: [String: Any]) {
        if object["t"] as? String == "state", let devices = object["devices"] as? [[String: Any]] {
            let own = devices.first { $0["id"] as? String == AppState.shared.deviceID }
            setActive(AppState.shared.authVerified && own?["trusted"] as? Bool == true && own?["enabled"] as? Bool != false)
        }
        guard let handle, let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else { return }
        text.withCString { mesh_direct_hub(handle, $0) }
    }
    func disconnected() { setActive(false) }

    private func setActive(_ on: Bool) {
        if on {
            guard handle == nil else { return }
            let token = DirectCallbacks.insert(self)
            lock.lock(); accepting = true; generation = token; lock.unlock()
            handle = AppState.shared.deviceID.withCString {
                mesh_direct_new($0, directTransportCallback, UnsafeMutableRawPointer(bitPattern: token))
            }
            if let handle { "{\"t\":\"hub_connected\"}".withCString { mesh_direct_hub(handle, $0) } }
            else {
                lock.lock(); accepting = false; generation = 0; lock.unlock()
                DirectCallbacks.remove(token)
            }
        } else {
            lock.lock(); accepting = false; let token = generation; generation = 0; queue.removeAll(); scheduled = false; lock.unlock()
            DirectCallbacks.remove(token)
            if let handle { mesh_direct_disconnect(handle); mesh_direct_free(handle) }
            handle = nil
            bluetooth.event(["_lan": "bt_stop"])
        }
    }

    fileprivate func enqueue(_ text: String, generation token: UInt) {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
        var key: String?
        let kind = object["_lan"] as? String
        if let message = object["message"] as? [String: Any] {
            if kind == "inject", message["t"] as? String == "move" { key = "move" }
            if kind == "hub", message["t"] as? String == "direct_relay",
               let wire = message["data"] as? [String: Any], wire["kind"] as? String == "input",
               let input = wire["input"] as? [String: Any], input["event"] == nil {
                key = "relay:\(input["session"] ?? ""):\(input["target"] ?? ""):\(input["barrier"] ?? 0)"
            }
        }
        lock.lock()
        guard accepting && generation == token else { lock.unlock(); return }
        if let key, queue.last?.key == key, queue.last?.generation == token { queue.removeLast() }
        queue.append((object, key, token))
        let start = !scheduled
        scheduled = true
        lock.unlock()
        if start { DispatchQueue.main.async { [weak self] in self?.drain() } }
    }
    private func drain() {
        lock.lock()
        let batch = queue
        queue.removeAll(keepingCapacity: true)
        scheduled = false
        lock.unlock()
        for item in batch {
            lock.lock(); let valid = accepting && generation == item.generation; lock.unlock()
            if !valid { continue }
            let object = item.message
            if (object["_lan"] as? String)?.hasPrefix("bt_") == true {
                let kind = object["_lan"] as? String ?? ""
                if AppState.shared.operational || kind == "bt_stop" || kind == "bt_close" { bluetooth.event(object) }
                continue
            }
            switch object["_lan"] as? String {
            case "status": AppState.shared.set(.transport, .ok, object["text"] as? String ?? "")
            case "metrics": AppState.shared.log(object["text"] as? String ?? "")
            case "channel_status":
                AppState.shared.set(object["channel"] as? String == "pointer" ? .pointerChannel : .keyboardChannel, .ok, object["text"] as? String ?? "")
            case "hub": if let message = object["message"] as? [String: Any] { hub?.sendRaw(message) }
            case "inject":
                if let message = object["message"] as? [String: Any] {
                    hub?.handle(message)
                    if let handle, let trace = message["_motion_trace"] as? [String: Any],
                       let data = try? JSONSerialization.data(withJSONObject: trace),
                       let text = String(data: data, encoding: .utf8) {
                        text.withCString { mesh_direct_applied(handle, $0) }
                    }
                }
            default: break
            }
        }
    }
}

private func directTransportCallback(_ context: UnsafeMutableRawPointer?, _ text: UnsafePointer<CChar>?) {
    guard let context, let text else { return }
    let token = UInt(bitPattern: context)
    DirectCallbacks.owner(token)?.enqueue(String(cString: text), generation: token)
}
