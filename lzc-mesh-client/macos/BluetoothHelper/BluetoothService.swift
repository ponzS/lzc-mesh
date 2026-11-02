import Foundation
import IOBluetooth

/// Main-run-loop RFCOMM adapter. TLS, device identity and input grants live in Rust.
final class BluetoothService: NSObject, IOBluetoothRFCOMMChannelDelegate {
    private let emit: ([String: Any]) -> Void
    private var record: IOBluetoothSDPServiceRecord?
    private var notification: IOBluetoothUserNotification?
    private var channels: [String: IOBluetoothRFCOMMChannel] = [:]
    private var frames: [String: (Data, Int)] = [:]
    private var running = false

    init(emit: @escaping ([String: Any]) -> Void) { self.emit = emit; super.init() }
    private func report(_ object: [String: Any]) { emit(object) }
    private func connectionID(_ channel: IOBluetoothRFCOMMChannel) -> String? {
        channels.first { $0.value === channel }?.key
    }
    func event(_ object: [String: Any]) {
        switch object["_lan"] as? String {
        case "bt_start": start()
        case "bt_stop": stop()
        case "bt_close":
            if let id = object["connection"] as? String { close(id) }
        case "bt_send":
            guard running, let id = object["connection"] as? String, channels[id] != nil,
                  let encoded = object["data"] as? String, encoded.count <= 90000,
                  let bytes = Data(base64Encoded: encoded), frames[id] == nil else { return }
            frames[id] = (bytes, 0)
            writeNext(id)
        default: break
        }
    }
    func start() {
        guard !running else { return }
        running = true
        // RFCOMM is a classic Bluetooth service. Initialize the service before
        // querying its host controller, matching IOBluetooth's service lifecycle.
        // No BLE scan or CBCentralManager is needed for this transport.
        publish()
    }
    private func publish() {
        var bytes = UUID(uuidString: "F4174C12-5A3B-4429-B265-06EEDCEFE21A")!.uuid
        let uuid = withUnsafeBytes(of: &bytes) { IOBluetoothSDPUUID(bytes: $0.baseAddress, length: 16) }
        record = IOBluetoothSDPServiceRecord.publishedServiceRecord(with: [
            "0001 - ServiceClassIDList": [uuid],
            "0004 - ProtocolDescriptorList": [[IOBluetoothSDPUUID(uuid16: 0x0100)!], [IOBluetoothSDPUUID(uuid16: 0x0003)!, NSNumber(value: 13)]],
            "0100 - ServiceName": "LZC Mesh",
            "LocalAttributes": ["Persistent": false],
        ])
        var channel: BluetoothRFCOMMChannelID = 0
        guard let record, record.getRFCOMMChannelID(&channel) == kIOReturnSuccess, channel > 0 else {
            report(["t": "bt_local", "error": "无法发布蓝牙服务，请检查蓝牙权限"]); return
        }
        notification = IOBluetoothRFCOMMChannel.register(forChannelOpenNotifications: self, selector: #selector(opened(_:channel:)), withChannelID: channel, direction: kIOBluetoothUserNotificationChannelDirectionIncoming)
        guard notification != nil else {
            stop(); report(["t": "bt_local", "error": "无法监听蓝牙服务"]); return
        }
        guard let address = IOBluetoothHostController.default().addressAsString() else {
            stop(); report(["t": "bt_local", "error": "无法读取蓝牙适配器地址"]); return
        }
        report(["t": "bt_local", "bluetooth": ["role": "listener", "address": address.replacingOccurrences(of: "-", with: ":").uppercased(), "channel": Int(channel)]])
        report(["t": "bt_log", "text": "bluetooth_service ready channel=\(channel)"])
    }
    func stop() {
        running = false
        notification?.unregister(); notification = nil
        record?.remove(); record = nil
        for id in Array(channels.keys) { close(id) }
    }
    private func close(_ id: String) {
        let channel = channels.removeValue(forKey: id)
        frames.removeValue(forKey: id)
        channel?.close()
    }
    @objc func opened(_ note: IOBluetoothUserNotification, channel: IOBluetoothRFCOMMChannel) {
        guard running, channels.count < 8 else { channel.close(); return }
        let id = UUID().uuidString
        channels[id] = channel
        channel.setDelegate(self)
        report(["t": "bt_open", "connection": id])
    }
    func rfcommChannelData(_ channel: IOBluetoothRFCOMMChannel!, data pointer: UnsafeMutableRawPointer!, length: Int) {
        guard let channel, let id = connectionID(channel), length > 0, length <= 65536 else { return }
        let bytes = Data(bytes: pointer, count: length)
        report(["t": "bt_data", "connection": id, "data": bytes.base64EncodedString()])
    }
    func rfcommChannelClosed(_ channel: IOBluetoothRFCOMMChannel!) {
        guard let channel, let id = connectionID(channel) else { return }
        channels.removeValue(forKey: id); frames.removeValue(forKey: id)
        report(["t": "bt_closed", "connection": id, "error": "蓝牙连接已断开"])
    }
    private final class WriteBuffer {
        let pointer: UnsafeMutableRawPointer
        init(_ bytes: Data) {
            pointer = .allocate(byteCount: bytes.count, alignment: 1)
            bytes.copyBytes(to: pointer.assumingMemoryBound(to: UInt8.self), count: bytes.count)
        }
        deinit { pointer.deallocate() }
    }
    private func writeNext(_ id: String) {
        guard let channel = channels[id], let (data, offset) = frames[id] else { return }
        if offset == data.count {
            frames.removeValue(forKey: id)
            report(["t": "bt_written", "connection": id]); return
        }
        let length = min(Int(channel.getMTU()), data.count - offset)
        guard length > 0 else { close(id); report(["t": "bt_closed", "connection": id, "error": "蓝牙 MTU 不可用"]); return }
        let buffer = WriteBuffer(data.subdata(in: offset..<(offset + length)))
        let token = Unmanaged.passRetained(buffer).toOpaque()
        frames[id] = (data, offset + length)
        let result = channel.writeAsync(buffer.pointer, length: UInt16(length), refcon: token)
        if result != kIOReturnSuccess {
            Unmanaged<WriteBuffer>.fromOpaque(token).release()
            close(id); report(["t": "bt_closed", "connection": id, "error": "蓝牙发送失败 \(result)"])
        }
    }
    func rfcommChannelWriteComplete(_ channel: IOBluetoothRFCOMMChannel!, refcon: UnsafeMutableRawPointer!, status: IOReturn) {
        if let refcon { Unmanaged<WriteBuffer>.fromOpaque(refcon).release() }
        guard let channel, let id = connectionID(channel) else { return }
        if status == kIOReturnSuccess { writeNext(id) }
        else { close(id); report(["t": "bt_closed", "connection": id, "error": "蓝牙发送失败 \(status)"]) }
    }
}
