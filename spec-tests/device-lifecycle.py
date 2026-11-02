#!/usr/bin/env python3
"""Local real-hub/browser integration for dormancy protocol and HTTP cancellation.
Uses private fixture accounts/data, not personal clipboard contents or documents.
Native desktop resource checks are recorded separately in the deployment report.
"""
import http.client
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import tempfile
import time
from hub_ws import HubSocket
from device_identity import Identity

ROOT=Path(__file__).resolve().parent.parent
SCENARIOS={
 'SC-DEVICE-DORMANCY':'dormancy',
 'SC-DORMANCY-SURVIVES-RESTART':'restart',
 'SC-WAKE-WITHOUT-REPLAY':'wake',
 'SC-DEVICE-SWITCH-ACCOUNT-ISOLATION':'isolation',
}
def require(value,message):
 if not value:raise AssertionError(message)
def wait(predicate,message,timeout=10):
 until=time.monotonic()+timeout
 while True:
  value=predicate()
  if value:return value
  if time.monotonic()>until:raise AssertionError(message)
  time.sleep(.05)

class Browser:
 def __init__(self,url,artifacts):
  self.log=(artifacts/'browser.log').open('w')
  self.process=subprocess.Popen(['node',str(ROOT/'spec-tests/browser-probe.mjs'),url],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.log,text=True,bufsize=1)
  self.serial=0
  require(self.read().get('ready'),'Browser did not start')
 def read(self):
  require(select.select([self.process.stdout],[],[],20)[0],'Browser probe timed out')
  line=self.process.stdout.readline();require(line,'Browser probe exited unexpectedly');return json.loads(line)
 def call(self,op,**values):
  self.serial+=1;self.process.stdin.write(json.dumps(dict(id=self.serial,op=op,**values))+'\n');self.process.stdin.flush()
  value=self.read();require(not value.get('error'),value.get('error'));return value.get('result')
 def evaluate(self,expression):return self.call('eval',expression=expression)
 def close(self):
  try:self.process.stdin.close();self.process.wait(timeout=10)
  except (OSError,subprocess.TimeoutExpired):self.process.kill();self.process.wait()
  self.log.close()

class Scene:
 def __init__(self,args):
  self.args=args;self.artifacts=args.artifacts;self.artifacts.mkdir(parents=True,exist_ok=True)
  self.directory=tempfile.TemporaryDirectory(prefix='lzc-mesh-device-test-')
  self.base=Path(self.directory.name);self.sockets=[];self.browser=None;self.server=None;self.server_log=None
  self.keys={};self.tokens={}
  with socket.socket() as s:s.bind(('127.0.0.1',0));self.port=s.getsockname()[1]
 def start(self):
  environment=os.environ|{'LZC_MESH_LISTEN':f'127.0.0.1:{self.port}','LZC_MESH_DATA':str(self.base/'layouts'),'LZC_MESH_FILES':str(self.base/'files'),'LZC_MESH_WEB':str(ROOT/'lzc-mesh-lpk/web')}
  self.server_log=(self.artifacts/'hub.log').open('a')
  self.server=subprocess.Popen([str(self.args.hub_bin)],env=environment,stdout=self.server_log,stderr=subprocess.STDOUT)
  def healthy():
   try:return self.http('GET','/api/health')[0]==200
   except OSError:return False
  wait(healthy,'Private hub did not start')
 def stop(self):
  for ws in self.sockets:
   try:ws.close()
   except OSError:pass
  self.sockets=[]
  if self.server:
   self.server.send_signal(signal.SIGINT)
   try:self.server.wait(timeout=5)
   except subprocess.TimeoutExpired:self.server.kill();self.server.wait()
   self.server=None
  if self.server_log:self.server_log.close();self.server_log=None
 def connect(self,user='ui-test',device=None,approve=True,key=None):
  ws=HubSocket('127.0.0.1',port=self.port,tls=False,headers={'X-HC-User-ID':user});self.sockets.append(ws)
  if device is None:ws.send(dict(t='hello',platform='web'));return ws
  identity=key or self.keys.setdefault((user,device),Identity())
  ws.send(dict(t='device_hello',id=device,name=device,platform={'mac':'macos','win':'windows','linux':'linux'}.get(device,'linux'),w=1000,h=800,version='0.2.0',public_key=identity.public))
  while True:
   value=ws.read(5)
   if value['t']=='device_challenge':ws.send(dict(t='device_proof',signature=identity.proof(user,device,value['nonce'])))
   if value['t']=='error':raise AssertionError(value['message'])
   if value['t']=='device_auth':break
  if approve and value['status']!='approved':
   with HubSocket('127.0.0.1',port=self.port,tls=False,headers={'X-HC-User-ID':user}) as manager:
    manager.send(dict(t='hello',platform='web'));manager.send(dict(t='device_trust',id=device,fingerprint=identity.fingerprint,approved=True))
    while True:
     value=ws.read(5)
     if value['t']=='device_auth' and value['status']=='approved':break
  self.tokens[(user,device)]=value.get('token','')
  return ws
 def state(self,predicate=lambda s:True,ws=None):
  ws=ws or self.viewer;deadline=time.monotonic()+10
  while time.monotonic()<deadline:
   message=ws.read(5)
   if message['t']=='state' and predicate(message):return message
  raise AssertionError('Expected state was not received')
 def drain(self,ws):
  out=[]
  while True:
   try:out.append(ws.read(.1))
   except socket.timeout:return out
 def enabled(self,device,on):
  self.viewer.send(dict(t='device_enabled',id=device,enabled=on))
  return self.state(lambda s:any(d['id']==device and d['enabled']==on for d in s['devices']))
 def http(self,method,path,body=None,device=None,user='ui-test',device_token=None):
  headers={'X-HC-User-ID':user}
  if device:
   headers['X-Lzc-Mesh-Device-Id']=device
   headers['X-Lzc-Mesh-Device-Token']=self.tokens.get((user,device),'') if device_token is None else device_token
  conn=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
  conn.request(method,path,body,headers);response=conn.getresponse();data=response.read();status=response.status;conn.close();return status,data
 def prepare(self):
  self.start();self.mac=self.connect(device='mac');self.win=self.connect(device='win');self.linux=self.connect(device='linux');self.viewer=self.connect()
  self.state(lambda s:len(s['devices'])==3)
  self.viewer.send(dict(t='layout',id='mac',x=0,y=0));self.viewer.send(dict(t='layout',id='win',x=1000,y=0));self.viewer.send(dict(t='layout',id='linux',x=2000,y=0))
  self.state(lambda s:next(d for d in s['devices'] if d['id']=='win')['x']==1000)
  self.browser=Browser(f'http://127.0.0.1:{self.port}/',self.artifacts)
  wait(lambda:self.browser.evaluate("document.querySelectorAll('.device-switch').length === 3 && ws?.readyState === 1"),'Management page failed to join private account')
 def dormancy(self):
  self.mac.send(dict(t='edge',dir='right',pos=.5));self.state(lambda s:s['active'] and s['active']['target']=='win')
  self.mac.send(dict(t='key',code=30,down=True));self.mac.send(dict(t='button',button=1,down=True));time.sleep(.1)
  self.drain(self.win)
  # Keep an incomplete upload open. Dormancy must cancel it without waiting for
  # its remaining body or publishing a partially received file.
  upload=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
  upload.putrequest('PUT','/api/files/interrupted.bin');upload.putheader('X-HC-User-ID','ui-test');upload.putheader('X-Lzc-Mesh-Device-Id','win');upload.putheader('X-Lzc-Mesh-Device-Token',self.tokens[('ui-test','win')]);upload.putheader('Content-Length','1048576');upload.endheaders();upload.send(b'x'*4096)
  self.browser.evaluate("document.querySelector('[data-device=win] .device-switch').focus()")
  self.browser.call('key',key=' ',code='Space',vk=32)
  self.state(lambda s:not next(d for d in s['devices'] if d['id']=='win')['enabled'] and s['active'] is None)
  wait(lambda:self.browser.evaluate("document.querySelector('[data-device=win] .device-switch').getAttribute('aria-checked') === 'false'"),'Graphical switch did not reflect dormancy')
  response=upload.getresponse();require(response.status==403,'Dormancy did not abort an in-flight upload');response.read();upload.close()
  released=self.drain(self.win)
  require(any(m['t']=='key' and not m['down'] for m in released),'Held key was not released')
  require(any(m['t']=='button' and not m['down'] for m in released),'Held button was not released')
  self.drain(self.mac);self.drain(self.linux)
  self.win.send(dict(t='clipboard',text='must-not-leave-dormant-device'))
  time.sleep(.1);require(not any(m['t']=='clipboard' for m in self.drain(self.mac)),'Dormant device sent clipboard')
  self.mac.send(dict(t='clipboard',text='new-copy-for-enabled-devices'))
  time.sleep(.1);require(not any(m['t']=='clipboard' for m in self.drain(self.win)),'Dormant device received clipboard')
  require(any(m['t']=='clipboard' for m in self.drain(self.linux)),'Unaffected device lost clipboard sharing')
  require(self.http('GET','/api/files',device='win')[0]==403,'Dormant device can access file API')
  require(self.http('GET','/api/files')[0]==200,'Browser file management was disabled')
  require('interrupted.bin' not in self.http('GET','/api/files')[1].decode(),'Interrupted file was published')
  self.browser.call('viewport',width=390,height=844,mobile=True)
  require(self.browser.evaluate('document.documentElement.scrollWidth <= innerWidth'),'Mobile management page overflows horizontally')
  self.browser.call('shot',path=str(self.artifacts/'device-switch-mobile.png'))
  self.browser.call('viewport',width=1280,height=1000,mobile=False)
  self.browser.call('shot',path=str(self.artifacts/'device-switch-desktop.png'))
 def restart(self):
  self.enabled('win',False)
  before=json.loads((self.base/'layouts/ui-test.json').read_text())
  self.stop();self.start()
  self.mac=self.connect(device='mac');self.win=self.connect(device='win');self.linux=self.connect(device='linux');self.viewer=self.connect()
  state=self.state(lambda s:len(s['devices'])==3)
  require(not next(d for d in state['devices'] if d['id']=='win')['enabled'],'Dormancy lost on service restart/reconnect')
  self.win.send(dict(t='sharing',enabled=True))
  state=self.state(lambda s:not next(d for d in s['devices'] if d['id']=='win')['sharing'])
  require(not next(d for d in state['devices'] if d['id']=='win')['enabled'],'Local keyboard preference overwrote remote close')
  for device in state['devices']:
   require((device['x'],device['y'])==(before['devices'][device['id']]['x'],before['devices'][device['id']]['y']),'Restart moved a screen')
  self.browser.call('reload')
  wait(lambda:self.browser.evaluate("document.querySelector('[data-device=win] .device-switch')?.getAttribute('aria-checked') === 'false'"),'Page did not recover saved switch after restart')
 def wake(self):
  self.enabled('win',False);self.drain(self.win)
  self.mac.send(dict(t='clipboard',text='copy-while-closed'));time.sleep(.1)
  self.browser.evaluate("document.querySelector('[data-device=win] .device-switch').click()")
  self.state(lambda s:next(d for d in s['devices'] if d['id']=='win')['enabled'])
  pending=self.drain(self.win);require(not any(m['t']=='clipboard' for m in pending),'Old clipboard was replayed on wake')
  self.mac.send(dict(t='clipboard',text='fresh-copy-after-wake'));time.sleep(.1)
  copies=[m['text'] for m in self.drain(self.win) if m['t']=='clipboard']
  require(copies==['fresh-copy-after-wake'],'Fresh clipboard did not resume exactly once')
  # The in-flight download is also revoked on off -> on; the old HTTP body
  # cannot silently continue after the same client has already been re-enabled.
  block=b'0'*(1024*1024);upload=http.client.HTTPConnection('127.0.0.1',self.port,timeout=20)
  upload.putrequest('PUT','/api/files/preserved.bin');upload.putheader('X-HC-User-ID','ui-test');upload.putheader('Content-Length',str(32*len(block)));upload.endheaders()
  for _ in range(32):upload.send(block)
  response=upload.getresponse();require(response.status==200,'Fixture upload failed');response.read();upload.close()
  download=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10);download.connect();download.sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,16384)
  download.request('GET','/api/files/preserved.bin',headers={'X-HC-User-ID':'ui-test','X-Lzc-Mesh-Device-Id':'win','X-Lzc-Mesh-Device-Token':self.tokens[('ui-test','win')]})
  response=download.getresponse();require(response.status==200,'Fixture download failed');response.read(1024)
  self.enabled('win',False);self.enabled('win',True)
  try:rest=response.read();received=len(rest)+1024
  except http.client.IncompleteRead as error:received=len(error.partial)+1024
  download.close();require(received<32*len(block),'Old download survived dormancy and wake')
  require('preserved.bin' in self.http('GET','/api/files')[1].decode(),'Dormancy deleted an existing shared file')
  self.browser.evaluate("document.querySelector('[data-device=win] .device-switch').focus()")
  self.viewer.send(dict(t='settings',clipboard=True));self.state()
  require(self.browser.evaluate("document.activeElement === document.querySelector('[data-device=win] .device-switch')"),'Status rendering stole switch keyboard focus')
 def isolation(self):
  foreign=self.connect(user='other-test',device='foreign')
  self.drain(foreign);self.drain(self.viewer)
  self.viewer.send(dict(t='device_enabled',id='foreign',enabled=False))
  require(any(m['t']=='error' for m in self.drain(self.viewer)),'Another account device was not rejected')
  foreign.send(dict(t='ping'))
  require(self.http('GET','/api/files',device='foreign',device_token=self.tokens[('other-test','foreign')])[0]==403,'File device id crossed accounts')
  require(self.http('GET','/api/files',user='other-test',device='foreign')[0]==200,'Foreign device was actually changed')
 def close(self):
  if self.browser:self.browser.close()
  self.stop();self.directory.cleanup()

def execute(args,scenarios):
 from importlib.util import spec_from_file_location,module_from_spec
 spec=spec_from_file_location('progress',ROOT/'spec-tests/windows-parity.py');module=module_from_spec(spec);spec.loader.exec_module(module)
 progress=module.progress
 scene=Scene(args);results=[];failure=None
 progress(dict(event='phase',phase='setup',status='running',message='Starting private real Rust hub and headless Chromium'))
 try:
  started=time.monotonic();scene.prepare()
  progress(dict(event='phase',phase='setup',status='passed',message='Real HTTP/WebSocket fixture and management page ready',duration_seconds=time.monotonic()-started))
  for scenario in scenarios:
   gid=scenario['global_id'];key=gid.rsplit('/',1)[-1];started=time.monotonic()
   progress(dict(event='scenario',scenario=gid,status='running'))
   result=dict(scenario=gid,status='passed',test_id='hub-lifecycle/'+SCENARIOS.get(key,'unsupported'))
   try:
    if key not in SCENARIOS:raise ValueError('No local hub lifecycle implementation for selected AC')
    getattr(scene,SCENARIOS[key])()
   except Exception as error:result.update(status='failed',error=str(error))
   result['duration_seconds']=time.monotonic()-started;result['debug_report']=str(args.artifacts)
   results.append(result);progress(dict(event='scenario',scenario=gid,status=result['status'],duration_seconds=result['duration_seconds']))
   if result['status']=='failed':break
 except Exception as error:failure=str(error)
 finally:
  try:scene.close()
  except Exception as error:failure='Cleanup failed: '+str(error)
 selected={r['scenario'] for r in results}
 for scenario in scenarios:
  if scenario['global_id'] not in selected:results.append(dict(scenario=scenario['global_id'],status='skipped',error=failure or 'Earlier scenario failed',duration_seconds=0))
 result=dict(status='failed' if failure or any(r['status']!='passed' for r in results) else 'passed',results=results)
 if failure:result['error']=failure
 args.artifacts.mkdir(parents=True,exist_ok=True);(args.artifacts/'results.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
 return result
