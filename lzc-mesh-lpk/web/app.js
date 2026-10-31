"use strict"

// Layout page. Talks to the hub over the same WebSocket protocol as the native
// clients, but only as a viewer: it watches state and sends layout edits.

const stage = document.getElementById("stage")
const empty = document.getElementById("empty")
const list = document.getElementById("devices")
const statusText = document.getElementById("status")
const dot = document.getElementById("dot")
const swap = document.getElementById("swap")

const PLATFORM = { macos: "macOS", windows: "Windows", linux: "Linux" }
let ws = null
let state = { devices: [], active: null, settings: { swap_cmd_ctrl: true } }
let view = { scale: 1, ox: 0, oy: 0 }
let drag = null

function setStatus(text, ok) {
  statusText.textContent = text
  dot.classList.toggle("ok", !!ok)
}

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:"
  ws = new WebSocket(`${proto}//${location.host}/api/ws`)
  ws.onopen = () => {
    ws.send(JSON.stringify({ t: "hello", platform: "web" }))
    setStatus("已连接", true)
  }
  ws.onmessage = ev => {
    const msg = JSON.parse(ev.data)
    if (msg.t === "welcome") setStatus(`已连接 · ${msg.user}`, true)
    if (msg.t === "state") {
      state = msg
      if (!drag) render()
    }
    if (msg.t === "error") alert(msg.message)
  }
  ws.onclose = () => {
    setStatus("连接断开，正在重连…", false)
    setTimeout(connect, 1500)
  }
}

function send(msg) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg))
}

function computeView() {
  const ds = state.devices
  if (!ds.length) return
  const minX = Math.min(...ds.map(d => d.x)), minY = Math.min(...ds.map(d => d.y))
  const maxX = Math.max(...ds.map(d => d.x + d.w)), maxY = Math.max(...ds.map(d => d.y + d.h))
  const pad = 48
  const W = stage.clientWidth - pad * 2, H = stage.clientHeight - pad * 2
  // Leave room so a screen can be dragged next to any side of the group.
  const scale = Math.min(W / ((maxX - minX) * 1.6), H / ((maxY - minY) * 1.6), 0.25)
  view.scale = scale
  view.ox = pad + (W - (maxX - minX) * scale) / 2 - minX * scale
  view.oy = pad + (H - (maxY - minY) * scale) / 2 - minY * scale
}

function render() {
  computeView()
  swap.checked = !!state.settings?.swap_cmd_ctrl
  empty.style.display = state.devices.length ? "none" : "flex"
  const seen = new Set()
  for (const d of state.devices) {
    seen.add(d.id)
    let el = stage.querySelector(`[data-id="${CSS.escape(d.id)}"]`)
    if (!el) {
      el = document.createElement("div")
      el.className = "screen"
      el.dataset.id = d.id
      el.innerHTML = `<span class="badge"></span><div class="name"></div><div class="meta"></div>`
      el.addEventListener("pointerdown", startDrag)
      stage.appendChild(el)
    }
    const activeRole = state.active?.target === d.id ? "受控中" : state.active?.source === d.id ? "控制中" : ""
    el.classList.toggle("offline", !d.online)
    el.classList.toggle("active", !!activeRole)
    el.querySelector(".name").textContent = d.name
    el.querySelector(".meta").textContent = `${PLATFORM[d.platform] || d.platform} · ${Math.round(d.w)}×${Math.round(d.h)}`
    el.querySelector(".badge").textContent = activeRole || (d.online ? "" : "离线")
    el.style.left = `${view.ox + d.x * view.scale}px`
    el.style.top = `${view.oy + d.y * view.scale}px`
    el.style.width = `${d.w * view.scale}px`
    el.style.height = `${d.h * view.scale}px`
  }
  for (const el of [...stage.querySelectorAll(".screen")]) if (!seen.has(el.dataset.id)) el.remove()
  renderList()
}

function renderList() {
  list.innerHTML = ""
  for (const d of state.devices) {
    const li = document.createElement("li")
    const info = document.createElement("div")
    info.innerHTML = `<div></div><div class="meta"></div>`
    info.children[0].textContent = d.name
    info.children[1].textContent = `${PLATFORM[d.platform] || d.platform} · ${d.online ? "在线" : "离线"}${d.version ? " · v" + d.version : ""}`
    li.appendChild(info)
    if (!d.online) {
      const btn = document.createElement("button")
      btn.textContent = "移除"
      btn.onclick = () => send({ t: "forget", id: d.id })
      li.appendChild(btn)
    }
    list.appendChild(li)
  }
  if (!state.devices.length) list.innerHTML = `<li class="meta">暂无设备</li>`
}

function startDrag(ev) {
  const el = ev.currentTarget
  el.setPointerCapture(ev.pointerId)
  drag = { el, id: el.dataset.id, sx: ev.clientX, sy: ev.clientY, left: parseFloat(el.style.left), top: parseFloat(el.style.top) }
  el.classList.add("dragging")
  el.addEventListener("pointermove", moveDrag)
  el.addEventListener("pointerup", endDrag, { once: true })
  el.addEventListener("pointercancel", endDrag, { once: true })
}

function moveDrag(ev) {
  if (!drag) return
  drag.el.style.left = `${drag.left + ev.clientX - drag.sx}px`
  drag.el.style.top = `${drag.top + ev.clientY - drag.sy}px`
}

function endDrag() {
  if (!drag) return
  const { el, id } = drag
  el.removeEventListener("pointermove", moveDrag)
  el.classList.remove("dragging")
  const x = (parseFloat(el.style.left) - view.ox) / view.scale
  const y = (parseFloat(el.style.top) - view.oy) / view.scale
  drag = null
  send({ t: "layout", id, x, y })
  render()
}

swap.addEventListener("change", () => send({ t: "settings", swap_cmd_ctrl: swap.checked }))
window.addEventListener("resize", () => drag || render())
setInterval(() => send({ t: "ping" }), 20000)
connect()
