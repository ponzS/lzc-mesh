import { createKeyboard } from "./controller-keyboard.js?v=0.3.3"
import { createTrackpad } from "./controller-trackpad.js?v=0.3.3"

const $ = id => document.getElementById(id)
const root = $("controller"), pad = $("touchpad"), bar = $("mobile-shortcuts"), input = $("mobile-input")
const selector = $("target-device"), toggle = $("control-toggle")
let ws, source = "", connected = false, active = null, pending = false, reconnectTimer, stopped = false, manualStop = false
let state = { devices:[], settings:{} }
let notice = ""
const ready = () => connected && !!active && !document.hidden
const keyboard = createKeyboard({input, bar, toggle:$("mobile-keyboard"), ready, send, stageText, layout})
const trackpad = createTrackpad(pad, {ready, send})
const cancelGestures = transmit => trackpad.reset(transmit)

function send(msg) {
  if (ws?.readyState !== WebSocket.OPEN) return false
  // Never replay a large stale input queue after a stalled connection.
  if (ws.bufferedAmount > 256 * 1024) { ws.close(); loseConnection(); return false }
  ws.send(JSON.stringify(msg)); return true
}
function stageText(text) {
  if (!ready()) return
  if (!state.settings.clipboard) notice = "非按键文本需要在首页开启共享剪贴板；也可切换远端输入法后使用英文键盘。"
  else if (send({t:"web_text", text})) notice = "文本已发送至当前设备剪贴板，点 Ctrl+V 粘贴。"
  render()
}
function unavailable() {
  if (!connected) return "连接断开，正在重连…"
  if (state.settings.input_enabled === false) return "键鼠接力已暂停，请在首页恢复"
  if (["direct","bluetooth"].includes(state.settings.transport_mode)) return "手机需要中转，请在首页选择自动或仅中枢转发"
  if (state.active && state.active.source !== source) return "其他控制端正在使用设备，请先结束原控制"
  if (!state.devices.some(eligible)) return "没有可用设备，请先在首页批准设备并开启接力"
  return "选择设备，开始控制"
}
function eligible(d) { return d.online && d.trusted && d.enabled !== false && d.sharing !== false }
function render() {
  const previous = selector.value
  const seen = new Set()
  for (const d of state.devices) {
    seen.add(d.id)
    let option = [...selector.options].find(o => o.value === d.id)
    if (!option) { option=document.createElement("option"); option.value=d.id; selector.appendChild(option) }
    option.disabled = !eligible(d)
    const reason = d.trusted !== true ? "未批准" : !d.online ? "离线" : d.enabled === false ? "已关闭" : d.sharing === false ? "已暂停" : ""
    option.textContent = `${d.name}${reason ? ` · ${reason}` : ""}`
  }
  for (const option of [...selector.options]) if (!seen.has(option.value)) option.remove()
  if (!selector.options.length) { const o=document.createElement("option"); o.value=""; o.textContent="暂无可用设备"; selector.appendChild(o) }
  let savedTarget = ""
  try { savedTarget = localStorage.getItem("lzc-mesh-target") || "" } catch {}
  if (active) selector.value = active.target
  else if ([...selector.options].some(o => o.value===previous && !o.disabled)) selector.value = previous
  else if (savedTarget && [...selector.options].some(o => o.value===savedTarget && !o.disabled)) selector.value = savedTarget
  else selector.value = state.devices.find(eligible)?.id || ""
  const blocked = !connected || state.settings.input_enabled === false || ["direct","bluetooth"].includes(state.settings.transport_mode) || (state.active && state.active.source !== source)
  selector.disabled = !connected || pending
  toggle.disabled = pending || (!active && (blocked || !selector.value || !state.devices.some(d => d.id===selector.value && eligible(d))))
  toggle.textContent = pending ? "连接中…" : active ? "停止控制" : "开始控制"
  root.classList.toggle("controlling", !!active)
  $("current-device").textContent = active ? (state.devices.find(d => d.id===active.target)?.name || "设备") : "键盘触控板"
  $("control-status").textContent = active ? "中枢转发 · 正在控制" : unavailable()
  $("input-notice").textContent = notice
  keyboard.update()
  if (!manualStop && !active && !pending && !toggle.disabled && !document.hidden) startControl()
}
function connect() {
  if (stopped) return
  source=""; connected=false
  ws = new WebSocket(`${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/api/ws`)
  ws.onopen = () => send({t:"hello",platform:"web"})
  ws.onmessage = event => {
    const msg=JSON.parse(event.data)
    if (msg.t==="welcome" && msg.id?.startsWith("web:")) {source=msg.id; connected=true}
    if (msg.t==="state") {
      const next=msg.active?.source===source ? msg.active : null
      if (active && (!next || next.control_id!==active.control_id)) {
        cancelGestures(false); keyboard.reset(); notice=""
      } else if (active && next.target!==active.target) {
        // Keep pointing and IME focus, not a held drag or fling, across screens.
        trackpad.handoff(); notice=""
        keyboard.clearSticky()
      }
      if (pending && next) pending=false
      active=next; state=msg
    }
    if (msg.t==="error") {notice=msg.message; pending=false}
    render()
  }
  ws.onclose = () => {
    loseConnection()
    clearTimeout(reconnectTimer)
    if (!stopped) reconnectTimer=setTimeout(connect,1500)
  }
}
function loseConnection() {
  connected=false; pending=false; active=null
  cancelGestures(false); keyboard.reset(); render()
}
function endControl() {
  // Release is bound server-side to this connection, never to the selected ID.
  if (active || pending) send({t:"release"})
  active=null; pending=false; notice=""
  cancelGestures(false); keyboard.reset(); render()
}
function startControl() {
  if (!connected || !selector.value || document.hidden) return
  try { localStorage.setItem("lzc-mesh-target", selector.value) } catch {}
  cancelGestures(true); keyboard.reset(); notice=""; pending=true
  send({t:"web_control",target:selector.value}); render()
}
toggle.addEventListener("click", () => {
  if (active) { manualStop = true; endControl() }
  else { manualStop = false; startControl() }
})
selector.addEventListener("change", () => { manualStop = false; startControl() })
$("mobile-home").addEventListener("click", () => { manualStop = true; endControl() })
bar.addEventListener("pointerdown", () => trackpad.reset(true), {capture:true})

function layout() {
  const v=window.visualViewport
  const height=v?.height ?? innerHeight, top=v?.offsetTop ?? 0, width=v?.width ?? innerWidth
  if (Math.abs(width-lastViewportWidth)>80) {keyboardReference=Math.max(innerHeight,height);keyboardWasVisible=false}
  lastViewportWidth=width
  const focused=document.activeElement===input
  const keyboardVisible=focused && Math.max(innerHeight-height-top,keyboardReference-height)>80
  if (keyboardVisible) keyboardWasVisible=true
  else if (focused && keyboardWasVisible && height>=keyboardReference-40) {keyboardWasVisible=false;input.blur()}
  root.style.setProperty("--visible-height", `${height}px`)
  root.style.setProperty("--visible-width", `${v?.width ?? innerWidth}px`)
  root.style.setProperty("--visible-top", `${top}px`)
  root.style.setProperty("--visible-left", `${v?.offsetLeft ?? 0}px`)
  root.classList.toggle("keyboard-visible", keyboardVisible)
}
let keyboardReference=innerHeight, keyboardWasVisible=false, lastViewportWidth=innerWidth
input.addEventListener("focus", () => { keyboardReference=Math.max(innerHeight,window.visualViewport?.height||0); keyboardWasVisible=false; layout() })
for (const target of [window, window.visualViewport]) {
  target?.addEventListener("resize", layout)
  target?.addEventListener("scroll", layout)
}
window.addEventListener("orientationchange", () => { cancelGestures(true); keyboardReference=innerHeight; layout() })
new ResizeObserver(layout).observe(bar)

window.addEventListener("blur", endControl)
window.addEventListener("focus", () => { if (!document.hidden) render() })
document.addEventListener("visibilitychange", () => { if(document.hidden) endControl(); else render() })
window.addEventListener("pagehide", () => {endControl();stopped=true;clearTimeout(reconnectTimer);ws?.close()})
window.addEventListener("pageshow", event => {if(event.persisted) {stopped=false;connect()}})
setInterval(() => {
  if (ready()) send({t:"web_alive",control_id:active.control_id})
  else if (connected) send({t:"ping"})
},1000)
layout(); render(); connect()
