#!/usr/bin/env python3
"""Device trust through the real hub, cryptographic proofs, HTTP and browser UI.
All accounts, keys, clipboard text and files are disposable private fixtures.
"""
import http.client
import importlib.util
import json
from pathlib import Path
import time
from hub_ws import HubSocket
from device_identity import Identity

ROOT=Path(__file__).resolve().parent.parent
_spec=importlib.util.spec_from_file_location('lifecycle_base',Path(__file__).with_name('device-lifecycle.py'))
base=importlib.util.module_from_spec(_spec);_spec.loader.exec_module(base)
require,wait=base.require,base.wait
SCENARIOS={
 'SC-UNAPPROVED-DEVICE-ISOLATION':'pending',
 'SC-APPROVED-IDENTITY-PERSISTS':'persist',
 'SC-FORGED-IDENTITY-REJECTED':'forged',
 'SC-TRUST-REVOCATION':'revoke',
 'SC-TRUST-ACCOUNT-ISOLATION':'isolation',
}
class Scene(base.Scene):
 def raw(self,user='ui-test',origin=None):
  headers={'X-HC-User-ID':user}
  if origin:headers['Origin']=origin
  value=HubSocket('127.0.0.1',port=self.port,tls=False,headers=headers);self.sockets.append(value);return value
 def until(self,ws,kind):
  while True:
   value=ws.read(5)
   if value['t']==kind:return value
 def sync(self):
  self.viewer.send(dict(t='settings',clipboard=True));return self.state()
 def collect_token(self,ws,device,user='ui-test'):
  value=self.until(ws,'device_auth');require(value['status']=='approved' and value.get('token'),'Approved device did not receive a connection credential')
  self.tokens[(user,device)]=value['token'];return value['token']
 def click_trust(self,device):
  self.browser.call('confirm',accept=True)
  self.browser.evaluate(f"document.querySelector('[data-device={device}] .device-trust').click()")
 def pending(self):
  peer=self.connect(device='pending',approve=False)
  own=self.state(ws=peer);require(len(own['devices'])==1 and own['devices'][0]['id']=='pending','Pending connection received other device metadata')
  self.state(lambda s:any(d['id']=='pending' and not d['trusted'] for d in s['devices']))
  self.drain(peer);self.drain(self.mac)
  peer.send(dict(t='direct_hello',version=1,certificate='unapproved',addresses=[]));peer.send(dict(t='edge',dir='left',pos=.5));peer.send(dict(t='clipboard',text='blocked-fixture'))
  self.mac.send(dict(t='clipboard',text='private-fixture'))
  require(self.http('PUT','/api/files/fixture.txt',body=b'fixture')[0]==200,'Browser fixture upload failed')
  time.sleep(.1)
  require(not any(m['t'] in ('clipboard','files','direct_peers','capture','enter') for m in self.drain(peer)),'Unapproved device received sharing data or control')
  require(not any(m['t']=='clipboard' for m in self.drain(self.mac)),'Unapproved clipboard was relayed')
  require(self.http('GET','/api/files',device='pending')[0]==403,'Unapproved native file request accepted')
  require(self.http('GET','/api/files',device='win',device_token='a'*44)[0]==403,'Claiming another device ID granted file access')
  wait(lambda:self.browser.evaluate("document.querySelector('[data-device=pending] .device-trust')?.disabled === false"),'Pending approval control missing')
  fp=self.keys[('ui-test','pending')].fingerprint
  require(self.browser.evaluate("document.querySelector('[data-device=pending] .device-fingerprint').textContent")==fp,'Browser fingerprint differs from proved key')
  self.browser.call('confirm',accept=True)
  self.browser.evaluate("document.querySelector('[data-device=pending] .device-trust').focus()")
  self.browser.call('key',key=' ',code='Space',vk=32)
  self.state(lambda s:any(d['id']=='pending' and d['trusted'] for d in s['devices']))
  self.collect_token(peer,'pending')
  require(self.http('GET','/api/files',device='pending')[0]==200,'Approved native file access did not resume')
  self.browser.call('viewport',width=390,height=844,mobile=True);time.sleep(.25)
  require(self.browser.evaluate('document.documentElement.scrollWidth <= innerWidth'),'Trust fingerprint causes horizontal overflow')
  self.browser.call('shot',path=str(self.artifacts/'device-trust-mobile.png'))
  self.browser.call('viewport',width=1280,height=1000,mobile=False);time.sleep(.25)
  self.browser.call('shot',path=str(self.artifacts/'device-trust-desktop.png'))
 def persist(self):
  before=self.sync();old=self.tokens[('ui-test','win')];self.enabled('win',False)
  devices=[d['id'] for d in before['devices']]
  self.stop();self.start()
  peers={device:self.connect(device=device,approve=False) for device in devices}
  self.mac,self.win,self.linux=peers['mac'],peers['win'],peers['linux'];self.viewer=self.connect()
  after=self.state(lambda s:len(s['devices'])==len(devices))
  for original in before['devices']:
   current=next(d for d in after['devices'] if d['id']==original['id'])
   require(current['trusted'] and current['fingerprint']==original['fingerprint'],'Trust or key changed on restart')
   require(all(current[k]==original[k] for k in ('x','y','w','h')),'Credential migration moved a screen')
  require(not next(d for d in after['devices'] if d['id']=='win')['enabled'],'Trust handshake woke a dormant device')
  self.enabled('win',True)
  require(self.http('GET','/api/files',device='win',device_token=old)[0]==403,'Old connection file token survived restart')
  require(self.http('GET','/api/files',device='win')[0]==200,'New proved connection could not use native files')
  self.browser.call('reload');wait(lambda:self.browser.evaluate("document.querySelector('[data-device=win] .device-trust')?.textContent === '撤销信任'"),'Browser lost approved state after restart')
 def forged(self):
  self.mac.send(dict(t='edge',dir='right',pos=.5));original=self.state(lambda s:s['active'] and s['active']['target']=='win')
  key=Identity();peer=self.raw()
  peer.send(dict(t='device_hello',id='win',name='spoof',platform='windows',w=1234,h=987,public_key=key.public))
  challenge=self.until(peer,'device_challenge');peer.send(dict(t='device_proof',signature=key.proof('ui-test','win',challenge['nonce'])))
  require(self.until(peer,'device_auth')['status']=='key_mismatch','Different key could claim approved device ID')
  current=self.sync();require(current['active']==original['active'],'Forged registration interrupted existing control')
  for field in ('fingerprint','x','y','w','h','name'):
   require(next(d for d in current['devices'] if d['id']=='win')[field]==next(d for d in original['devices'] if d['id']=='win')[field],'Forgery changed bound metadata')
  a,b=self.raw(),self.raw();key=Identity()
  message=dict(t='device_hello',id='replay',name='replay',platform='linux',w=1000,h=800,public_key=key.public)
  a.send(message);b.send(message);one=self.until(a,'device_challenge');two=self.until(b,'device_challenge')
  proof=key.proof('ui-test','replay',one['nonce']);b.send(dict(t='device_proof',signature=proof));self.until(b,'error')
  b.send(dict(t='device_proof',signature=key.proof('ui-test','replay',two['nonce'])));self.until(b,'error')
  require(not any(d['id']=='replay' for d in self.sync()['devices']),'Consumed or foreign challenge was accepted')
  self.mac.send(dict(t='release'));self.state(lambda s:s['active'] is None)
 def revoke(self):
  require(self.http('PUT','/api/files/retained.txt',body=b'keep')[0]==200,'Fixture upload failed')
  old=self.tokens[('ui-test','win')]
  upload=http.client.HTTPConnection('127.0.0.1',self.port,timeout=10)
  upload.putrequest('PUT','/api/files/revoked-upload.bin')
  for k,v in {'X-HC-User-ID':'ui-test','X-Lzc-Mesh-Device-Id':'win','X-Lzc-Mesh-Device-Token':old,'Content-Length':'1048576'}.items():upload.putheader(k,v)
  upload.endheaders();upload.send(b'x'*4096)
  self.mac.send(dict(t='edge',dir='right',pos=.5));self.state(lambda s:s['active'] and s['active']['target']=='win')
  self.mac.send(dict(t='key',code=30,down=True));self.mac.send(dict(t='button',button=1,down=True));self.drain(self.win)
  self.click_trust('win');self.state(lambda s:s['active'] is None and not next(d for d in s['devices'] if d['id']=='win')['trusted'])
  response=upload.getresponse();require(response.status==403,'Revocation did not cancel native upload');response.read();upload.close()
  messages=self.drain(self.win);require(any(m['t']=='device_auth' and m['status']=='revoked' and not m.get('token') for m in messages),'Native revocation was not delivered')
  require(self.http('GET','/api/files',device='win',device_token=old)[0]==403,'Revoked file token remains usable')
  require(self.http('GET','/api/files/retained.txt')[1]==b'keep','Revocation removed stored files or blocked manager')
  self.enabled('win',False);self.enabled('win',True)
  self.win.close();self.win=self.connect(device='win',approve=False)
  require(not next(d for d in self.sync()['devices'] if d['id']=='win')['trusted'],'Running switch or reconnect restored revoked trust')
  require(not self.tokens[('ui-test','win')],'Revoked connection obtained a file token')
  self.click_trust('win');self.state(lambda s:next(d for d in s['devices'] if d['id']=='win')['trusted'])
  fresh=self.collect_token(self.win,'win');require(fresh!=old,'Reapproval reused old file credential')
  require(self.http('GET','/api/files',device='win')[0]==200 and self.http('GET','/api/files',device='win',device_token=old)[0]==403,'Reapproval restored old credentials')
 def isolation(self):
  foreign=self.connect(user='other-test',device='foreign');manager=self.connect(user='other-test')
  self.drain(self.viewer);self.drain(manager)
  fp=self.keys[('other-test','foreign')].fingerprint
  self.viewer.send(dict(t='device_trust',id='foreign',fingerprint=fp,approved=True))
  require(any(m['t']=='error' for m in self.drain(self.viewer)),'Another account could approve a device')
  manager.send(dict(t='device_trust',id='win',fingerprint=self.keys[('ui-test','win')].fingerprint,approved=False))
  require(any(m['t']=='error' for m in self.drain(manager)),'Another account could revoke a device')
  token=self.tokens[('other-test','foreign')]
  require(self.http('GET','/api/files',device='foreign',device_token=token)[0]==403,'File credential crossed accounts')
  require(self.http('GET','/api/files',device='foreign',user='other-test')[0]==200,'Foreign account fixture was actually revoked')
  require(self.http('PUT','/api/files/account-only.txt',body=b'first',user='a.b')[0]==200,'Punctuation account upload failed')
  require(self.http('GET','/api/files/account-only.txt',user='a_b')[0]==404,'Sanitized account paths collided')
  require(self.http('PUT','/api/files/account-only.txt',body=b'second',user='a_b')[0]==200,'Underscore account upload failed')
  require(self.http('GET','/api/files/account-only.txt',user='a.b')[1]==b'first','Another account overwrote stored files')
  try:self.raw(origin='https://untrusted.example')
  except Exception as error:require('403' in str(error),'Cross-origin test failed for an unrelated reason')
  else:raise AssertionError('Cross-origin WebSocket was accepted')

def execute(args,scenarios):
 progress=lambda value:print('AC_PROGRESS '+json.dumps(value,ensure_ascii=False),file=__import__('sys').stderr,flush=True)
 scene=Scene(args);results=[];failure=None
 progress(dict(event='phase',phase='setup',status='running',message='Starting isolated real hub, device proofs and management browser'))
 try:
  started=time.monotonic();scene.prepare()
  progress(dict(event='phase',phase='setup',status='passed',message='Private accounts and cryptographic fixture identities ready',duration_seconds=time.monotonic()-started))
  for scenario in scenarios:
   gid=scenario['global_id'];key=gid.rsplit('/',1)[-1];started=time.monotonic()
   progress(dict(event='scenario',scenario=gid,status='running'))
   result=dict(scenario=gid,status='passed',test_id='hub-trust/'+SCENARIOS.get(key,'unsupported'))
   try:
    if key not in SCENARIOS:raise ValueError('No trust implementation for selected AC')
    getattr(scene,SCENARIOS[key])()
   except Exception as error:result.update(status='failed',error=str(error))
   result.update(duration_seconds=time.monotonic()-started,debug_report=str(args.artifacts));results.append(result)
   progress(dict(event='scenario',scenario=gid,status=result['status'],duration_seconds=result['duration_seconds']))
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
