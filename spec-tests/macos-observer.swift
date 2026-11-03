// Private AppKit OS-input observer. Records only planned non-text test keys
// delivered to its own view. Input is sent by a separate authorized CLI fixture.
import AppKit
import ApplicationServices
import Carbon
import Darwin

final class TestWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}
final class TestView: NSView {
    weak var owner: Observer?
    override var acceptsFirstResponder: Bool { true }
    override func draw(_ rect: NSRect) { NSColor.white.setFill();rect.fill() }
    override func keyDown(with e: NSEvent) { if e.keyCode==97 { owner?.record(["kind":"key","code":97,"down":true,"flags":e.modifierFlags.rawValue]) } }
    override func keyUp(with e: NSEvent) { if e.keyCode==97 { owner?.record(["kind":"key","code":97,"down":false,"flags":e.modifierFlags.rawValue]) } }
    override func flagsChanged(with e: NSEvent) { if e.keyCode==56 || e.keyCode==60 { owner?.record(["kind":"modifier","code":e.keyCode,"flags":e.modifierFlags.rawValue]) } }
    override func mouseDown(with e: NSEvent) { owner?.record(["kind":"button","button":1,"down":true]) }
    override func mouseUp(with e: NSEvent) { owner?.record(["kind":"button","button":1,"down":false]) }
    override func rightMouseDown(with e: NSEvent) { owner?.record(["kind":"button","button":2,"down":true]) }
    override func rightMouseUp(with e: NSEvent) { owner?.record(["kind":"button","button":2,"down":false]) }
    override func mouseMoved(with e: NSEvent) { owner?.motion(e) }
    override func mouseDragged(with e: NSEvent) { owner?.motion(e) }
    override func scrollWheel(with e: NSEvent) { owner?.wheelX += e.scrollingDeltaX;owner?.wheelY += e.scrollingDeltaY }
}
@MainActor final class Observer {
    let directory:URL
    let previousApp=NSWorkspace.shared.frontmostApplication
    let previousCursor=CGEvent(source:nil)?.location ?? .zero
    let started=Date()
    let window:TestWindow
    var events:[[String:Any]]=[]
    var positions:[[String:Any]]=[]
    var moves=0,commandID=0
    var commandError=""
    var wheelX=0.0,wheelY=0.0
    var restore=true
    init(_ directory:URL) {
        self.directory=directory
        let frame=NSScreen.screens.reduce(NSRect.null){$0.union($1.frame)}
        window=TestWindow(contentRect:frame,styleMask:[.borderless],backing:.buffered,defer:false)
        let view=TestView(frame:NSRect(origin:.zero,size:frame.size));view.owner=self
        window.contentView=view;window.level = .floating;window.isReleasedWhenClosed=false;window.acceptsMouseMovedEvents=true
        window.makeKeyAndOrderFront(nil);window.makeFirstResponder(view)
    }
    func record(_ event:[String:Any]) {if events.count<256{events.append(event)}}
    func motion(_ event:NSEvent) {
        moves += 1
        if positions.count<2048,let cg=event.cgEvent {
            positions.append(["x":cg.location.x,"y":cg.location.y,"marker":cg.getIntegerValueField(.eventSourceUserData)])
        }
    }
    func visible()->Bool {
        guard let symbol=dlsym(UnsafeMutableRawPointer(bitPattern:-2),"CGCursorIsVisible") else{return false}
        return unsafeBitCast(symbol,to:(@convention(c) ()->Int32).self)() != 0
    }
    func stop() {
        NSApp.stop(nil)
        if let event=NSEvent.otherEvent(with:.applicationDefined,location:.zero,modifierFlags:[],timestamp:ProcessInfo.processInfo.systemUptime,windowNumber:0,context:nil,subtype:0,data1:0,data2:0) { NSApp.postEvent(event,atStart:true) }
    }
    func tick() {
        let commandURL=directory.appendingPathComponent("macos-observer.json.command")
        if let data=try? Data(contentsOf:commandURL),let c=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] {
            try? FileManager.default.removeItem(at:commandURL);commandID=c["id"] as? Int ?? 0;commandError=""
            if c["op"] as? String=="reset" {events=[];positions=[];moves=0;wheelX=0;wheelY=0;window.makeKeyAndOrderFront(nil);NSApp.activate(ignoringOtherApps:true)}
            if c["op"] as? String=="park" {
                if window.isKeyWindow,let session=CGSessionCopyCurrentDictionary() as? [String:Any],session["CGSSessionScreenIsLocked"] as? Bool != true,!IsSecureEventInputEnabled() {
                    let bounds=CGDisplayBounds(CGMainDisplayID())
                    if CGWarpMouseCursorPosition(CGPoint(x:bounds.midX,y:bounds.midY)) != .success { commandError="Cursor park failed" }
                } else { commandError="Fixture lost focus; park refused" }
            }
            if c["op"] as? String=="quit" {restore=c["restore"] as? Bool ?? true;stop()}
        }
        if Date().timeIntervalSince(started)>1800 {restore=false;stop()}
        let point=CGEvent(source:nil)?.location ?? .zero
        let session=CGSessionCopyCurrentDictionary() as? [String:Any]
        let unlocked=session != nil && session?["CGSSessionScreenIsLocked"] as? Bool != true && !IsSecureEventInputEnabled()
        let value:[String:Any]=["command":commandID,"error":commandError,"events":events,"positions":positions,"moves":moves,"cursor_x":point.x,"cursor_y":point.y,"cursor_visible":visible(),"wheel_x":wheelX,"wheel_y":wheelY,"focused":window.isKeyWindow && unlocked]
        if let data=try? JSONSerialization.data(withJSONObject:value){try? data.write(to:directory.appendingPathComponent("macos-observer.json"),options:.atomic)}
    }
    func run() {
        // Prevent App Nap from suspending an otherwise visible test heartbeat;
        // this explicitly does not inhibit normal system sleep or screen lock.
        let activity=ProcessInfo.processInfo.beginActivity(options:[.userInitiatedAllowingIdleSystemSleep,.latencyCritical],reason:"Isolated LZC Mesh input verification")
        defer { ProcessInfo.processInfo.endActivity(activity) }
        let timer=Timer(timeInterval:0.05,repeats:true){[weak self] _ in MainActor.assumeIsolated{self?.tick()}}
        RunLoop.main.add(timer,forMode:.common);NSApp.activate(ignoringOtherApps:true);NSApp.run();timer.invalidate();window.close()
        if restore {CGWarpMouseCursorPosition(previousCursor);previousApp?.activate(options:[])}
    }
}
MainActor.assumeIsolated {
    let app=NSApplication.shared
    app.setActivationPolicy(.accessory)
    guard !IsSecureEventInputEnabled() else{exit(3)}
    let observer=Observer(URL(fileURLWithPath:CommandLine.arguments[1],isDirectory:true))
    observer.run()
}
