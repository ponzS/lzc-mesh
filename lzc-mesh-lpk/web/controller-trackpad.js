// Touch-only approximation of Magic Trackpad with tap-to-click and three-finger
// dragging enabled. Phone browsers cannot sense its mechanical/Force Touch click.
// Touch Events provide atomic finger snapshots; separate pointermove events would
// turn finger arrival/lift into centroid jumps and distort scroll velocity.
const TAP_TIME = 250, TAP_SLOP = 6, DOUBLE_TIME = 300, DOUBLE_SLOP = 30
const SCROLL_UNIT = 12, DECAY = 0.995, MIN_SPEED = 0.02, EDGE_SCROLL_WIDTH = 68
const center = points => ({x:points.reduce((n,p)=>n+p.x,0)/points.length, y:points.reduce((n,p)=>n+p.y,0)/points.length})
const distance = (a,b) => Math.hypot(a.x-b.x,a.y-b.y)
const scrollGain = (dist,dt) => 1 + Math.min(3.2, Math.max(0, dist / Math.max(4, dt) - 0.22) * 1.45)

export function createTrackpadGestures({ready, send, now=()=>performance.now(), requestFrame=requestAnimationFrame, cancelFrame=cancelAnimationFrame}) {
  let gesture=null, lastTap=null, held=false, scroll=false, history=[]
  let motionX=0, motionY=0, motionFrame=0, momentumFrame=0, momentum=false
  function flush() {
    if (motionFrame) cancelFrame(motionFrame)
    motionFrame=0
    const dx=motionX,dy=motionY; motionX=motionY=0
    if (ready() && (dx || dy)) send({t:"motion",dx,dy})
  }
  function move(dx,dy) {
    motionX+=dx*2; motionY+=dy*2
    if (!motionFrame) motionFrame=requestFrame(flush)
  }
  function button(down) { flush(); if (held!==down) {held=down;send({t:"button",button:1,down})} }
  function click(which) { flush();send({t:"button",button:which,down:true});send({t:"button",button:which,down:false}) }
  function wheel(dx,dy,phase,momentumPhase) {
    send({t:"wheel",dx:dx*SCROLL_UNIT,dy:dy*SCROLL_UNIT,source:"continuous",phase,
      ...(momentumPhase?{momentum_phase:momentumPhase}:{}),
      ...(!dx&&!dy?{stop_x:true,stop_y:true}:{})})
  }
  function stopMomentum(transmit=true) {
    if (momentumFrame) cancelFrame(momentumFrame)
    momentumFrame=0
    if (momentum && transmit && ready()) wheel(0,0,"ended","ended")
    momentum=false
  }
  function finishScroll(inertia=false, cancelled=false) {
    if (!scroll) return
    scroll=false
    if (!ready()) return
    wheel(0,0,cancelled?"cancelled":"ended")
    const end=history.at(-1),start=history[0]
    if (!inertia || !start || !end || end.t-start.t<8 || now()-end.t>80) return
    let vx=(end.x-start.x)/(end.t-start.t),vy=(end.y-start.y)/(end.t-start.t)
    const speed=Math.hypot(vx,vy)
    if (speed<0.08) return
    // Bound accidental outliers without altering the direction of a diagonal.
    if (speed>10) {vx*=10/speed;vy*=10/speed}
    let previous=now(),elapsed=0,first=true
    momentum=true
    function step(time) {
      momentumFrame=0
      if (!momentum) return
      if (!ready()) {momentum=false;return}
      const dt=time-previous
      // A backgrounded/throttled frame must never replay a large stale fling.
      if (dt>80 || dt<0) {stopMomentum();return}
      previous=time;elapsed+=dt
      const decay=DECAY**dt, integral=(decay-1)/Math.log(DECAY)
      wheel(vx*integral,vy*integral,"ended",first?"began":"changed")
      first=false;vx*=decay;vy*=decay
      if (Math.hypot(vx,vy)<MIN_SPEED || elapsed>=1800) {stopMomentum();return}
      momentumFrame=requestFrame(step)
    }
    momentumFrame=requestFrame(step)
  }
  function sample(point) {
    const t=now();history.push({...point,t})
    while (history.length>2 && t-history[0].t>90) history.shift()
  }
  function touch(phase, points) {
    if (!ready()) {reset(false);return}
    if (phase==="cancel") {reset(true);return}
    const time=now(),count=points.length
    if (!gesture) {
      if (phase!=="start" || !count) return
      const interrupted=momentum
      stopMomentum()
      const p=center(points)
      const edge=count===1 && !!points[0].edge
      const second=!interrupted && count===1 && !edge && lastTap && time-lastTap.t<=DOUBLE_TIME && distance(p,lastTap)<=DOUBLE_SLOP
      gesture={count,max:count,start:time,lastTime:time,origin:p,previous:p,scrollPos:{x:0,y:0},points:new Map(points.map(p=>[p.id,p])),mode:edge?"edge":count===1?"point":count===2?"two":"three",edge,tap:!interrupted,moved:false,second:!!second}
      lastTap=null
      history=[]
      if (edge) sample(gesture.scrollPos)
      else if (count===2) sample(p)
      if (count>3) {gesture.mode="blocked";gesture.tap=false}
      return
    }
    const g=gesture
    if (!count) {
      flush()
      if (held) button(false)
      if (g.mode==="scroll" || g.mode==="scroll-ending") finishScroll(true)
      if (g.tap && !g.moved && time-g.start<=TAP_TIME) {
        if (g.max===1) {click(1);if(!g.second) lastTap={...g.previous,t:time}}
        else if (g.max===2) click(2)
      }
      gesture=null;history=[]
      return
    }
    if (count!==g.count || points.some(p=>!g.points.has(p.id))) {
      flush()
      if (held) button(false)
      if (count<g.count) {
        if (g.mode==="scroll") g.mode="scroll-ending"
        else if (g.mode==="two") g.mode="two-ending"
        else if (g.max>=3) {g.mode="blocked";g.tap=false}
        // Do not turn the remaining finger into a pointer or a left click.
      } else {
        finishScroll(false,true)
        g.tap &&= time-g.start<=120 && !g.moved && !g.second
        g.max=Math.max(g.max,count)
        g.edge=false
        g.mode=count===2 && g.max===2?"two":count===3?"three":"blocked"
        if (count>3) g.tap=false
        g.origin=center(points);g.previous=g.origin;g.lastTime=time;g.scrollPos={x:0,y:0};history=[]
        if (count===2) sample(g.origin)
      }
      g.count=count;g.points=new Map(points.map(p=>[p.id,p]));return
    }
    if (phase!=="move" || ["blocked","scroll-ending","two-ending"].includes(g.mode)) return
    const p=center(points), dt=Math.max(4,time-(g.lastTime||time))
    g.lastTime=time
    // Individual finger travel also invalidates a tap (a pinch has no centroid
    // motion). Unsupported gestures must not leak a spurious secondary click.
    for (const point of points) if (distance(point,g.points.get(point.id))>TAP_SLOP) g.moved=true
    if (distance(p,g.origin)>TAP_SLOP) g.moved=true
    if (g.mode==="point") {
      if (g.second && g.moved && !held) button(true)
      move(p.x-g.previous.x,p.y-g.previous.y)
    }
    else if (g.mode==="edge" || (g.mode==="scroll" && g.edge)) {
      if (g.mode==="edge" && distance(p,g.origin)>TAP_SLOP && Math.abs(p.x-g.origin.x)>Math.abs(p.y-g.origin.y)) {
        g.mode="point";g.edge=false;history=[]
        if (g.second && !held) button(true)
        move(p.x-g.origin.x,p.y-g.origin.y)
      } else if (g.mode==="scroll" || Math.abs(p.y-g.origin.y)>TAP_SLOP) {
        const from=g.mode==="edge"?g.origin:g.previous
        const rawDy=p.y-from.y, gain=scrollGain(Math.abs(rawDy),dt), dy=rawDy*gain
        g.scrollPos={x:0,y:g.scrollPos.y+dy}
        sample(g.scrollPos)
        wheel(0,dy,scroll?"changed":"began")
        scroll=true;g.mode="scroll";g.tap=false
      } else {
        sample(g.scrollPos)
      }
    }
    else if (g.mode==="two" || g.mode==="scroll") {
      sample(p)
      if (g.mode==="scroll" || distance(p,g.origin)>TAP_SLOP) {
        const from=g.mode==="two"?g.origin:g.previous
        wheel(p.x-from.x,p.y-from.y,scroll?"changed":"began")
        scroll=true;g.mode="scroll";g.tap=false
      }
    } else if (g.mode==="three" || g.mode==="drag") {
      if (g.mode==="drag" || distance(p,g.origin)>TAP_SLOP) {
        const from=g.mode==="three"?g.origin:g.previous
        if (!held) button(true)
        move(p.x-from.x,p.y-from.y);g.mode="drag";g.tap=false
      }
    }
    g.previous=p
  }
  function reset(transmit=true) {
    if (motionFrame) cancelFrame(motionFrame)
    motionFrame=0;motionX=motionY=0
    if (held && transmit && ready()) send({t:"button",button:1,down:false})
    held=false
    if (transmit) finishScroll(false,true)
    scroll=false;stopMomentum(transmit)
    gesture=null;lastTap=null;history=[]
  }
  return {
    touch, flush, reset,
    handoff() {
      // The hub has released the old target. Keep single-finger motion alive,
      // but never continue held buttons or a fling on a different device.
      held=false;scroll=false;stopMomentum(false);lastTap=null;history=[]
      if (motionFrame) cancelFrame(motionFrame)
      motionFrame=0;motionX=motionY=0
      if (gesture) {gesture.tap=false;if(gesture.mode!=="point") gesture.mode="blocked"}
    },
  }
}

export function createTrackpad(pad, {ready,send}) {
  const gestures=createTrackpadGestures({ready,send})
  const mouse=new Map()
  let wheelTimer=null,wheeling=false
  const isEdge=clientX=>{
    const rect=pad.getBoundingClientRect()
    const style=getComputedStyle(pad)
    const insetLeft=parseFloat(style.paddingLeft)||0
    const insetRight=parseFloat(style.paddingRight)||0
    const width=Math.min(EDGE_SCROLL_WIDTH,rect.width*0.24)
    return rect.width>0 && (clientX-rect.left<=width+insetLeft || rect.right-clientX<=width+insetRight)
  }
  const touches=event=>[...event.targetTouches].map(p=>({id:p.identifier,x:p.clientX,y:p.clientY,edge:isEdge(p.clientX)}))
  for (const [name,phase] of [["touchstart","start"],["touchmove","move"],["touchend","end"],["touchcancel","cancel"]]) {
    pad.addEventListener(name,event=>{
      event.preventDefault()
      if (phase==="start") endWheel()
      gestures.touch(phase,touches(event))
    },{passive:false})
  }
  pad.addEventListener("contextmenu",e=>e.preventDefault())
  pad.addEventListener("pointerdown",event=>{
    event.preventDefault() // Keep the software keyboard focused.
    if (event.pointerType==="touch" || !ready()) return
    gestures.reset();pad.setPointerCapture(event.pointerId)
    const button=({0:1,2:2,1:3})[event.button]||1
    mouse.set(event.pointerId,{x:event.clientX,y:event.clientY,button})
    send({t:"button",button,down:true})
  })
  pad.addEventListener("pointermove",event=>{
    const p=mouse.get(event.pointerId)
    if (!p || !ready()) return
    send({t:"motion",dx:(event.clientX-p.x)*2,dy:(event.clientY-p.y)*2})
    p.x=event.clientX;p.y=event.clientY
  })
  function mouseEnd(event) {
    const p=mouse.get(event.pointerId)
    if (!p) return
    mouse.delete(event.pointerId)
    if (ready()) send({t:"button",button:p.button,down:false})
    if (pad.hasPointerCapture(event.pointerId)) pad.releasePointerCapture(event.pointerId)
  }
  for (const name of ["pointerup","pointercancel","lostpointercapture"]) pad.addEventListener(name,mouseEnd)
  function endWheel(transmit=true) {
    clearTimeout(wheelTimer);wheelTimer=null
    if (wheeling && transmit && ready()) send({t:"wheel",dx:0,dy:0,source:"continuous",phase:"ended",stop_x:true,stop_y:true})
    wheeling=false
  }
  pad.addEventListener("wheel",event=>{
    if (!ready()) return
    event.preventDefault();gestures.reset()
    const unit=event.deltaMode===1?16:event.deltaMode===2?pad.clientHeight:1
    send({t:"wheel",dx:-event.deltaX*unit*12,dy:-event.deltaY*unit*12,source:"continuous",phase:wheeling?"changed":"began"})
    wheeling=true;clearTimeout(wheelTimer);wheelTimer=setTimeout(endWheel,100)
    // Browser/OS wheel streams already include inertia; never add another fling.
  },{passive:false})
  return {
    reset(transmit=true) {
      gestures.reset(transmit);endWheel(transmit)
      const pointers=[...mouse];mouse.clear()
      for (const [id,p] of pointers) {
        if (transmit && ready()) send({t:"button",button:p.button,down:false})
        if (pad.hasPointerCapture(id)) pad.releasePointerCapture(id)
      }
    },
    handoff() {gestures.handoff();endWheel(false);mouse.clear()},
  }
}
