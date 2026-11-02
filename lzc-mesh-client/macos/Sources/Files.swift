// Shared files on macOS: list, upload (NSOpenPanel or drag and drop), download
// to ~/Downloads and delete, over the LazyCat network with the session cookie.
import AppKit

struct SharedFile {
    var name: String, size: Int64, modified: Double, uploader: String
}

final class FilesClient {
    var onStatus: ((String) -> Void)?
    private var active = false
    private var generation = 0
    private var tasks: [UUID: URLSessionTask] = [:]

    func setActive(_ on: Bool) {
        guard active != on else { return }
        active = on; generation += 1
        for task in tasks.values { task.cancel() }
        tasks.removeAll()
        onStatus?(on ? "文件共享已就绪" : "客户端已关闭，文件共享已停用")
    }
    private let session: URLSession = {
        let c = URLSessionConfiguration.default
        c.connectionProxyDictionary = [:]
        c.httpShouldSetCookies = false
        c.timeoutIntervalForRequest = 60
        c.timeoutIntervalForResource = 24 * 3600
        return URLSession(configuration: c, delegate: nil, delegateQueue: .main)
    }()

    private func request(_ method: String, _ name: String) -> URLRequest? {
        let s = AppState.shared
        guard active, s.operational, !s.boxDomain.isEmpty, let esc = name.addingPercentEncoding(withAllowedCharacters: .alphanumerics),
              let url = URL(string: "https://lzc-mesh.\(s.boxDomain)/api/files/\(esc)") else { return nil }
        var r = URLRequest(url: url)
        r.httpMethod = method
        r.setValue("Lzc-Auth-Token=\(s.cookie)", forHTTPHeaderField: "Cookie")
        let dev = (Host.current().localizedName ?? "Mac").addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? "Mac"
        r.setValue(dev, forHTTPHeaderField: "X-Lzc-Mesh-Device")
        r.setValue(s.deviceID, forHTTPHeaderField: "X-Lzc-Mesh-Device-Id")
        r.setValue(s.fileToken, forHTTPHeaderField: "X-Lzc-Mesh-Device-Token")
        return r
    }

    private func status(_ text: String) { onStatus?(text) }

    func upload(_ file: URL) {
        let name = file.lastPathComponent
        guard var req = request("PUT", name) else { return }
        req.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        let size = (try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        status("正在上传 \(name)（\(ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file))）…")
        let id = UUID(), epoch = generation
        let task = session.uploadTask(with: req, fromFile: file) { [weak self] _, resp, err in
            guard let self else { return }
            self.tasks[id] = nil
            guard self.active, self.generation == epoch, AppState.shared.operational else { return }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if code == 200 { AppState.shared.log("已上传 \(name)"); self.onStatus?("已上传 \(name)") }
            else {
                let why = err?.localizedDescription ?? (code == 413 ? "文件过大" : "HTTP \(code)")
                AppState.shared.log("上传 \(name) 失败：\(why)"); self.onStatus?("上传失败：\(why)")
            }
        }
        tasks[id] = task; task.resume()
    }

    func download(_ name: String) {
        guard let req = request("GET", name) else { return }
        status("正在下载 \(name)…")
        let id = UUID(), epoch = generation
        let task = session.downloadTask(with: req) { [weak self] tmp, resp, err in
            guard let self else { return }
            self.tasks[id] = nil
            guard self.active, self.generation == epoch, AppState.shared.operational else { return }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard let tmp = tmp, code == 200 else {
                let why = err?.localizedDescription ?? "HTTP \(code)"
                AppState.shared.log("下载 \(name) 失败：\(why)"); self.onStatus?("下载失败：\(why)")
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
                AppState.shared.log("已下载 \(name) → \(dest.path)")
                self.onStatus?("已下载到 \(dest.path)")
                NSWorkspace.shared.activateFileViewerSelecting([dest])
            } catch {
                self.onStatus?("保存失败：\(error.localizedDescription)")
            }
        }
        tasks[id] = task; task.resume()
    }

    func delete(_ name: String) {
        guard let req = request("DELETE", name) else { return }
        let id = UUID(), epoch = generation
        let task = session.dataTask(with: req) { [weak self] _, resp, err in
            guard let self else { return }
            self.tasks[id] = nil
            guard self.active, self.generation == epoch, AppState.shared.operational else { return }
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            self.status(code == 204 || code == 404 ? "已删除 \(name)" : "删除失败：\(err?.localizedDescription ?? "HTTP \(code)")")
        }
        tasks[id] = task; task.resume()
    }
}

/// List of shared files with upload / download / delete, accepts dropped files.
final class FilesView: NSView {
    let client = FilesClient()
    private let stack = NSStackView()
    private let statusLabel = makeLabel("", size: 11, color: Theme.muted)
    private let emptyLabel = makeLabel("还没有共享文件。点击“上传文件”或把文件拖到这里。", size: 12, color: Theme.muted)
    private var names: [String] = []
    private var uploadButton: NSButton?
    private let df: DateFormatter = { let f = DateFormatter(); f.dateFormat = "MM-dd HH:mm"; return f }()

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 0
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
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
        emptyLabel.alignment = .center
        let upload = FlatButton(title: "上传文件", primary: true, target: self, action: #selector(pick))
        uploadButton = upload; upload.isEnabled = false
        let hint = makeLabel("所有设备都能下载；下载到“下载”文件夹，也可把文件拖进来", size: 11, color: Theme.muted)
        let spacer = NSView()
        spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let bar = NSStackView(views: [hint, spacer, upload])
        bar.spacing = 8
        let root = NSStackView(views: [bar, emptyLabel, scroll, statusLabel])
        root.orientation = .vertical
        root.alignment = .leading
        root.spacing = 8
        root.translatesAutoresizingMaskIntoConstraints = false
        addSubview(root)
        NSLayoutConstraint.activate([
            root.topAnchor.constraint(equalTo: topAnchor), root.bottomAnchor.constraint(equalTo: bottomAnchor),
            root.leadingAnchor.constraint(equalTo: leadingAnchor), root.trailingAnchor.constraint(equalTo: trailingAnchor),
            bar.widthAnchor.constraint(equalTo: root.widthAnchor), scroll.widthAnchor.constraint(equalTo: root.widthAnchor),
            emptyLabel.widthAnchor.constraint(equalTo: root.widthAnchor), statusLabel.widthAnchor.constraint(equalTo: root.widthAnchor),
            scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 90),
        ])
        client.onStatus = { [weak self] in self?.statusLabel.stringValue = $0 }
    }

    required init?(coder: NSCoder) { fatalError() }

    final class FlippedView: NSView { override var isFlipped: Bool { true } }

    func setActive(_ on: Bool) {
        client.setActive(on)
        uploadButton?.isEnabled = on
        if !on { update([]) }
    }

    func update(_ files: [SharedFile]) {
        stack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        emptyLabel.isHidden = !files.isEmpty
        names = files.map { $0.name }
        for (i, f) in files.enumerated() {
            let name = makeLabel(f.name, size: 12, color: Theme.text)
            name.lineBreakMode = .byTruncatingMiddle
            name.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            let size = ByteCountFormatter.string(fromByteCount: f.size, countStyle: .file)
            let meta = makeLabel("\(size) · \(f.uploader.isEmpty ? "-" : f.uploader) · \(df.string(from: Date(timeIntervalSince1970: f.modified)))", size: 11, color: Theme.muted)
            meta.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)
            let dl = FlatButton(title: "下载", primary: false, target: self, action: #selector(download(_:)))
            let rm = FlatButton(title: "删除", primary: false, target: self, action: #selector(remove(_:)))
            for b in [dl, rm] { b.tag = i }
            let spacer = NSView()
            spacer.setContentHuggingPriority(.defaultLow, for: .horizontal)
            let row = NSStackView(views: [name, spacer, meta, dl, rm])
            row.spacing = 8
            row.edgeInsets = NSEdgeInsets(top: 6, left: 0, bottom: 6, right: 0)
            let sep = NSView()
            sep.wantsLayer = true
            sep.layer?.backgroundColor = Theme.line.cg
            stack.addArrangedSubview(row)
            stack.addArrangedSubview(sep)
            row.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
            sep.translatesAutoresizingMaskIntoConstraints = false
            sep.heightAnchor.constraint(equalToConstant: 1).isActive = true
            sep.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
    }

    @objc private func pick() {
        guard AppState.shared.operational else { statusLabel.stringValue = "连接中枢后才能上传"; return }
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

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { AppState.shared.operational ? .copy : [] }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard AppState.shared.operational, let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] else { return false }
        urls.filter { !$0.hasDirectoryPath }.forEach { client.upload($0) }
        return true
    }
}
