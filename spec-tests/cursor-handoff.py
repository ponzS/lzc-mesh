#!/usr/bin/env python3
"""Repeated native Windows/Mac entry and edge return; private OS fixtures only.
Uses native-input-matrix.py's CLI and safety/ownership checks. Synthetic input
is not physical takeover certification. Layout is never changed.
"""
import json,os,runpy,time
from pathlib import Path

module=runpy.run_path(str(Path(__file__).with_name('native-input-matrix.py')),run_name='cursor_matrix')
Matrix,args=module['Matrix'],module['args']
m=Matrix();results=[]

def observe(platform,expected,label):
    state=m.status(platform)
    actual=(state['cursor_x'],state['cursor_y'])
    assert state['focused'],f'{label}: private fixture lost focus'
    (args.artifacts/f'{len(results):02d}-{platform}-{label}.json').write_text(json.dumps(state,indent=2))
    assert max(abs(a-b) for a,b in zip(actual,expected))<=3,f'{label}: {platform} cursor {actual}, expected {expected}'
    assert state['cursor_visible'],f'{label}: {platform} cursor invisible at {actual}'
    return actual

def check_paused_idle():
    state=m.state()
    assert state['active'] is None and not state['settings']['input_enabled'],'Idle maintenance pause required; no input sent'

try:
    check_paused_idle()
    for platform in ('windows','macos'):
        assert m.command(platform,'reset')['focused'],'Private fixture did not focus'
        m.command(platform,'park')
    m.viewer.send(dict(t='settings',input_enabled=True,transport_mode=args.transport_mode))
    m.wait_state(lambda s:s['settings']['input_enabled'],'Test input did not enable');time.sleep(4)
    for i in range(int(os.environ.get('MESH_CURSOR_ROUNDS','6'))):
        source,target=('windows','macos') if i%2==0 else ('macos','windows')
        assert m.state()['active'] is None,'User session active; test refused'
        assert m.command(source,'reset')['focused'] and m.command(target,'reset')['focused'],'Private fixture unavailable'
        a,b=m.devices[source],m.devices[target];direction=m.neighbour(a,b)
        assert direction in (2,3),'Expected the currently connected vertical Windows/Mac layout'
        low=max(a['x'],b['x']);high=min(a['x']+a['w'],b['x']+b['w'])
        x=low+(high-low)*[.12,.5,.88][i%3];pos=(x-a['x'])/a['w']
        # Windows edge message has four decimal place position precision.
        if source=='windows':pos=int(pos*10000)/10000;x=a['x']+pos*a['w']
        m.source=source
        m.command(source,'edge',direction=direction,position=pos,approach_edge=True)
        state=m.wait_state(lambda s:s['active'] is not None,'Native edge did not start')
        assert state['active']['source']==a['id'] and state['active']['target']==b['id'],'Unexpected session; no takeover attempted'
        m.owned=state['active']['control_id']
        m.check_owned();time.sleep(.25)
        entry_y=b['y']+2 if direction==3 else b['y']+b['h']-2
        expected=((x-b['x'])*(b['w']-1)/b['w'],(entry_y-b['y'])*(b['h']-1)/b['h']) if target=='macos' else (x-b['x'],entry_y-b['y'])
        observed=observe(target,expected,'entry')
        if source=='windows':
            m.check_owned();m.command(source,'stale_capture_fault');time.sleep(.1);m.check_owned()
        # A small intentional move away from the shared edge must be a small
        # move, not a jump to a corner. Then cross back without an emergency key.
        delta=24 if direction==3 else -24
        m.motion(source,0,delta);time.sleep(.15);m.check_owned()
        moved=observe(target,(expected[0],expected[1]+delta),'motion')
        m.motion(source,0,-delta*2)
        m.wait_state(lambda s:s['active'] is None,'Native edge return did not end test session')
        m.owned=None;time.sleep(.2)
        return_y=2 if direction==2 else a['h']-2
        returned=((x-a['x'])*(a['w']-1)/a['w'],return_y*(a['h']-1)/a['h']) if source=='macos' else (x-a['x'],return_y)
        actual_return=observe(source,returned,'return')
        results.append(dict(source=source,target=target,entry=observed,moved=moved,returned=actual_return,status='passed'))
        print(f'PASS {i+1}: {source}->{target}: visible edge entry, small motion, correct edge return',flush=True)
        time.sleep(.5)
finally:
    active=m.state()['active']
    if active and m.owned==active['control_id']:m.stop_owned()
    if m.state()['active'] is None:
        m.viewer.send(dict(t='settings',**m.original_settings))
        m.wait_state(lambda s:s['settings']==m.original_settings,'Settings restore failed')
    (args.artifacts/'cursor-results.json').write_text(json.dumps(dict(results=results,evidence='synthetic OS input on installed native clients, not physical takeover certification'),indent=2))
    m.viewer.close()
