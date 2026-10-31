"use strict"

// Layout page. Talks to the hub over the same WebSocket protocol as the native
// clients, but only as a viewer: it watches state and sends layout edits.

const stage = document.getElementById("stage")
const empty = document.getElementById("empty")
const list = document.getElementById("devices")
const statusText = document.getElementById("status")
const dot = document.getElementById("dot")
const swap = document.getElementById("swap")
const clipboardToggle = document.getElementById("clipboard")

const PLATFORM = { macos: "macOS", windows: "Windows", linux: "Linux" }
let ws = null
let state = { devices: [], active: null, settings: { swap_cmd_ctrl: true, clipboard: false } }
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
    if (msg.t === "files") renderFiles(msg.files)
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
  const narrow = stage.clientWidth < 600
  const pad = narrow ? 16 : 48
  const W = stage.clientWidth - pad * 2, H = stage.clientHeight - pad * 2
  // Leave room so a screen can be dragged next to any side of the group;
  // on a phone the width is the constraint, so use almost all of it.
  const room = narrow ? 1.15 : 1.6
  const scale = Math.min(W / ((maxX - minX) * room), H / ((maxY - minY) * room), 0.25)
  view.scale = scale
  view.ox = pad + (W - (maxX - minX) * scale) / 2 - minX * scale
  view.oy = pad + (H - (maxY - minY) * scale) / 2 - minY * scale
}

function render() {
  computeView()
  swap.checked = !!state.settings?.swap_cmd_ctrl
  clipboardToggle.checked = !!state.settings?.clipboard
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
    const activeRole = state.active?.target === d.id ? "受控中" : state.active?.source === d.id ? "控制中" : d.online && d.sharing === false ? "已暂停" : ""
    el.classList.toggle("offline", !d.online)
    el.classList.toggle("active", !!activeRole)
    el.querySelector(".name").textContent = d.name
    el.querySelector(".meta").textContent = `${PLATFORM[d.platform] || d.platform} · ${Math.round(d.w)}×${Math.round(d.h)}`
    el.querySelector(".badge").textContent = activeRole || (d.online ? "" : "离线")
    el.style.left = `${view.ox + d.x * view.scale}px`
    el.style.top = `${view.oy + d.y * view.scale}px`
    el.style.width = `${d.w * view.scale}px`
    el.style.height = `${d.h * view.scale}px`
    // Small screens show only what fits: name, then the platform line.
    el.classList.toggle("compact", d.w * view.scale < 120 || d.h * view.scale < 64)
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
    info.children[1].textContent = `${PLATFORM[d.platform] || d.platform} · ${d.online ? (d.sharing === false ? "在线 · 已暂停键鼠共享" : "在线") : "离线"}${d.version ? " · v" + d.version : ""}`
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
clipboardToggle.addEventListener("change", () => send({ t: "settings", clipboard: clipboardToggle.checked }))
window.addEventListener("resize", () => drag || render())
new ResizeObserver(() => drag || render()).observe(stage)
setInterval(() => send({ t: "ping" }), 20000)
connect()
initFiles()

// ---------------------------------------------------------------- shared files

const filesBody = document.getElementById("files")
const filesEmpty = document.getElementById("files-empty")
const uploadsBox = document.getElementById("uploads")

function fmtSize(n) {
  if (n < 1024) return `${n} B`
  const units = ["KB", "MB", "GB", "TB"]
  let v = n / 1024, i = 0
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++ }
  return `${v.toFixed(v < 10 ? 1 : 0)} ${units[i]}`
}

function fmtTime(sec) {
  const d = new Date(sec * 1000), now = new Date()
  const pad = x => String(x).padStart(2, "0")
  const hm = `${pad(d.getHours())}:${pad(d.getMinutes())}`
  if (d.toDateString() === now.toDateString()) return `今天 ${hm}`
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${hm}`
}

function fileURL(name) {
  return `/api/files/${encodeURIComponent(name)}`
}

function renderFiles(files) {
  filesBody.textContent = ""
  filesEmpty.style.display = files.length ? "none" : "block"
  for (const f of files) {
    const tr = document.createElement("tr")
    const name = document.createElement("td")
    name.className = "name"
    const link = document.createElement("a")
    link.href = fileURL(f.name)
    link.download = f.name
    link.textContent = f.name
    name.appendChild(link)
    const size = document.createElement("td")
    size.className = "num meta"
    size.textContent = fmtSize(f.size)
    const from = document.createElement("td")
    from.className = "meta"
    from.textContent = f.uploader || "-"
    const time = document.createElement("td")
    time.className = "meta"
    time.textContent = fmtTime(f.modified)
    const actions = document.createElement("td")
    actions.className = "actions"
    const dl = document.createElement("button")
    dl.textContent = "下载"
    dl.onclick = () => link.click()
    const del = document.createElement("button")
    del.textContent = "删除"
    del.onclick = async () => {
      if (!confirm(`删除“${f.name}”？所有设备上都将无法再下载它。`)) return
      const r = await fetch(fileURL(f.name), { method: "DELETE" })
      if (!r.ok && r.status !== 404) alert(`删除失败：HTTP ${r.status}`)
    }
    actions.append(dl, del)
    tr.append(name, size, from, time, actions)
    filesBody.appendChild(tr)
  }
}

function upload(file) {
  const row = document.createElement("div")
  row.className = "upload"
  row.innerHTML = `<span class="uname"></span><span class="bar"><i></i></span><span class="pct">0%</span>`
  row.querySelector(".uname").textContent = file.name
  const bar = row.querySelector(".bar i"), pct = row.querySelector(".pct")
  uploadsBox.appendChild(row)
  const xhr = new XMLHttpRequest()
  xhr.open("PUT", fileURL(file.name))
  xhr.setRequestHeader("Content-Type", "application/octet-stream")
  xhr.upload.onprogress = e => {
    if (!e.lengthComputable) return
    const p = Math.floor((e.loaded / e.total) * 100)
    bar.style.width = `${p}%`
    pct.textContent = `${p}%`
  }
  const fail = text => {
    row.classList.add("failed")
    pct.textContent = text
    const close = document.createElement("button")
    close.textContent = "关闭"
    close.onclick = () => row.remove()
    row.appendChild(close)
  }
  xhr.onload = () => {
    if (xhr.status >= 200 && xhr.status < 300) row.remove()
    else fail(xhr.status === 413 ? "文件过大" : `失败 ${xhr.status}`)
  }
  xhr.onerror = () => fail("网络错误")
  xhr.send(file)
}

async function initFiles() {
  const input = document.getElementById("file-input")
  input.addEventListener("change", () => {
    for (const f of input.files) upload(f)
    input.value = ""
  })
  const zone = document.getElementById("dropzone")
  zone.addEventListener("dragover", e => {
    if (![...e.dataTransfer.types].includes("Files")) return
    e.preventDefault()
    zone.classList.add("over")
  })
  zone.addEventListener("dragleave", () => zone.classList.remove("over"))
  zone.addEventListener("drop", e => {
    e.preventDefault()
    zone.classList.remove("over")
    for (const f of e.dataTransfer.files) upload(f)
  })
  try {
    const r = await fetch("/api/files")
    if (r.ok) renderFiles((await r.json()).files)
  } catch {}
}
