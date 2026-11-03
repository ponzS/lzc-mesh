#!/usr/bin/env python3
"""Installed native clients + isolated OS windows, six source/target directions.
Uses synthetic OS input, not physical hardware certification. Never stores text,
clipboard/file frames or tokens. Layout is read-only; stop on unexpected control.
Observers must already be running in the interactive user sessions.
"""
import argparse,json,math,os,re,subprocess,sys,time
from collections import deque
from pathlib import Path
from hub_ws import HubSocket

p=argparse.ArgumentParser();p.add_argument('--ssh-helper',required=True);p.add_argument('--copy-helper',required=True);p.add_argument('--cookie-file',type=Path,required=True);p.add_argument('--hub',required=True);p.add_argument('--hub-address');p.add_argument('--before',type=Path,required=True);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--transport-mode',choices=['auto','direct'],default='auto');args=p.parse_args()
args.artifacts.mkdir(parents=True,exist_ok=True);os.chmod(args.artifacts,0o700)

class Matrix:
 def __init__(self):
  self.viewer=HubSocket(args.hub,args.cookie_file.read_text().strip(),args.hub_address);self.viewer.send(dict(t='hello',platform='web'));self.last=None;self.serial=int(time.monotonic()*1000)%1000000000;self.owned=None;self.source=None;self.results=[]
  self.before=json.loads(args.before.read_text());self.devices={d['platform']:d for d in self.before['devices']};self.original_settings=self.before['settings']
  self.local_dir=args.artifacts/'linux-fixture';self.local_dir.mkdir(exist_ok=True)
  pid=subprocess.check_output(['pgrep','-f','^'+re.escape(str(Path.home()/'.local/bin/lzc-mesh'))],text=True).split()[0];self.linux_env={}
  for item in Path(f'/proc/{pid}/environ').read_bytes().split(b'\0'):
   if b'=' in item:
    key,value=item.split(b'=',1);self.linux_env[key.decode()]=value.decode()
  active=json.loads(subprocess.check_output(['hyprctl','-j','activewindow'],env=self.linux_env,text=True));self.previous_linux_window=active.get('address','')
 def focus_linux_fixture(self):
  data=json.loads(subprocess.check_output(['hyprctl','-j','layers'],env=self.linux_env,text=True))
  fixtures=[layer for monitor in data.values() for layers in monitor.get('levels',{}).values() for layer in layers if layer.get('namespace')=='lzc-mesh-test-observer']
  assert len(fixtures)==1,'Expected exactly one private exclusive Linux test layer'
 def ssh(self,platform,command):return subprocess.check_output([args.ssh_helper,{'macos':'mac','windows':'win'}[platform],command],text=True,timeout=25)
 def state(self):
  self.viewer.send(dict(t='ping'))
  while True:
   value=self.viewer.read(10)
   if value['t']=='state':self.last=value
   if value['t']=='pong':return self.last
 def wait_state(self,predicate,message,timeout=12):
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   value=self.state()
   if predicate(value):return value
   time.sleep(.05)
  raise AssertionError(message)
 def status(self,platform):
  if platform=='linux':
   path=self.local_dir/'linux-observer.json';assert time.time()-path.stat().st_mtime<3,'Linux fixture heartbeat expired';return json.loads(path.read_text())
  if platform=='macos':text=self.ssh(platform,"python3 -c 'import json,time;from pathlib import Path;p=Path.home()/\"lzc-mesh-handoff-tests/macos-observer.json\";v=json.loads(p.read_text());v[\"_fixture_age\"]=time.time()-p.stat().st_mtime;print(json.dumps(v))'")
  else:text=self.ssh(platform,'$p=Join-Path $env:USERPROFILE "mesh-parity\\observer.json";$v=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8)|ConvertFrom-Json;$v|Add-Member -NotePropertyName _fixture_age -NotePropertyValue (([DateTime]::UtcNow-(Get-Item $p).LastWriteTimeUtc).TotalSeconds);$v|ConvertTo-Json -Depth 10 -Compress')
  value=json.loads(text);assert value['_fixture_age']<3,platform+' fixture heartbeat expired';return value
 def command(self,platform,op,**fields):
  if platform=='linux' and op in ('input','edge','park'):
   assert subprocess.check_output(['omarchy-shell','lock','isLocked'],env=self.linux_env,text=True,timeout=10).strip()=='false','Omarchy locked during test; no input sent'
  self.serial+=1;identifier=self.serial;payload=dict(id=identifier,op=op,**fields)
  if platform=='linux' and op=='reset':self.focus_linux_fixture()
  if platform=='macos' and op in ('input','edge'):
   temp=args.artifacts/'macos-input-command.json';temp.write_text(json.dumps(payload));temp.chmod(0o600)
   subprocess.run([args.copy_helper,'mac',str(temp),'R:lzc-mesh-handoff-tests/input-command.json'],check=True,stdout=subprocess.DEVNULL,timeout=25)
   self.ssh('macos','/usr/bin/swift "$HOME/lzc-mesh-handoff-input.swift" "$HOME/lzc-mesh-handoff-tests/input-command.json"')
   return dict(command=identifier)
  if platform=='linux':
   target=self.local_dir/'linux-observer.json.command';tmp=target.with_suffix('.tmp');tmp.write_text(json.dumps(payload));tmp.chmod(0o600);tmp.replace(target)
  else:
   temp=args.artifacts/'fixture-command.json';temp.write_text(json.dumps(payload));temp.chmod(0o600)
   remote='lzc-mesh-handoff-tests/macos-observer.json.command' if platform=='macos' else 'mesh-parity/observer.json.command'
   subprocess.run([args.copy_helper,'mac' if platform=='macos' else 'win',str(temp),'R:'+remote],check=True,stdout=subprocess.DEVNULL,timeout=25)
  if op=='quit':return {}
  end=time.monotonic()+15
  while time.monotonic()<end:
   value=self.status(platform)
   if value.get('command')==identifier:
    assert not value.get('error'),value['error']
    if op!='reset' or value.get('focused'):return value
   time.sleep(.05)
  raise AssertionError(platform+' observer command timeout')
 def check_owned(self):
  active=self.state()['active']
  assert active and active['control_id']==self.owned,'Unexpected control change; test stopped without taking it over'
 def input(self,source,events):
  self.check_owned();converted=[]
  mac_codes={64:97,42:56,29:59,56:58,1:53}
  for event in events:
   e=dict(event)
   if e['t']=='key':
    code=e['code']
    if source=='macos':e['code']=mac_codes[code]
    if source=='windows':e['scan']=code
   if e['t']=='motion' and source=='windows':e['absolute_source']=True
   if e['t']=='wheel':
    if source=='macos':e['dy']=int(e.get('dy',0)/120);e['dx']=int(e.get('dx',0)/120)
    if source=='windows':e['delta']=e.get('dy',0)
   converted.append(e)
  return self.command(source,'input',events=converted,allow_captured=True)
 def motion(self,source,dx,dy):
  count=max(1,math.ceil(max(abs(dx),abs(dy))/100))
  self.input(source,[dict(t='motion',dx=dx/count,dy=dy/count) for _ in range(count)])
 def neighbour(self,a,b):
  ax,ay,aw,ah=(a[k] for k in ['x','y','w','h']);bx,by,bw,bh=(b[k] for k in ['x','y','w','h'])
  if min(ay+ah,by+bh)-max(ay,by)>.5:
   if abs(ax-(bx+bw))<1.5:return 0
   if abs(ax+aw-bx)<1.5:return 1
  if min(ax+aw,bx+bw)-max(ax,bx)>.5:
   if abs(ay-(by+bh))<1.5:return 2
   if abs(ay+ah-by)<1.5:return 3
 def path(self,source,target):
  queue=deque([[source]])
  while queue:
   path=queue.popleft()
   if path[-1]==target:return path
   for other in self.devices:
    if other not in path and self.neighbour(self.devices[path[-1]],self.devices[other]) is not None:queue.append(path+[other])
  raise AssertionError('No connected layout path; test does not move user screens')
 def crossing(self,a,b,direction):
  if direction<2:
   along=(max(a['y'],b['y'])+min(a['y']+a['h'],b['y']+b['h']))/2
   return (a['x']-3 if direction==0 else a['x']+a['w']+3,along),(along-a['y'])/a['h']
  along=(max(a['x'],b['x'])+min(a['x']+a['w'],b['x']+b['w']))/2
  return (along,a['y']-3 if direction==2 else a['y']+a['h']+3),(along-a['x'])/a['w']
 def entry(self,b,direction,point):
  x,y=point
  return (b['x']+2 if direction==1 else b['x']+b['w']-2 if direction==0 else x,b['y']+2 if direction==3 else b['y']+b['h']-2 if direction==2 else y)
 def stop_owned(self):
  active=self.state()['active']
  if active is None:self.owned=None;return
  assert active['control_id']==self.owned,'User has a different control session; will not stop it'
  self.input(self.source,[dict(t='key',code=c,down=True) for c in [29,56,1]]+[dict(t='key',code=c,down=False) for c in [1,56,29]])
  self.wait_state(lambda s:s['active'] is None,'Emergency return did not end test session');self.owned=None
 def pair(self,source,target):
  assert self.state()['active'] is None,'User control active; test refused'
  target_ready=self.command(target,'reset');source_ready=self.command(source,'reset')
  assert target_ready.get('focused') and source_ready.get('focused'),'Private fixture focus not acquired; no input injected'
  path=self.path(source,target);self.source=source
  a,b=self.devices[source],self.devices[path[1]];direction=self.neighbour(a,b);point,pos=self.crossing(a,b,direction)
  self.command(source,'edge',direction=direction,position=pos)
  state=self.wait_state(lambda s:s['active'] and s['active']['source']==a['id'] and s['active']['target']==b['id'],'Native source failed to cross first edge')
  self.owned=state['active']['control_id'];cursor=self.entry(b,direction,point)
  for index in range(1,len(path)-1):
   a,b=self.devices[path[index]],self.devices[path[index+1]];direction=self.neighbour(a,b);point,_=self.crossing(a,b,direction)
   if direction<2:self.motion(source,0,point[1]-cursor[1]);self.motion(source,point[0]-cursor[0],0)
   else:self.motion(source,point[0]-cursor[0],0);self.motion(source,0,point[1]-cursor[1])
   self.wait_state(lambda s:s['active'] and s['active']['target']==b['id'],'Native source failed intermediate screen handover');cursor=self.entry(b,direction,point)
  b=self.devices[target];center=(b['x']+b['w']/2,b['y']+b['h']/2);self.motion(source,center[0]-cursor[0],center[1]-cursor[1]);time.sleep(.2)
  events=[dict(t='key',code=64,down=True),dict(t='key',code=64,down=False),dict(t='key',code=42,down=True),dict(t='key',code=64,down=True),dict(t='key',code=64,down=False),dict(t='key',code=42,down=False),dict(t='button',button=1,down=True),dict(t='motion',dx=12,dy=0),dict(t='button',button=1,down=False),dict(t='button',button=2,down=True),dict(t='button',button=2,down=False),dict(t='wheel',dy=120)]
  assert self.status(target).get('focused'),'Target fixture lost focus; keyboard/click injection refused'
  self.input(source,events)
  deadline=time.monotonic()+8;observed={}
  while time.monotonic()<deadline:
   observed=self.status(target);items=observed.get('events',[])
   key='vk' if target=='windows' else 'code';expected={'windows':117,'macos':97,'linux':64}[target]
   keys=[e for e in items if e.get('kind')=='key' and e.get(key)==expected]
   buttons=[e for e in items if e.get('kind')=='button']
   if len(keys)>=4 and len(buttons)>=4 and abs(observed.get('wheel_y',0))>0:break
   time.sleep(.1)
  (args.artifacts/f'{source}-to-{target}.json').write_text(json.dumps(observed,indent=2))
  assert sum(e.get('down') is True for e in keys)==2 and sum(e.get('down') is False for e in keys)==2,f'{source}->{target}: F6 down/up missing'
  assert len(buttons)==4,f'{source}->{target}: mouse buttons missing'
  assert abs(observed.get('wheel_y',0))>0,f'{source}->{target}: scroll missing'
  shifted=[e for e in keys if e.get('down') and (e.get('shift') if target=='windows' else e.get('flags',0)&(0x20000 if target=='macos' else 1))]
  assert shifted,f'{source}->{target}: Shift did not accompany F6'
  if target=='macos':assert observed['cursor_visible'],'Mac receiver cursor is hidden'
  self.check_owned();self.stop_owned()
  self.results.append(dict(source=source,target=target,via=path[1:-1],status='passed',mode=args.transport_mode,evidence='installed native clients and synthetic OS events, not physical hardware certification'))
  print(f'PASS: {source}->{target}: F6, Shift+F6, click, drag, right click, scroll'+(' via '+','.join(path[1:-1]) if len(path)>2 else ''),flush=True)
 def run(self):
  locked=subprocess.check_output(['omarchy-shell','lock','isLocked'],env=self.linux_env,text=True,timeout=10).strip()
  assert locked=='false','Omarchy is locked; manual unlock required. No test input was injected.'
  state=self.state();assert state['active'] is None and not state['settings']['input_enabled'],'Maintenance pause/idle required'
  for d in state['devices']:assert d['trusted'] and d['enabled'] and d['online']
  # Establish live, focused fixtures and park away from shared edges while
  # input is still paused. A stale observer must never enable production input.
  for platform in self.devices:
   assert self.command(platform,'reset')['focused'],'Private fixture did not focus'
   self.command(platform,'park')
  self.viewer.send(dict(t='settings',input_enabled=True,transport_mode=args.transport_mode))
  self.wait_state(lambda s:s['settings']['input_enabled'] and s['settings']['transport_mode']==args.transport_mode,'Requested test transport mode did not enable');time.sleep(4)
  try:
   for source,target in [('macos','windows'),('windows','macos'),('macos','linux'),('linux','macos'),('windows','linux'),('linux','windows')]:self.pair(source,target)
  finally:
   self.stop_owned()
   if self.state()['active'] is None:self.viewer.send(dict(t='settings',**self.original_settings));self.wait_state(lambda s:s['settings']==self.original_settings,'Original settings were not restored')
   (args.artifacts/'results.json').write_text(json.dumps(dict(results=self.results),indent=2))
   if self.state()['active'] is None and self.previous_linux_window:
    expression='hl.dsp.focus({ window = '+json.dumps('address:'+self.previous_linux_window)+' })'
    subprocess.run(['hyprctl','dispatch',expression],env=self.linux_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   self.viewer.close()

if __name__=='__main__':Matrix().run()
