// Start at login via a per-user LaunchAgent (no entitlements or notarization needed).
import Foundation

enum LoginItem {
    static let label = "cloud.lazycat.lzcmesh.login"

    private static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    static var isEnabled: Bool { FileManager.default.fileExists(atPath: plistURL.path) }

    /// Path of the running app bundle's executable. Refuses to register a build that is not
    /// inside an .app (e.g. a debug binary), because the agent would point at a temp path.
    private static var executable: String? {
        let path = Bundle.main.executablePath
        return path?.contains(".app/Contents/MacOS/") == true ? path : nil
    }

    @discardableResult
    static func set(_ on: Bool) -> String? {
        if !on {
            try? FileManager.default.removeItem(at: plistURL)
            return nil
        }
        guard let exe = executable else { return "只有安装为应用后才能设置开机启动" }
        let plist: [String: Any] = [
            "Label": label,
            "ProgramArguments": [exe, "--background"],
            "RunAtLoad": true,
            "ProcessType": "Interactive",
        ]
        do {
            try FileManager.default.createDirectory(at: plistURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            let data = try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
            try data.write(to: plistURL, options: .atomic)
            return nil
        } catch {
            return error.localizedDescription
        }
    }
}
