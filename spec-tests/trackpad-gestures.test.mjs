import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createTrackpadGestures } from '../lzc-mesh-lpk/web/controller-trackpad.js'

function fixture() {
  let time=1000,serial=0,active=true
  const frames=new Map(),messages=[]
  const gestures=createTrackpadGestures({
    ready:()=>active,send:m=>messages.push({...m}),now:()=>time,
    requestFrame:fn=>{frames.set(++serial,fn);return serial},cancelFrame:id=>frames.delete(id),
  })
  const touch=(phase,points,dt=16)=>{time+=dt;gestures.touch(phase,points.map(([id,x,y,edge])=>({id,x,y,edge})))}
  const tick=(dt=16)=>{time+=dt;const pending=[...frames.values()];frames.clear();for(const fn of pending)fn(time)}
  const buttons=()=>messages.filter(m=>m.t==='button')
  const wheels=()=>messages.filter(m=>m.t==='wheel')
  return {gestures,messages,touch,tick,buttons,wheels,frames,setActive:value=>active=value}
}
const one=(x=100,y=100,edge=false)=>[[0,x,y,edge]]
const two=(x=100,y=100)=>[[0,x,y],[1,x+60,y]]
const three=(x=100,y=100)=>[[0,x,y],[1,x+60,y],[2,x+120,y]]
function tap(f,points=one()) {f.touch('start',points);f.touch('end',[])}
function fling(f) {
  f.touch('start',two());f.touch('move',two(100,120));f.touch('move',two(100,145));f.touch('end',[])
}

test('one finger points relatively; a long hold is not a secondary click',()=>{
  const f=fixture();f.touch('start',one());f.touch('move',one(120,110));f.tick();f.touch('end',[])
  assert.deepEqual(f.messages,[{t:'motion',dx:40,dy:20}])
  f.touch('start',one(),500);f.touch('end',[],700)
  assert.deepEqual(f.buttons(),[])
})
test('single tap and double tap make ordinary clicks, never a stray held button',()=>{
  const f=fixture();tap(f);tap(f)
  assert.deepEqual(f.buttons().map(m=>[m.button,m.down]),[[1,true],[1,false],[1,true],[1,false]])
  const g=fixture();tap(g);g.touch('start',one(250,250));g.touch('move',one(280,250));g.touch('end',[])
  assert.equal(g.buttons().length,2,'distant next touch must not drag')
})
test('tap then slide on second touch drags until lift, no extra click',()=>{
  const f=fixture();tap(f);f.touch('start',one());f.touch('move',one(130,100));f.touch('end',[])
  assert.deepEqual(f.buttons().map(m=>m.down),[true,false,true,false])
  assert.equal(f.messages.filter(m=>m.t==='motion').length,1)
  assert.equal(f.messages.at(-1).down,false)
})
test('two-finger tap produces exactly one right click even with staggered arrival/lift',()=>{
  const f=fixture();f.touch('start',one());f.touch('start',two());f.touch('end',one());f.touch('end',[])
  assert.deepEqual(f.buttons().map(m=>[m.button,m.down]),[[2,true],[2,false]])
  assert.equal(f.messages.filter(m=>m.t==='motion').length,0)
})
test('natural diagonal scroll has exact centroid distance without pointer jumps or click leakage',()=>{
  const f=fixture();f.touch('start',one());f.touch('start',two())
  f.touch('move',two(110,130));f.touch('end',one(110,130));f.touch('move',one(160,200));f.touch('end',[],200)
  const changed=f.wheels().filter(m=>m.dx||m.dy)
  assert.equal(changed.reduce((n,m)=>n+m.dx,0),120)
  assert.equal(changed.reduce((n,m)=>n+m.dy,0),360)
  assert.equal(f.buttons().length,0)
  assert.equal(f.messages.filter(m=>m.t==='motion').length,0)
  assert.equal(f.frames.size,0,'stationary lift must not fling')
})
test('three fingers drag with left down before motion and up on first finger lift',()=>{
  const f=fixture();f.touch('start',three());f.touch('move',three(120,115));f.touch('end',two(120,115));f.touch('move',two(150,130));f.touch('end',[])
  assert.deepEqual(f.buttons().map(m=>m.down),[true,false])
  assert.equal(f.messages[0].t,'button')
  assert.deepEqual(f.messages[1],{t:'motion',dx:40,dy:30})
  assert.equal(f.wheels().length,0)
})
test('finger-count changes, cancelled touch and unsupported pinch do not create clicks',()=>{
  const f=fixture();f.touch('start',one());f.touch('cancel',[]);assert.equal(f.buttons().length,0)
  f.touch('start',three());f.touch('end',[]);assert.equal(f.buttons().length,0)
  f.touch('start',two());f.touch('move',[[0,80,100],[1,180,100]]);f.touch('end',[])
  assert.equal(f.buttons().length,0)
  assert.equal(f.wheels().length,0)
})
test('fling has one decaying continuous momentum stream and a final stop',()=>{
  const f=fixture();fling(f)
  for(let i=0;i<110&&f.frames.size;i++)f.tick()
  const momentum=f.wheels().filter(m=>m.momentum_phase)
  assert.equal(momentum[0].momentum_phase,'began')
  assert.equal(momentum.at(-1).momentum_phase,'ended')
  assert.ok(momentum[1].dy>0 && momentum[1].dy<momentum[0].dy)
  assert.ok(momentum.every(m=>m.source==='continuous'))
  assert.equal(momentum.filter(m=>m.momentum_phase==='began').length,1)
  assert.equal(f.frames.size,0)
})
test('new touch immediately interrupts momentum without interpreting the stop tap as a click',()=>{
  const f=fixture();fling(f);f.tick();f.touch('start',one());const n=f.messages.length
  f.tick();f.touch('end',[])
  assert.equal(f.messages.length,n)
  assert.equal(f.buttons().length,0)
  assert.equal(f.wheels().at(-1).momentum_phase,'ended')
})
test('cancelled or throttled scroll never launches/replays inertia',()=>{
  const f=fixture();f.touch('start',two());f.touch('move',two(100,130));f.touch('cancel',[])
  assert.equal(f.frames.size,0);assert.equal(f.wheels().at(-1).phase,'cancelled')
  fling(f);f.tick(200)
  assert.equal(f.frames.size,0)
  assert.equal(f.wheels().filter(m=>m.momentum_phase&&m.dy).length,0)
})
test('reset releases a drag and clears queued motion; handoff never re-holds it on the new target',()=>{
  const f=fixture();f.touch('start',three());f.touch('move',three(120,100));f.gestures.reset()
  assert.deepEqual(f.buttons().map(m=>m.down),[true,false]);assert.equal(f.frames.size,0)
  f.touch('start',three());f.touch('move',three(120,100));f.gestures.handoff();const n=f.messages.length
  f.touch('move',three(150,100));f.touch('end',[]);f.tick();assert.equal(f.messages.length,n)
})
test('single-finger pointing survives handoff and control loss drops pending frames',()=>{
  const f=fixture();f.touch('start',one());f.touch('move',one(120,100));f.gestures.flush();f.gestures.handoff()
  f.touch('move',one(130,100));f.tick();assert.equal(f.messages.at(-1).dx,20)
  f.touch('move',one(140,100));const n=f.messages.length;f.setActive(false);f.gestures.reset(false);f.tick()
  assert.equal(f.messages.length,n)
})
test('right-edge single-finger slide scrolls vertically with natural direction, acceleration and inertia',()=>{
  const f=fixture()
  f.touch('start',one(375,100,true))
  f.touch('move',one(365,120,true))
  f.touch('move',one(360,150,false))
  f.touch('end',[])
  assert.equal(f.messages.filter(m=>m.t==='motion').length,0)
  assert.equal(f.buttons().length,0)
  const changed=f.wheels().filter(m=>!m.momentum_phase && (m.dx||m.dy))
  assert.equal(changed.reduce((n,m)=>n+m.dx,0),0)
  assert.ok(changed.reduce((n,m)=>n+m.dy,0)>1500)
  for(let i=0;i<110&&f.frames.size;i++)f.tick()
  const momentum=f.wheels().filter(m=>m.momentum_phase)
  assert.equal(momentum[0].momentum_phase,'began')
  assert.equal(momentum.at(-1).momentum_phase,'ended')
  assert.ok(momentum.every(m=>m.dx===0))
})
