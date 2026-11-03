// Synthetic non-text OS input for an explicitly owned native test session.
// No AppKit, no permission requests, no credential or clipboard access.
import Foundation
import ApplicationServices
import Carbon
let file=URL(fileURLWithPath:CommandLine.arguments[1])
let session=CGSessionCopyCurrentDictionary() as? [String:Any]
guard let session,session["CGSSessionScreenIsLocked"] as? Bool != true,
      !IsSecureEventInputEnabled(),CGPreflightPostEventAccess(),
      let data=try? Data(contentsOf:file),let command=(try? JSONSerialization.jsonObject(with:data)) as? [String:Any] else {exit(3)}
let marker:Int64=0x54535431
func post(_ event:CGEvent?){event?.setIntegerValueField(.eventSourceUserData,value:marker);event?.post(tap:.cghidEventTap)}
if command["op"] as? String == "edge" {
 let direction=command["direction"] as? Int ?? 0,pos=command["position"] as? Double ?? 0.5,b=CGDisplayBounds(CGMainDisplayID())
 let p=CGPoint(x:direction==0 ? b.minX : direction==1 ? b.maxX-1 : b.minX+pos*(b.width-1),y:direction==2 ? b.minY : direction==3 ? b.maxY-1 : b.minY+pos*(b.height-1))
 let event=CGEvent(mouseEventSource:nil,mouseType:.mouseMoved,mouseCursorPosition:p,mouseButton:.left)
 event?.setDoubleValueField(.mouseEventDeltaX,value:direction==0 ? -1 : direction==1 ? 1 : 0)
 event?.setDoubleValueField(.mouseEventDeltaY,value:direction==2 ? -1 : direction==3 ? 1 : 0);post(event)
} else {
 var flags:UInt64=0
 for e in command["events"] as? [[String:Any]] ?? [] {
  switch e["t"] as? String ?? "" {
  case "key":
   let key=CGKeyCode(e["code"] as? Int ?? 97),down=e["down"] as? Bool ?? false
   guard [97,56,59,58,53].contains(key) else {exit(4)}
   let masks:[CGKeyCode:UInt64]=[56:0x20002,59:0x40001,58:0x80020]
   if let mask=masks[key]{if down{flags|=mask}else{flags &= ~mask}}
   let event=CGEvent(keyboardEventSource:nil,virtualKey:key,keyDown:down);event?.flags=CGEventFlags(rawValue:flags);post(event)
  case "motion":
   let event=CGEvent(mouseEventSource:nil,mouseType:.mouseMoved,mouseCursorPosition:CGEvent(source:nil)?.location ?? .zero,mouseButton:.left)
   event?.setDoubleValueField(.mouseEventDeltaX,value:e["dx"] as? Double ?? 0);event?.setDoubleValueField(.mouseEventDeltaY,value:e["dy"] as? Double ?? 0);post(event)
  case "button":
   let right=e["button"] as? Int == 2,down=e["down"] as? Bool ?? false
   post(CGEvent(mouseEventSource:nil,mouseType:right ? (down ? .rightMouseDown:.rightMouseUp):(down ? .leftMouseDown:.leftMouseUp),mouseCursorPosition:CGEvent(source:nil)?.location ?? .zero,mouseButton:right ? .right:.left))
  case "wheel":post(CGEvent(scrollWheelEvent2Source:nil,units:.line,wheelCount:2,wheel1:Int32(e["dy"] as? Int ?? 0),wheel2:Int32(e["dx"] as? Int ?? 0),wheel3:0))
  default:exit(4)
  }
  Thread.sleep(forTimeInterval:0.03)
 }
}
print("Applied planned non-text test events")
