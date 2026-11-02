import Foundation
import Darwin

/// Launch Bluetooth as its own signed application so macOS can attribute its
/// hardware permission correctly. Private FIFOs carry encrypted TLS bytes only.
final class BluetoothTransport {
    private final class Session: @unchecked Sendable {
        let directory: URL
        let input: Int32, output: Int32
        private let lock = NSLock()
        private var enabled = true
        var active: Bool { lock.lock(); defer { lock.unlock() }; return enabled }
        init() throws {
            directory = FileManager.default.temporaryDirectory.appendingPathComponent("lzc-mesh-bt-" + UUID().uuidString)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
            let incoming = directory.appendingPathComponent("commands").path
            let outgoing = directory.appendingPathComponent("events").path
            guard mkfifo(incoming, 0o600) == 0, mkfifo(outgoing, 0o600) == 0 else {
                try? FileManager.default.removeItem(at: directory)
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
            }
            input = Darwin.open(incoming, O_RDWR | O_NONBLOCK | O_CLOEXEC)
            output = Darwin.open(outgoing, O_RDWR | O_NONBLOCK | O_CLOEXEC)
            guard input >= 0, output >= 0 else {
                if input >= 0 { Darwin.close(input) }; if output >= 0 { Darwin.close(output) }
                try? FileManager.default.removeItem(at: directory)
                throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno))
            }
        }
        func stop() {
            lock.lock()
            guard enabled else { lock.unlock(); return }
            let stop = Data("{\"_lan\":\"bt_stop\"}\n".utf8)
            _ = stop.withUnsafeBytes { Darwin.write(input, $0.baseAddress, $0.count) }
            enabled = false
            lock.unlock()
            // The helper also observes disappearance if the pipe was full.
            try? FileManager.default.removeItem(at: directory)
        }
        deinit { Darwin.close(input); Darwin.close(output); try? FileManager.default.removeItem(at: directory) }
        func write(_ data: Data) {
            data.withUnsafeBytes { bytes in
                var offset = 0
                while active && offset < bytes.count {
                    let count = Darwin.write(input, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                    if count > 0 { offset += count }
                    else if errno == EAGAIN || errno == EINTR {
                        var descriptor = pollfd(fd: input, events: Int16(POLLOUT), revents: 0)
                        _ = Darwin.poll(&descriptor, 1, 100)
                    } else { break }
                }
            }
        }
    }
    private weak var transport: DirectTransport?
    private var session: Session?
    private let writer = DispatchQueue(label: "cloud.lazycat.lzcmesh.bluetooth.writer")
    private var ready = false
    init(transport: DirectTransport) { self.transport = transport }
    deinit { session?.stop() }
    func event(_ object: [String: Any]) {
        switch object["_lan"] as? String {
        case "bt_start": start()
        case "bt_stop": stop()
        default:
            guard let session, let data = try? JSONSerialization.data(withJSONObject: object) else { return }
            writer.async { session.write(data + Data([10])) }
        }
    }
    private func stop() { session?.stop(); session = nil; ready = false }
    private func start() {
        stop()
        let current: Session
        do { current = try Session() }
        catch { transport?.received(["t": "bt_local", "error": "无法创建私有蓝牙通道"]); return }
        session = current
        let helper = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/LZC Mesh Bluetooth.app")
        let launcher = Process()
        launcher.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        launcher.arguments = ["-n", helper.path, "--args", "--pipes", current.directory.path, String(getpid())]
        launcher.standardOutput = FileHandle.nullDevice; launcher.standardError = FileHandle.nullDevice
        do { try launcher.run() }
        catch { stop(); transport?.received(["t": "bt_local", "error": "无法启动蓝牙辅助应用"]); return }
        DispatchQueue.global(qos: .userInteractive).async { [weak self] in
            var buffer = Data(), bytes = [UInt8](repeating: 0, count: 16384)
            while current.active {
                var descriptor = pollfd(fd: current.output, events: Int16(POLLIN), revents: 0)
                guard Darwin.poll(&descriptor, 1, 200) > 0 else { continue }
                let count = bytes.withUnsafeMutableBytes { Darwin.read(current.output, $0.baseAddress, $0.count) }
                guard count > 0 else { continue }
                buffer.append(contentsOf: bytes.prefix(count))
                if buffer.count > 131072 { current.stop(); break }
                while let end = buffer.firstIndex(of: 10) {
                    let line = Data(buffer[..<end]); buffer.removeSubrange(...end)
                    guard let value = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
                    DispatchQueue.main.async { [weak self] in
                        guard let self, self.session === current else { return }
                        if value["t"] as? String == "bt_log" { AppState.shared.log(value["text"] as? String ?? "") }
                        else {
                            if value["t"] as? String == "bt_local" { self.ready = true }
                            self.transport?.received(value)
                        }
                    }
                }
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 60) { [weak self] in
            guard let self, self.session === current, !self.ready else { return }
            self.stop()
            self.transport?.received(["t": "bt_local", "error": "蓝牙初始化超时，请检查系统蓝牙权限"])
        }
    }
}
