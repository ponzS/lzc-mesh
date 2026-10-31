// Shared files on macOS: list, upload (NSOpenPanel or drag and drop), download
// to ~/Downloads and delete, over the LazyCat network with the session cookie.
import AppKit

struct SharedFile {
    var name: String, size: Int64, modified: Double, uploader: String
}

final class FilesClient {
    var onStatus: ((String) -> Void)?
    private let session: URLSession = {
        let c = URLSessionConfiguration.default
        c.connectionProxyDictionary = [:]
        c.httpShouldSetCookies = false
        c.timeoutIntervalForRequest = 60
        c.timeoutIntervalForResource = 24 * 3600
        return URLSession(configuration: c)
    }()

    private func request(_ method: String, _ name: String) -> URLRequest? {
        let s = AppState.shared
        guard !s.boxDomain.isEmpty, let esc = name.addingPercentEncoding(withAllowedCharacters: .alphanumerics),
              let url = URL(string: "https://lzc-mesh.\(s.boxDomain)/api/files/\(esc)") else { return nil }
        var r = URLRequest(url: url)
        r.httpMethod = method
        r.setValue("Lzc-Auth-Token=\(s.cookie)", forHTTPHeaderField: "Cookie")
        let dev = (Host.current().localizedName ?? "Mac").addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? "Mac"
        r.setValue(dev, forHTTPHeaderField: "X-Lzc-Mesh-Device")
        return r
    }

    private func status(_ text: String) { DispatchQueue.main.async { self.onStatus?(text) } }

    func upload(_ file: URL) {
        let name = file.lastPathComponent
        guard var req = request("PUT", name) else { return }
        req.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        let size = (try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        status("正在上传 \(name)（\(ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file))）…")
        session.uploadTask(with: req, fromFile: file) { _, resp, err in
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            DispatchQueue.main.async {
                if code == 200 { AppState.shared.log("已上传 \(name)"); self.onStatus?("已上传 \(name)") }
                else {
                    let why = err?.localizedDescription ?? (code == 413 ? "文件过大" : "HTTP \(code)")
                    AppState.shared.log("上传 \(name) 失败：\(why)")
                    self.onStatus?("上传失败：\(why)")
                }
            }
        }.resume()
    }

    func download(_ name: String) {
        guard let req = request("GET", name) else { return }
        status("正在下载 \(name)…")
        session.downloadTask(with: req) { tmp, resp, err in
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard let tmp = tmp, code == 200 else {
                let why = err?.localizedDescription ?? "HTTP \(code)"
                DispatchQueue.main.async { AppState.shared.log("下载 \(name) 失败：\(why)"); self.onStatus?("下载失败：\(why)") }
                return
            }
            let fm = FileManager.default
            let dir = fm.urls(for: .downloadsDirectory, in: .userDomainMask).first ?? fm.homeDirectoryForCurrentUser
            var dest = dir.appendingPathComponent(name)
            let stem = dest.deletingPathExtension().lastPathComponent, ext = dest.pathExtension
            var n = 2
            while fm.fileExists(atPath: dest.path) {
                dest = dir.appendingPathComponent(ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)")
                n += 1
            }
            do {
                try fm.moveItem(at: tmp, to: dest)
                DispatchQueue.main.async {
                    AppState.shared.log("已下载 \(name) → \(dest.path)")
                    self.onStatus?("已下载到 \(dest.path)")
                    NSWorkspace.shared.activateFileViewerSelecting([dest])
                }
            } catch {
                DispatchQueue.main.async { self.onStatus?("保存失败：\(error.localizedDescription)") }
            }
        }.resume()
    }

    func delete(_ name: String) {
        guard let req = request("DELETE", name) else { return }
        session.dataTask(with: req) { _, resp, err in
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            self.status(code == 204 || code == 404 ? "已删除 \(name)" : "删除失败：\(err?.localizedDescription ?? "HTTP \(code)")")
        }.resume()
    }
}

/// List of shared files with upload / download / delete, accepts dropped files.
final class FilesView: NSView {
    let client = FilesClient()
    private let stack = NSStackView()
    private let statusLabel = NSTextField(labelWithString: "")
    private let emptyLabel = NSTextField(labelWithString: "还没有共享文件。点击“上传文件”或把文件拖到这里。")
    private var names: [String] = []

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 4
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        let doc = FlippedView()
        doc.translatesAutoresizingMaskIntoConstraints = false
        stack.translatesAutoresizingMaskIntoConstraints = false
        doc.addSubview(stack)
        scroll.documentView = doc
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: doc.topAnchor), stack.leadingAnchor.constraint(equalTo: doc.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: doc.trailingAnchor), stack.bottomAnchor.constraint(equalTo: doc.bottomAnchor),
            doc.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
        ])
        for l in [statusLabel, emptyLabel] { l.font = .systemFont(ofSize: 11); l.textColor = .gray }
        let upload = NSButton(title: "上传文件", target: self, action: #selector(pick))
        upload.bezelStyle = .rounded
        let hint = NSTextField(wrappingLabelWithString: "上传的文件所有设备都能下载；下载保存到“下载”文件夹。")
        hint.font = .systemFont(ofSize: 11)
        hint.textColor = .gray
        let bar = NSStackView(views: [hint, upload])
        bar.spacing = 8
        let root = NSStackView(views: [bar, emptyLabel, scroll, statusLabel])
        root.orientation = .vertical
        root.alignment = .leading
        root.spacing = 6
        root.translatesAutoresizingMaskIntoConstraints = false
        addSubview(root)
        NSLayoutConstraint.activate([
            root.topAnchor.constraint(equalTo: topAnchor), root.bottomAnchor.constraint(equalTo: bottomAnchor),
            root.leadingAnchor.constraint(equalTo: leadingAnchor), root.trailingAnchor.constraint(equalTo: trailingAnchor),
            bar.widthAnchor.constraint(equalTo: root.widthAnchor), scroll.widthAnchor.constraint(equalTo: root.widthAnchor),
            scroll.heightAnchor.constraint(equalToConstant: 120),
        ])
        client.onStatus = { [weak self] in self?.statusLabel.stringValue = $0 }
    }

    required init?(coder: NSCoder) { fatalError() }

    final class FlippedView: NSView { override var isFlipped: Bool { true } }

    func update(_ files: [SharedFile]) {
        stack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        emptyLabel.isHidden = !files.isEmpty
        names = files.map { $0.name }
        let df = DateFormatter()
        df.dateFormat = "MM-dd HH:mm"
        for (i, f) in files.enumerated() {
            let name = NSTextField(labelWithString: f.name)
            name.lineBreakMode = .byTruncatingMiddle
            name.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            let meta = NSTextField(labelWithString: "\(ByteCountFormatter.string(fromByteCount: f.size, countStyle: .file)) · \(f.uploader.isEmpty ? "-" : f.uploader) · \(df.string(from: Date(timeIntervalSince1970: f.modified)))")
            meta.font = .systemFont(ofSize: 11)
            meta.textColor = .gray
            let dl = NSButton(title: "下载", target: self, action: #selector(download(_:)))
            let rm = NSButton(title: "删除", target: self, action: #selector(remove(_:)))
            for b in [dl, rm] { b.tag = i; b.bezelStyle = .rounded; b.controlSize = .small }
            let spacer = NSView()
            spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
            let row = NSStackView(views: [name, spacer, meta, dl, rm])
            row.spacing = 8
            stack.addArrangedSubview(row)
            row.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -16).isActive = true
        }
    }

    @objc private func pick() {
        guard AppState.shared.hubConnected else { statusLabel.stringValue = "连接中枢后才能上传"; return }
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        panel.message = "选择要共享的文件"
        panel.beginSheetModal(for: window!) { r in
            if r == .OK { panel.urls.forEach { self.client.upload($0) } }
        }
    }

    @objc private func download(_ b: NSButton) { if b.tag < names.count { client.download(names[b.tag]) } }

    @objc private func remove(_ b: NSButton) {
        guard b.tag < names.count else { return }
        let name = names[b.tag]
        let a = NSAlert()
        a.messageText = "删除“\(name)”？"
        a.informativeText = "所有设备上都将无法再下载它。"
        a.addButton(withTitle: "删除")
        a.addButton(withTitle: "取消")
        a.beginSheetModal(for: window!) { r in if r == .alertFirstButtonReturn { self.client.delete(name) } }
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { AppState.shared.hubConnected ? .copy : [] }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] else { return false }
        urls.filter { !$0.hasDirectoryPath }.forEach { client.upload($0) }
        return true
    }
}
