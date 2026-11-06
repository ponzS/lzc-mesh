#!/usr/bin/env python3
"""Private real hub + Chromium/CDP touch integration, not phone/OS injection acceptance.
Run: python spec-tests/mobile-controller.py --hub-bin <binary> --artifacts <dir>
Uses only disposable identities, test text and isolated browser/data directories.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import time
from types import SimpleNamespace

spec=importlib.util.spec_from_file_location('lifecycle',Path(__file__).with_name('device-lifecycle.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def run(args):
 scene=m.Scene(args);checks=[]
 def passed(name):checks.append(name);print('PASS: '+name,flush=True)
 try:
  scene.start();a=scene.connect(device='linux');b=scene.connect(device='win');scene.viewer=scene.connect()
  scene.viewer.send(dict(t='layout',id='linux',x=0,y=0));scene.viewer.send(dict(t='layout',id='win',x=1000,y=0))
  scene.state(lambda s:len(s['devices'])==2 and next(d for d in s['devices'] if d['id']=='win')['x']==1000)
  scene.browser=m.Browser(f'http://127.0.0.1:{scene.port}/',args.artifacts);browser=scene.browser
  m.wait(lambda:browser.evaluate("document.querySelectorAll('.device-switch').length===2"),'Homepage failed')
  m.require(browser.evaluate("document.querySelector('.controller-entry').getAttribute('href')==='controller.html'"),'Homepage controller entry missing')
  browser.call('viewport',width=390,height=844,mobile=True)
  browser.evaluate("document.querySelector('.controller-entry').click()")
  m.wait(lambda:browser.evaluate("document.querySelectorAll('[data-mobile-key]').length>15 && !document.getElementById('control-toggle').disabled"),'Controller did not join')
  def evaluate(js):return browser.evaluate(js)
  def start(target='linux'):
   evaluate(f"document.getElementById('target-device').value={json.dumps(target)};if(document.getElementById('control-toggle').textContent==='开始控制')document.getElementById('control-toggle').click();else document.getElementById('target-device').dispatchEvent(new Event('change'))")
   return scene.state(lambda s:s['active'] and s['active']['source'].startswith('web:') and s['active']['target']==target)['active']
  def stop():
   evaluate("document.getElementById('control-toggle').click()")
   scene.state(lambda s:s['active'] is None)
   m.wait(lambda:evaluate("document.getElementById('control-toggle').textContent==='开始控制'"),'Stop not reflected')
  def touch(kind,points):browser.call('touch',type=kind,points=[dict(x=x,y=y,id=i) for i,x,y in points])
  def tap(x=140,y=180):touch('touchStart',[(0,x,y)]);touch('touchEnd',[])
  def received(ws,predicate):
   messages=scene.drain(ws);m.require(predicate(messages),f'Expected input missing: {messages}');return messages
  # Current main auto-starts eligible targets; do not click its active toggle
  # as the previous explicit-start fixture did (that would now stop control).
  scene.state(lambda s:s['active'] and s['active']['source'].startswith('web:'))
  stop()
  evaluate("window.dispatchEvent(new Event('focus'))")
  time.sleep(.15)
  scene.viewer.send(dict(t='hello',platform='web'))
  m.require(scene.state()['active'] is None,'Manual stop was undone by focus/render')
  passed('eligible target auto-starts on entry; manual stop survives focus/render')
  active=start();m.wait(lambda:evaluate("document.getElementById('current-device').textContent==='linux'"),'Target not shown')
  scene.viewer.send(dict(t='hello',platform='web'))
  m.require(len(scene.state(lambda s:s['active'])['devices'])==2,'Browser registered a persistent screen')
  scene.drain(a);tap()
  received(a,lambda ms:any(x['t']=='button' and x['button']==1 and x['down'] for x in ms) and any(x['t']=='button' and not x['down'] for x in ms))
  time.sleep(.35);scene.drain(a)
  touch('touchStart',[(0,100,180)]);touch('touchMove',[(0,140,210)]);touch('touchEnd',[])
  received(a,lambda ms:any(x['t']=='move' for x in ms) and not any(x['t']=='button' and x['down'] for x in ms))
  m.require(evaluate("!document.getElementById('drag-toggle')"),'Drag button must be removed')
  time.sleep(.35);touch('touchStart',[(0,120,180)]);time.sleep(.6);touch('touchEnd',[])
  received(a,lambda ms:not any(x['t']=='button' for x in ms))
  touch('touchStart',[(0,120,180),(1,180,180)]);touch('touchEnd',[])
  received(a,lambda ms:sum(x['t']=='button' and x['button']==2 and x['down'] for x in ms)==1 and not any(x['t']=='button' and x['button']==1 for x in ms))
  touch('touchStart',[(0,120,180),(1,180,180)]);touch('touchMove',[(0,130,210),(1,190,210)]);time.sleep(.15);touch('touchEnd',[])
  received(a,lambda ms:any(x['t']=='wheel' and x['source']=='continuous' and x['dx']>0 and x['dy']>0 for x in ms) and any(x['t']=='wheel' and x.get('stop_y') for x in ms) and not any(x['t']=='button' for x in ms))
  passed('real CDP touch: relative move, tap, two-finger right click, natural diagonal scroll; no long-press right click or drag button')
  time.sleep(.35)
  touch('touchStart',[(0,80,180),(1,140,180),(2,200,180)]);touch('touchMove',[(0,100,190),(1,160,190),(2,220,190)]);touch('touchEnd',[])
  received(a,lambda ms:any(x['t']=='button' and x['button']==1 and x['down'] for x in ms) and any(x['t']=='move' for x in ms) and any(x['t']=='button' and not x['down'] for x in ms) and not any(x['t']=='wheel' for x in ms))
  tap();touch('touchStart',[(0,140,180)]);touch('touchMove',[(0,160,180)]);touch('touchEnd',[])
  received(a,lambda ms:sum(x['t']=='button' and x['down'] for x in ms)==2 and sum(x['t']=='button' and not x['down'] for x in ms)==2)
  # Back-to-back taps must remain a double click, not leak an extra drag click.
  time.sleep(.35);tap();tap()
  received(a,lambda ms:sum(x['t']=='button' and x['down'] for x in ms)==2 and sum(x['t']=='button' and not x['down'] for x in ms)==2)
  passed('real CDP touch: three-finger drag, tap-then-slide drag and ordinary double tap, all release on lift')
  time.sleep(.35)
  touch('touchStart',[(0,120,180),(1,180,180)])
  for y in [195,215,240]:time.sleep(.02);touch('touchMove',[(0,120,y),(1,180,y)])
  touch('touchEnd',[])
  deadline=time.monotonic()+3
  while True:
   message=a.read(3)
   if message['t']=='wheel' and message.get('momentum_phase') in ('began','changed') and message['dy']>0:break
   m.require(time.monotonic()<deadline,'Momentum did not begin')
  touch('touchStart',[(0,140,180)])
  received(a,lambda ms:any(x['t']=='wheel' and x.get('momentum_phase')=='ended' for x in ms))
  touch('touchEnd',[])
  received(a,lambda ms:not any(x['t'] in ('wheel','button') for x in ms))
  passed('real CDP touch: one continuous fling, new touch cancels momentum immediately without a stray click')
  for x in [15,375]:
   scene.drain(a)
   touch('touchStart',[(0,x,160)]);touch('touchMove',[(0,x,200)]);touch('touchEnd',[])
   received(a,lambda ms:any(x['t']=='wheel' and x['dy']>0 and x['dx']==0 for x in ms) and not any(x['t'] in ('move','button') for x in ms))
   touch('touchStart',[(0,140,180)]);touch('touchEnd',[]);scene.drain(a)
  passed('real CDP touch: left/right edge single-finger natural scrolling without pointer or click leakage')
  def tap_control(selector):
   point=evaluate(f"(()=>{{const r=document.querySelector({json.dumps(selector)}).getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]}})()")
   tap(*point)
  tap_control('#mobile-keyboard')
  m.require(evaluate("document.activeElement.id==='mobile-input'"),'Keyboard input not focused')
  tap_control('[data-mobile-key=ControlLeft]')
  m.require(evaluate("document.activeElement.id==='mobile-input'"),'Shortcut touch stole keyboard focus')
  browser.call('text',text='c')
  received(a,lambda ms:any(x['t']=='key' and x['code']==29 and x['down'] for x in ms) and any(x['t']=='key' and x['code']==46 and x['down'] for x in ms))
  m.require(evaluate("document.querySelector('[data-mobile-key=ControlLeft]').getAttribute('aria-pressed')==='false'"),'Sticky modifier not consumed')
  browser.call('text',text='hello')
  received(a,lambda ms:sum(x['t']=='key' and x['down'] for x in ms)==5)
  browser.call('key',key='Backspace',code='Backspace',vk=8)
  received(a,lambda ms:sum(x['t']=='key' and x['code']==14 and x['down'] for x in ms)==1)
  browser.call('text',text='你好')
  received(a,lambda ms:any(x['t']=='clipboard' and x['text']=='你好' for x in ms) and not any(x['t']=='key' for x in ms))
  m.require(evaluate("document.getElementById('input-notice').textContent.includes('Ctrl+V')"),'Manual paste guidance missing')
  passed('system-keyboard focus, ASCII, one-shot sticky chord, delete, explicit Unicode clipboard staging')
  # Start away from the new edge-scroll zone; keep this a pointer handoff.
  touch('touchStart',[(0,100,180)]);touch('touchMove',[(0,420,180)])
  scene.state(lambda s:s['active'] and s['active']['target']=='win')
  m.wait(lambda:evaluate("document.getElementById('current-device').textContent==='win'"),'Cross-screen target not reflected')
  m.require(evaluate("document.activeElement.id==='mobile-input'"),'Handoff hid system keyboard')
  scene.drain(b);touch('touchMove',[(0,430,180)]);touch('touchEnd',[])
  received(b,lambda ms:any(x['t']=='move' for x in ms))
  passed('automatic cross-screen handoff continues same finger gesture and preserves keyboard focus')
  # Host values override env(), and explicit zero means already-inset content.
  evaluate("for(const [k,v] of Object.entries({top:24,right:12,bottom:34,left:8}))document.documentElement.style.setProperty('--lzc-safe-area-'+k,v+'px')")
  def geometry():return evaluate("JSON.stringify({pad:getComputedStyle(document.getElementById('touchpad')).paddingTop,bar:getComputedStyle(document.getElementById('mobile-shortcuts')).paddingBottom,bottom:document.getElementById('mobile-shortcuts').getBoundingClientRect().bottom,height:visualViewport.height,width:document.documentElement.scrollWidth,inner:innerWidth})")
  g=json.loads(geometry());m.require(g['pad']=='24px' and g['bar']=='42px',f'Host safe areas not consumed: {g}')
  m.require(g['bottom']<=g['height']+1 and g['width']<=g['inner'],'Mobile viewport overflow')
  browser.call('shot',path=str(args.artifacts/'controller-mobile.png'))
  browser.call('color',scheme='dark');browser.call('shot',path=str(args.artifacts/'controller-dark.png'))
  browser.call('viewport',width=844,height=390,mobile=True)
  m.wait(lambda:(lambda g:abs(g['bottom']-g['height'])<=1)(json.loads(geometry())),'Landscape toolbar not visible')
  browser.call('shot',path=str(args.artifacts/'controller-landscape.png'))
  evaluate("for(const k of ['top','right','bottom','left'])document.documentElement.style.setProperty('--lzc-safe-area-'+k,'0px')")
  g=json.loads(geometry());m.require(g['pad']=='0px' and g['bar']=='8px','Zero host safe areas double padded')
  passed('portrait/landscape, light/dark, host safe-area precedence and explicit zero (not physical host/IME acceptance)')
  # Switching targets releases an actual three-finger drag; so does returning home.
  touch('touchStart',[(0,80,100),(1,140,100),(2,200,100)])
  touch('touchMove',[(0,90,110),(1,150,110),(2,210,110)])
  received(b,lambda ms:any(x['t']=='button' and x['down'] for x in ms))
  evaluate("document.getElementById('target-device').value='linux';document.getElementById('target-device').dispatchEvent(new Event('change'))")
  scene.state(lambda s:s['active'] and s['active']['target']=='linux')
  received(b,lambda ms:any(x['t']=='button' and not x['down'] for x in ms) and any(x['t']=='leave' for x in ms))
  touch('touchEnd',[])
  m.wait(lambda:evaluate("document.getElementById('current-device').textContent==='linux'"),'Manual target not shown')
  touch('touchStart',[(0,80,100),(1,140,100),(2,200,100)])
  touch('touchMove',[(0,90,110),(1,150,110),(2,210,110)])
  received(a,lambda ms:any(x['t']=='button' and x['down'] for x in ms))
  evaluate("document.getElementById('mobile-home').click()")
  scene.state(lambda s:s['active'] is None)
  received(a,lambda ms:any(x['t']=='button' and not x['down'] for x in ms))
  m.wait(lambda:evaluate("!!document.querySelector('.controller-entry')"),'Home navigation failed')
  passed('manual target switch and returning home release held mouse button')
  # A stopped/frozen browser cannot hold input indefinitely on a live TCP socket.
  raw=scene.connect();raw.send(dict(t='web_control',target='linux'))
  scene.state(lambda s:s['active'] and s['active']['target']=='linux')
  raw.send(dict(t='button',button=1,down=True));raw.send(dict(t='key',code=64,down=True));scene.drain(a)
  deadline=time.monotonic()+7
  scene.state(lambda s:s['active'] is None)
  m.require(time.monotonic()<deadline,'Browser lease failed to expire')
  received(a,lambda ms:any(x['t']=='key' and x['code']==64 and not x['down'] for x in ms) and any(x['t']=='button' and not x['down'] for x in ms))
  passed('real server watchdog expires unrenewed browser control and releases held input')
  result=dict(status='passed',checks=checks,evidence='Private real hub and Chromium touch/DOM input; physical phone safe-area/IME and native OS injection remain user acceptance.')
  (args.artifacts/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
 finally:scene.close()

if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--hub-bin',type=Path,required=True);parser.add_argument('--artifacts',type=Path,required=True)
 args=parser.parse_args();args.hub_bin=args.hub_bin.resolve();args.artifacts=args.artifacts.resolve();run(args)
