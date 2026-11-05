// Adapted from lightos-admin/lightos-remote-desktop/web/src/mobile.ts:
// same two rows, one-shot sticky modifiers, IME sentinel and keyboard focus rules.
const ROWS = [
  [
    { label:"Ctrl", key:"ControlLeft", modifier:true }, { label:"Alt", key:"AltLeft", modifier:true },
    { label:"Shift", key:"ShiftLeft", modifier:true }, { label:"Super", key:"MetaLeft", modifier:true },
    ...[["Tab","Tab"],["Esc","Escape"],["↑","ArrowUp"],["↓","ArrowDown"],["←","ArrowLeft"],["→","ArrowRight"],["Enter","Enter"]].map(([label,key]) => ({label,key})),
  ],
  [
    ...["C","V","A"].map(k => ({ label:`Ctrl+${k}`, key:`Key${k}`, modifiers:["ControlLeft"] })),
    ...[["Home","Home"],["End","End"],["PgUp","PageUp"],["PgDn","PageDown"],["⌫","Backspace"],["Del","Delete"]].map(([label,key]) => ({label,key})),
    ...["/","-","~","$"].map(text => ({label:text,text})),
  ],
]
const CODES = { Escape:1, Backspace:14, Tab:15, Enter:28, ControlLeft:29, ControlRight:97, ShiftLeft:42, ShiftRight:54, AltLeft:56, AltRight:100, MetaLeft:125, MetaRight:126, Space:57, ArrowUp:103, ArrowDown:108, ArrowLeft:105, ArrowRight:106, Home:102, End:107, PageUp:104, PageDown:109, Delete:111, Insert:110 }
for (const [letters,codes] of [["QWERTYUIOP",[16,17,18,19,20,21,22,23,24,25]],["ASDFGHJKL",[30,31,32,33,34,35,36,37,38]],["ZXCVBNM",[44,45,46,47,48,49,50]]]) {
  [...letters].forEach((letter,i) => CODES[`Key${letter}`] = codes[i])
}
const SYMBOLS = {}
for (const [name,code,plain,shifted] of [
  ["Backquote",41,"`","~"],["Minus",12,"-","_"],["Equal",13,"=","+"],
  ["BracketLeft",26,"[","{"],["BracketRight",27,"]","}"],["Backslash",43,"\\","|"],
  ["Semicolon",39,";",":"],["Quote",40,"'",'"'],["Comma",51,",","<"],["Period",52,".",">"],["Slash",53,"/","?"],["Space",57," "," "],
  ...[..."1234567890"].map((n,i) => [`Digit${n}`,i+2,n,"!@#$%^&*()"[i]]),
]) {
  CODES[name] = code
  SYMBOLS[plain] = [name,false]
  SYMBOLS[shifted] = [name,plain !== shifted]
}
const MODIFIERS = new Set(["ControlLeft","AltLeft","ShiftLeft","MetaLeft"])
const SENTINEL = "\u200b"
const SPECIAL = {deleteContentBackward:"Backspace", deleteContentForward:"Delete", insertLineBreak:"Enter", insertParagraph:"Enter"}

export function createKeyboard({ input, bar, toggle, ready, send, stageText, layout }) {
  const sticky = new Set()
  const buttons = []
  let composing = false, committed = "", commitTimer
  function resetField() { input.value = SENTINEL; input.setSelectionRange(1,1) }
  function update() {
    const active = document.activeElement === input
    toggle.setAttribute("aria-pressed", String(active))
    toggle.setAttribute("aria-label", active ? "收起键盘" : "展开键盘")
    toggle.title = active ? "收起键盘" : "展开键盘"
    for (const [button,item] of buttons) {
      button.disabled = !ready()
      if (item.modifier) button.setAttribute("aria-pressed", String(sticky.has(item.key)))
    }
    input.disabled = toggle.disabled = !ready()
  }
  function chord(key, modifiers = []) {
    if (!ready() || !CODES[key]) return
    const keys = [...new Set([...sticky, ...modifiers, key])]
    sticky.clear()
    for (const k of keys) send({t:"key",code:CODES[k],down:true})
    for (const k of keys.reverse()) send({t:"key",code:CODES[k],down:false})
    update()
  }
  function text(value) {
    if (!value || !ready()) return
    if ([...value].some(c => !/[a-zA-Z\n\r\t]/.test(c) && !SYMBOLS[c])) {
      sticky.clear(); stageText(value); update(); return
    }
    for (const c of value) {
      if (c === "\n" || c === "\r") chord("Enter")
      else if (c === "\t") chord("Tab")
      else {
        const symbol = SYMBOLS[c]
        chord(symbol?.[0] || `Key${c.toUpperCase()}`, (symbol ? symbol[1] : c === c.toUpperCase()) ? ["ShiftLeft"] : [])
      }
    }
  }
  for (const [i,row] of ROWS.entries()) {
    const element = document.getElementById(`shortcut-row-${i+1}`)
    for (const item of row) {
      const button = document.createElement("button")
      button.type = "button"; button.className = "mobile-key"; button.textContent = item.label
      button.setAttribute("aria-label", item.modifier ? `粘滞 ${item.label}` : item.label)
      if (item.key) button.dataset.mobileKey = item.key
      button.addEventListener("click", () => {
        if (!ready()) return
        if (item.modifier) { if (sticky.has(item.key)) sticky.delete(item.key); else sticky.add(item.key); update() }
        else if (item.key) chord(item.key,item.modifiers)
        else text(item.text)
      })
      buttons.push([button,item]); element.appendChild(button)
    }
  }
  // Like the remote-desktop bar, prevent focus transfer on pointer-down. Native
  // horizontal panning is still governed by touch-action: pan-x, not this default.
  bar.addEventListener("pointerdown", event => { if (event.target.closest(".mobile-key")) event.preventDefault() })
  bar.addEventListener("mousedown", event => { if (event.target.closest(".mobile-key")) event.preventDefault() })
  toggle.addEventListener("click", () => {
    if (document.activeElement === input) input.blur()
    else if (ready()) { resetField(); input.focus({preventScroll:true}) }
  })
  input.addEventListener("focus", () => { update(); layout() })
  input.addEventListener("blur", () => { composing=false; committed=""; sticky.clear(); resetField(); update(); layout() })
  input.addEventListener("compositionstart", () => { composing=true; committed=""; clearTimeout(commitTimer) })
  input.addEventListener("compositionend", event => {
    composing=false; committed=event.data; text(event.data); resetField()
    clearTimeout(commitTimer); commitTimer=setTimeout(() => {committed=""}, 0)
  })
  input.addEventListener("beforeinput", event => {
    if (composing || event.isComposing || !event.cancelable) return
    if (SPECIAL[event.inputType]) { event.preventDefault(); chord(SPECIAL[event.inputType]); resetField() }
  })
  input.addEventListener("input", event => {
    if (composing || event.isComposing) return
    const value = input.value.startsWith(SENTINEL) ? input.value.slice(1) : input.value
    if (SPECIAL[event.inputType]) chord(SPECIAL[event.inputType])
    else if (value && value !== committed) text(value)
    committed=""; resetField()
  })
  input.addEventListener("keydown", event => {
    if (composing || event.isComposing || event.keyCode === 229) return
    const key = event.code || ({Enter:"Enter",Backspace:"Backspace",Delete:"Delete",Tab:"Tab",Escape:"Escape"})[event.key]
    if (!key || MODIFIERS.has(key) || !CODES[key]) return
    if (event.ctrlKey || event.altKey || event.metaKey || event.key.length !== 1) {
      event.preventDefault()
      chord(key, [event.ctrlKey?"ControlLeft":"",event.altKey?"AltLeft":"",event.shiftKey?"ShiftLeft":"",event.metaKey?"MetaLeft":""].filter(Boolean))
    }
  })
  input.addEventListener("paste", event => { event.preventDefault(); text(event.clipboardData?.getData("text/plain") || ""); resetField() })
  resetField(); update()
  return { update, clearSticky() { sticky.clear(); update() }, reset() { composing=false; committed=""; clearTimeout(commitTimer); sticky.clear(); input.blur(); resetField(); update() } }
}
