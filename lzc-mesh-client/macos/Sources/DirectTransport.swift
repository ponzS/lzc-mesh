import Foundation

/// The Rust worker owns networking and routing. This adapter owns the ordered
/// main-thread handoff; adjacent positions are replaced without crossing actions.
final class DirectTransport: @unchecked Sendable {
    private lazy var bluetooth = BluetoothTransport(transport: self)
    private weak var hub: Hub?
    private var handle: OpaquePointer?
    private let lock = NSLock()
    private var queue: [(message: [String: Any], key: String?)] = []
    private var scheduled = false

    init(hub: Hub) {
        self.hub = hub
        handle = AppState.shared.deviceID.withCString {
            mesh_direct_new($0, directTransportCallback, Unmanaged.passUnretained(self).toOpaque())
        }
    }
    deinit { if let handle { mesh_direct_free(handle) } }

    func input(_ object: [String: Any]) -> Bool {
        guard let handle, let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else { return false }
        return text.withCString { mesh_direct_input(handle, $0) }
    }
    func received(_ object: [String: Any]) {
        guard let handle, let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else { return }
        text.withCString { mesh_direct_hub(handle, $0) }
    }
    func disconnected() { if let handle { mesh_direct_disconnect(handle) } }

    fileprivate func enqueue(_ text: String) {
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
        if let key, queue.last?.key == key { queue.removeLast() }
        queue.append((object, key))
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
            let object = item.message
            if (object["_lan"] as? String)?.hasPrefix("bt_") == true {
                bluetooth.event(object); continue
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
    Unmanaged<DirectTransport>.fromOpaque(context).takeUnretainedValue().enqueue(String(cString: text))
}
