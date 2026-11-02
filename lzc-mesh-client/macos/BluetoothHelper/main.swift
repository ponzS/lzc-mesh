import Foundation
import Darwin

var input = FileHandle.standardInput
var output = FileHandle.standardOutput
var directory: String?
var parentPID: Int32?
if CommandLine.arguments.count == 4, CommandLine.arguments[1] == "--pipes" {
    let path = CommandLine.arguments[2]
    guard let pid = Int32(CommandLine.arguments[3]), pid > 1 else { exit(2) }
    parentPID = pid
    var info = stat()
    guard lstat(path, &info) == 0, info.st_uid == getuid(), info.st_mode & 0o777 == 0o700,
          info.st_mode & S_IFMT == S_IFDIR else { exit(2) }
    let incoming = Darwin.open(path + "/commands", O_RDWR | O_NOFOLLOW | O_CLOEXEC)
    let outgoing = Darwin.open(path + "/events", O_RDWR | O_NOFOLLOW | O_CLOEXEC)
    guard incoming >= 0, outgoing >= 0 else { exit(2) }
    for descriptor in [incoming, outgoing] {
        guard fstat(descriptor, &info) == 0, info.st_uid == getuid(), info.st_mode & S_IFMT == S_IFIFO else { exit(2) }
    }
    input = FileHandle(fileDescriptor: incoming, closeOnDealloc: true)
    output = FileHandle(fileDescriptor: outgoing, closeOnDealloc: true)
    directory = path
}
let writer = DispatchQueue(label: "cloud.lazycat.lzcmesh.bluetooth.events")
let service = BluetoothService { value in
    guard let data = try? JSONSerialization.data(withJSONObject: value) else { return }
    writer.async { try? output.write(contentsOf: data + Data([10])) }
}
let shutdownLock = NSLock()
var shuttingDown = false
func shutdown() {
    shutdownLock.lock()
    guard !shuttingDown else { shutdownLock.unlock(); return }
    shuttingDown = true
    shutdownLock.unlock()
    DispatchQueue.main.async { service.stop(); exit(0) }
    // An SDK initializer may be blocked before the main run loop starts.
    DispatchQueue.global().asyncAfter(deadline: .now() + 1) { exit(0) }
}
DispatchQueue.global(qos: .userInitiated).async {
    var buffer = Data(), bytes = [UInt8](repeating: 0, count: 16384)
    while true {
        let count = bytes.withUnsafeMutableBytes { Darwin.read(input.fileDescriptor, $0.baseAddress, $0.count) }
        if count < 0 && errno == EINTR { continue }
        if count <= 0 { break }
        buffer.append(contentsOf: bytes.prefix(count))
        if buffer.count > 131072 { exit(2) }
        while let end = buffer.firstIndex(of: 10) {
            let line = Data(buffer[..<end]); buffer.removeSubrange(...end)
            guard let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any],
                  let kind = object["_lan"] as? String,
                  ["bt_send", "bt_close", "bt_stop"].contains(kind) else { continue }
            if kind == "bt_stop" { shutdown() }
            else { DispatchQueue.main.async { service.event(object) } }
        }
    }
    shutdown()
}
let lifecycle = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
lifecycle.schedule(deadline: .now() + 0.2, repeating: 0.2)
lifecycle.setEventHandler {
    if let directory, !FileManager.default.fileExists(atPath: directory) { shutdown() }
    if let parentPID, kill(parentPID, 0) != 0 && errno == ESRCH { shutdown() }
}
lifecycle.resume()
service.start()
let keepAlive = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { _ in }
withExtendedLifetime(service) { RunLoop.main.run(until: .distantFuture) }
