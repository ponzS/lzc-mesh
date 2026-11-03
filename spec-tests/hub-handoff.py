#!/usr/bin/env python3
"""Protocol component evidence through a real private hub; not OS/physical AC acceptance."""
import argparse,importlib.util,json,time
from pathlib import Path
from types import SimpleNamespace
spec=importlib.util.spec_from_file_location('lifecycle',Path(__file__).with_name('device-lifecycle.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
p=argparse.ArgumentParser();p.add_argument('--hub-bin',type=Path,required=True);p.add_argument('--artifacts',type=Path,required=True);args=p.parse_args()
results=[]
for source in ['mac','win','linux']:
 for target in ['mac','win','linux']:
  if source==target:continue
  scene=m.Scene(SimpleNamespace(hub_bin=args.hub_bin.resolve(),artifacts=args.artifacts/source/target))
  try:
   scene.start();a=scene.connect(device=source);b=scene.connect(device=target)
   third=next(x for x in ['mac','win','linux'] if x not in (source,target));c=scene.connect(device=third)
   scene.viewer=scene.connect();scene.state(lambda s:len(s['devices'])==3)
   a.send(dict(t='edge',dir='right',pos=.5))
   active=scene.state(lambda s:s['active'] and s['active']['source']==source)['active'];old=active['control_id']
   a.send(dict(t='key',code=64,down=True));a.send(dict(t='button',button=1,down=True));time.sleep(.05);scene.drain(b)
   for socket in [a,c]:socket.send(dict(t='takeover',control_id=old))
   scene.viewer.send(dict(t='hello',platform='web'));m.require(scene.state()['active']['control_id']==old,'Non-receiver canceled control')
   b.send(dict(t='takeover',control_id=old));scene.state(lambda s:s['active'] is None)
   released=scene.drain(b)
   m.require(any(x['t']=='key' and not x['down'] for x in released),'Key not released')
   m.require(any(x['t']=='button' and not x['down'] for x in released),'Button not released')
   b.send(dict(t='edge',dir='left',pos=.5));fresh=scene.state(lambda s:s['active'] and s['active']['source']==target)['active']['control_id']
   m.require(fresh!=old,'Control ID was reused')
   a.send(dict(t='takeover',control_id=old));time.sleep(.03);scene.viewer.send(dict(t='hello',platform='web'))
   m.require(scene.state()['active']['control_id']==fresh,'Stale takeover canceled a new session')
   foreign=scene.connect(user='other-fixture',device=source);foreign.send(dict(t='takeover',control_id=fresh));time.sleep(.03)
   scene.viewer.send(dict(t='hello',platform='web'));m.require(scene.state()['active']['control_id']==fresh,'Cross-account takeover affected owner')
   scene.drain(a);b.send(dict(t='key',code=64,down=True));b.send(dict(t='key',code=64,down=False));time.sleep(.05)
   m.require(sum(x['t']=='key' for x in scene.drain(a))==2,'Reverse keyboard was not routed')
   b.send(dict(t='release'));scene.state(lambda s:s['active'] is None)
   results.append(dict(source=source,target=target,status='passed'))
   print(f'PASS: {source}->{target}: scoped takeover, held release, reverse keys, stale/account isolation',flush=True)
  finally:scene.close()
args.artifacts.mkdir(parents=True,exist_ok=True)
(args.artifacts/'results.json').write_text(json.dumps(dict(evidence='private real-hub protocol component; not physical/native OS acceptance',results=results),indent=2))
