#!/usr/bin/env python3
"""Real Windows-client relay tests with a Linux build of the native transport.

The supplied SSH helper authenticates outside this script. The coordinator uses
an existing account session and restores account settings/removes its own device.
It never changes network routes, firewall rules, pairing or other device layouts.
"""
import argparse
import base64
import copy
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import uuid
from hub_ws import HubSocket
from device_identity import Identity

ROOT = Path(__file__).resolve().parent.parent
SCENARIOS = {
    'SC-RELAY-MODE-CLOSES-LISTENERS': 'relay',
    'SC-GLOBAL-INPUT-PAUSE': 'pause',
    'SC-SEPARATE-INPUT-CHANNEL-OBSERVATION': 'channels',
    'SC-BLUETOOTH-UNAVAILABLE': 'bluetooth',
}

def require(condition, message):
    if not condition: raise AssertionError(message)

def wait(predicate, message, timeout=15):
    end = time.monotonic() + timeout
    while True:
        value = predicate()
        if value: return value
        if time.monotonic() >= end: raise AssertionError(message)
        time.sleep(.15)

class Peer:
    def __init__(self, args, device_id):
        self.id = device_id
        self.identity=Identity()
        self.registered=threading.Event();self.approved=threading.Event()
        self.ws = HubSocket(args.hub, args.cookie, args.hub_address)
        self.state = None
        self.grant = None
        self.events = []
        self.messages = []
        self.error = None
        self.stopping = False
        self.write_lock = threading.Lock()
        self.core_lock = threading.Lock()
        self.core = subprocess.Popen([str(ROOT/'spec-tests/.run-ac/transport-probe'),device_id],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,bufsize=1)
        self.threads = [threading.Thread(target=self.read_hub,daemon=True),threading.Thread(target=self.read_core,daemon=True)]
        self.hub({'t':'hub_connected'})
        for thread in self.threads: thread.start()
        self.send({'t':'device_hello','id':self.id,'name':'Windows integration probe','platform':'linux','w':1000,'h':1440,'version':'0.2.0','public_key':self.identity.public})
    def authorize(self,args):
        wait(lambda:self.registered.is_set() or self.error,'Test identity did not complete its proof')
        require(not self.error,'Test identity failed: '+str(self.error))
        with HubSocket(args.hub,args.cookie,args.hub_address) as manager:
            manager.send(dict(t='hello',platform='web'))
            manager.send(dict(t='device_trust',id=self.id,fingerprint=self.identity.fingerprint,approved=True))
            wait(lambda:self.approved.is_set() or self.error,'Disposable test identity was not approved')
        require(not self.error,'Test identity failed: '+str(self.error))
    def send(self,value):
        with self.write_lock: self.ws.send(value)
    def hub(self,value):
        with self.core_lock:
            self.core.stdin.write(json.dumps(value,separators=(',',':'))+'\n');self.core.stdin.flush()
    def local(self,value): self.hub({'_test':'input','message':value})
    def read_hub(self):
        try:
            while not self.stopping:
                try: value=self.ws.read(.5)
                except socket.timeout: continue
                if value['t'] in ('clipboard','files'):continue
                if value['t']=='device_challenge':self.send(dict(t='device_proof',signature=self.identity.proof(value['user'],self.id,value['nonce'])))
                if value['t']=='device_auth':
                    self.registered.set()
                    if value['status']=='approved':self.approved.set()
                if value['t']=='state': self.state=value
                if value['t']=='direct_session': self.grant=value
                self.messages.append(value)
                self.hub(value)
        except Exception as error:
            if not self.stopping: self.error=str(error)
    def read_core(self):
        try:
            for line in self.core.stdout:
                value=json.loads(line);self.events.append(value)
                if value.get('_lan')=='hub': self.send(value['message'])
                elif value.get('_lan')=='inject' and value['message'].get('_motion_trace'):
                    self.hub({'_test':'applied','message':value['message']['_motion_trace']})
        except Exception as error:
            if not self.stopping: self.error=str(error)
    def injected(self,kind): return [e['message'] for e in self.events if e.get('_lan')=='inject' and e['message'].get('t')==kind]
    def active(self): return self.state and self.state.get('active')
    def close(self):
        self.stopping=True
        self.ws.close()
        try:
            self.core.stdin.close();self.core.wait(timeout=5)
        except (OSError,subprocess.TimeoutExpired): self.core.kill();self.core.wait()
        for thread in self.threads: thread.join(timeout=2)

class Environment:
    def __init__(self,args):
        self.args=args
        self.peer=None
        self.original=None
        self.command_id=0
        self.observer_started=False
        self.windows=None
        self.artifacts=args.artifacts
        self.artifacts.mkdir(parents=True,exist_ok=True)
    def remote(self,script,timeout=45):
        preamble="$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';"
        encoded=base64.b64encode((preamble+script).encode('utf-16le')).decode()
        try:
            result=subprocess.run([self.args.ssh_helper,'win','powershell -NoProfile -EncodedCommand '+encoded],capture_output=True,text=True,timeout=timeout)
        except subprocess.TimeoutExpired:
            raise RuntimeError('Windows SSH test command timed out') from None
        if result.returncode: raise RuntimeError('Windows test command failed: '+result.stdout[-1000:]+result.stderr[-1000:])
        return result.stdout
    def remote_json(self,expression):
        output=self.remote("$value=& {"+expression+"};Write-Output ('RESULT:'+($value | ConvertTo-Json -Depth 8 -Compress))")
        line=next((s[len('RESULT:'):] for s in output.splitlines() if s.startswith('RESULT:')),None)
        require(line is not None,'Missing Windows JSON result')
        return json.loads(line)
    def observer(self):
        return self.remote_json("if(Test-Path \"$env:USERPROFILE\\mesh-parity\\observer.json\"){Get-Content \"$env:USERPROFILE\\mesh-parity\\observer.json\" -Raw | ConvertFrom-Json}")
    def command(self,op,**values):
        self.command_id+=1
        payload=base64.b64encode(json.dumps(dict(op=op,id=self.command_id,**values)).encode()).decode()
        self.remote("$p=\"$env:USERPROFILE\\mesh-parity\\observer.json.command\";[IO.File]::WriteAllBytes($p+'.tmp',[Convert]::FromBase64String('"+payload+"'));Move-Item -Force ($p+'.tmp') $p")
        result=wait(lambda:(v if (v:=self.observer()) and v.get('command')==self.command_id else None),'Observer command did not finish: '+op,timeout=25)
        require(not result.get('error'),result.get('error'))
        return result
    def info(self):
        return self.remote_json("$p=Get-Process -Name 'LZC Mesh' -ErrorAction SilentlyContinue;$f=[IO.File]::Open(\"$env:LOCALAPPDATA\\LZC Mesh\\client.log\",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite);try{$bytes=New-Object byte[] ([Math]::Min($f.Length,1048576));$n=$f.Read($bytes,0,$bytes.Length);$log=[Text.Encoding]::UTF8.GetString($bytes,0,$n)}finally{$f.Dispose()};$pattern='^\\s*UDP\\s+.*\\s+'+$p.Id+'\\s*$';@{pid=$p.Id;udp=@(netstat -ano -p udp | Where-Object {$_ -match $pattern}).Count;log=$log}")
    def mode(self,index):
        active=self.peer.active()
        require(not active or self.peer.id in (active['source'],active['target']),'A user control session started; do not change its mode')
        self.command('mode',index=index)
        mode=['auto','bluetooth','direct','relay'][index]
        wait(lambda:self.peer.state and self.peer.state['settings']['transport_mode']==mode,'Account mode was not synchronized: '+mode)
        wait(lambda:self.observer().get('mode_index')==index,'Windows mode selection did not match the hub')
    def begin_receiver(self):
        require(not self.peer.active(),'Unexpected active control before test')
        self.command('reset')
        self.peer.local({'t':'edge','dir':'left','pos':.25})
        wait(lambda:self.peer.active() and self.peer.active()['source']==self.peer.id and self.peer.active()['target']==self.windows['id'],'Peer could not control Windows through the hub')
        # Entry is one pixel inside the right edge; position in the observer.
        a=wait(lambda:self.peer.grant and self.peer.grant['source']==self.peer.id and self.peer.grant['target']==self.windows['id'] and self.peer.grant,'Control grant was not delivered')
        target=self.windows
        self.peer.local({'t':'motion','dx':target['x']+123-a['x'],'dy':target['y']+239-a['y']})
        wait(lambda:any(p['x']==123 and p['y']==239 for p in self.observer().get('positions',[])),'Windows observer never received the exact target position')
    def release(self):
        self.peer.local({'t':'release'})
        wait(lambda:self.peer.state and not self.peer.active(),'Control session was not released')
        time.sleep(.3)
    def prepare(self):
        with HubSocket(self.args.hub,self.args.cookie,self.args.hub_address) as viewer:
            viewer.send({'t':'hello','platform':'web'})
            while True:
                value=viewer.read(5)
                if value['t']=='state': self.original=value;break
        require(not self.original['active'],'A user control session is active; test aborted without taking it over')
        self.windows=next(d for d in self.original['devices'] if d['platform']=='windows' and d['online'])
        require(self.windows.get('trusted') and self.windows.get('enabled'),'Approve and enable the Windows device before interactive acceptance')
        (self.artifacts/'state-before.json').write_text(json.dumps(self.original,ensure_ascii=False,indent=2))
        with (self.artifacts/'build.log').open('w') as log:
            subprocess.run(['sh','spec-tests/build-windows-tests.sh'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
        expected=__import__('hashlib').sha256((ROOT/'lzc-mesh-client/windows/build/LZC Mesh.exe').read_bytes()).hexdigest()
        installed=self.remote_json('(Get-FileHash \"$env:LOCALAPPDATA\\\\Programs\\\\LZC Mesh\\\\LZC Mesh.exe\" -Algorithm SHA256).Hash')
        require(installed.lower()==expected,'Installed Windows client does not match the local build')
        (self.artifacts/'installed-sha256.txt').write_text(expected+'\n')
        self.remote('New-Item -ItemType Directory -Force \"$env:USERPROFILE\\\\mesh-parity\" | Out-Null')
        subprocess.run([self.args.copy_helper,'win',str(ROOT/'spec-tests/.run-ac/windows-observer.exe'),'R:mesh-parity/windows-observer.exe'],check=True,timeout=90)
        self.remote("$path=\"$env:USERPROFILE\\mesh-parity\";if(Get-Process -Name 'windows-observer' -ErrorAction SilentlyContinue){throw 'An observer is already running'};Remove-Item \"$path\\observer.json\",\"$path\\observer.json.command\" -ErrorAction SilentlyContinue;$name='LZC Mesh parity observer';$p=New-ScheduledTaskPrincipal -UserId \"$env:USERDOMAIN\\$env:USERNAME\" -LogonType Interactive -RunLevel Limited;Register-ScheduledTask -TaskName $name -Principal $p -Action (New-ScheduledTaskAction -Execute \"$path\\windows-observer.exe\") -Force | Out-Null;try{Start-ScheduledTask -TaskName $name;Start-Sleep 2}finally{Unregister-ScheduledTask -TaskName $name -Confirm:$false}")
        self.observer_started=True
        wait(lambda:self.observer(),'Windows observer did not open')
        self.peer=Peer(self.args,'test-windows-parity-'+uuid.uuid4().hex[:10])
        self.peer.authorize(self.args)
        wait(lambda:self.peer.state and any(d['id']==self.peer.id for d in self.peer.state['devices']),'Test peer did not join the hub')
        self.peer.send({'t':'layout','id':self.peer.id,'x':self.windows['x']+self.windows['w'],'y':self.windows['y']})
        wait(lambda:any(d['id']==self.peer.id and d['x']==self.windows['x']+self.windows['w'] and d['y']==self.windows['y'] for d in self.peer.state['devices']),'Test screen was not placed adjacent to Windows')
        self.command('reset')
    def relay(self):
        self.mode(3)
        wait(lambda:self.info()['udp']==0,'Relay mode did not release Windows UDP sockets')
        self.begin_receiver()
        for message in [dict(t='button',button=1,down=True),dict(t='motion',dx=5,dy=0),dict(t='button',button=1,down=False),dict(t='key',code=64,down=True),dict(t='key',code=64,down=False)]: self.peer.local(message)
        for _ in range(16):self.peer.local(dict(t='wheel',dx=.25,dy=.25,source='continuous',phase='changed'));time.sleep(.02)
        self.peer.local(dict(t='wheel',dx=0,dy=0,source='continuous',phase='ended',stop_x=True,stop_y=True))
        observed=wait(lambda:(s if len((s:=self.observer())['events'])>=4 and s['wheel_y']==4 and s['wheel_x']==-4 else None),'Windows did not receive ordered clicks, keys or fractional scrolling')
        (self.artifacts/'relay-observed.json').write_text(json.dumps(observed,indent=2))
        buttons=[e for e in observed['events'] if e['kind']=='button']
        keys=[e for e in observed['events'] if e['kind']=='key']
        require([(e['down'],e['x'],e['y']) for e in buttons]==[(True,123,239),(False,128,239)],'Click/drag position or ordering changed')
        require([(e['vk'],e['down']) for e in keys]==[(117,True),(117,False)],'F6 down/up was not preserved: '+repr(keys))
        time.sleep(.5)
        require(self.observer()['wheel_y']==4,'Receiver generated extra momentum after stop')
        self.release()
        self.command('restart')
        wait(lambda:self.peer.state and any(d['id']==self.windows['id'] and d['online'] for d in self.peer.state['devices']),'Windows did not reconnect after restart',timeout=25)
        wait(lambda:self.observer().get('mode_index')==3 and self.info()['udp']==0,'Relay mode did not survive client restart',timeout=25)
        self.command('show');self.command('hide')
        require(self.info()['pid'] and not self.observer()['mesh_visible'],'Closing main window did not preserve tray operation')
        self.command('show')
        (self.artifacts/'relay-observed.json').write_text(json.dumps(observed,indent=2))
    def pause(self):
        self.mode(0)
        wait(lambda:self.info()['udp']>0,'Automatic mode did not reopen native UDP endpoint')
        self.begin_receiver()
        self.peer.local(dict(t='button',button=1,down=True));self.peer.local(dict(t='key',code=30,down=True))
        wait(lambda:self.observer()['held_a'] and self.observer()['held_button'],'Held test inputs were not injected')
        self.command('pause')
        wait(lambda:self.peer.state and not self.peer.state['settings']['input_enabled'] and not self.peer.active(),'Global pause did not revoke active control')
        wait(lambda:not (s:=self.observer())['held_a'] and not s['held_button'] and self.info()['udp']==0,'Global pause left held input or UDP listeners')
        self.command('reset');self.command('edge',direction=1,position=.25)
        self.peer.local(dict(t='edge',dir='left',pos=.25));self.peer.local(dict(t='key',code=30,down=True))
        time.sleep(.5)
        require(not self.peer.active() and not self.observer()['events'],'Paused clients accepted new control/input')
        require(self.info()['pid'],'Global pause disconnected the client process')
        self.command('pause')
        wait(lambda:self.peer.state['settings']['input_enabled'],'Global pause could not be resumed')
        self.begin_receiver();self.release()
    def channels(self):
        self.mode(3)
        self.command('reset')
        old_enters=len(self.peer.injected('enter'))
        self.command('edge',direction=1,position=.25)
        wait(lambda:len(self.peer.injected('enter'))>old_enters,'Windows could not initiate relay control')
        require(self.peer.active()['source']==self.windows['id'],'Source was not the installed Windows client')
        before=len(self.peer.events)
        events=[dict(t='motion',dx=100,dy=0),dict(t='button',down=True),dict(t='motion',dx=5,dy=0),dict(t='button',down=False),dict(t='key',scan=30,down=True),dict(t='key',scan=30,down=False),dict(t='wheel',delta=120)]
        events += [dict(t='motion',dx=1 if n%2==0 else -1,dy=0,wait_ms=16) for n in range(240)]
        self.command('input',events=events)
        wait(lambda:len(self.peer.injected('button'))>=2 and len(self.peer.injected('key'))>=2 and self.peer.injected('wheel'),'Windows capture did not arrive at native Linux transport')
        require(self.peer.injected('button')[-2]['down'] and not self.peer.injected('button')[-1]['down'],'Captured button sequence was not preserved')
        wait(lambda:'pointer_metrics ' in self.info()['log'] and 'keyboard_metrics ' in self.info()['log'],'Per-channel diagnostics were not reported',timeout=15)
        log=self.info()['log']
        require('pointer-channel ok: 中枢转发' in log and 'keyboard-channel ok: 中枢转发' in log,'Channel states did not show relay path separately')
        require('收到按键 code=' not in log,'Production log records individual key codes')
        # The shortcut is generated through the OS and captured by the real hook.
        self.command('input',events=[dict(t='key',scan=29,down=True),dict(t='key',scan=56,down=True),dict(t='key',scan=1,down=True),dict(t='key',scan=1,down=False),dict(t='key',scan=56,down=False),dict(t='key',scan=29,down=False)])
        wait(lambda:not self.peer.active(),'Ctrl+Alt+Esc did not return control')
        (self.artifacts/'captured-transport-events.json').write_text(json.dumps(self.peer.events[before:],ensure_ascii=False,indent=2))
    def bluetooth(self):
        # Avoid launching Bluetooth helpers or asking for hardware permission on
        # the user's other computers just to test this Windows missing-radio case.
        others=[d['id'] for d in self.peer.state['devices'] if d['id'] not in (self.windows['id'],self.peer.id) and d.get('enabled',True)]
        try:
            for device in others:self.peer.send(dict(t='device_enabled',id=device,enabled=False))
            wait(lambda:all(not d.get('enabled',True) for d in self.peer.state['devices'] if d['id'] in others),'Other client fixtures did not enter temporary dormancy')
            self.mode(1)
            wait(lambda:'未检测到可用蓝牙适配器' in self.info()['log'],'Missing Bluetooth adapter was not reported',timeout=15)
            wait(lambda:self.info()['udp']==0,'Bluetooth-only mode opened UDP listeners')
            self.command('reset');self.command('edge',direction=1,position=.25)
            self.peer.local(dict(t='edge',dir='left',pos=.25))
            time.sleep(.5)
            require(not self.peer.active(),'Bluetooth-only mode silently started network control')
            observed=self.observer();require(not observed['events'],'Unavailable Bluetooth injected input')
        finally:
            self.mode(0)
            for device in others:self.peer.send(dict(t='device_enabled',id=device,enabled=True))
            wait(lambda:all(d.get('enabled',True) for d in self.peer.state['devices'] if d['id'] in others),'Other client fixtures were not restored')
        self.begin_receiver();self.release()
    def cleanup(self):
        errors=[]
        try:
            if self.peer and self.original:
                self.peer.local(dict(t='release'))
                if self.peer.active() and self.peer.id in (self.peer.active()['source'],self.peer.active()['target']):
                    self.peer.send(dict(t='settings',input_enabled=False))
                    wait(lambda:not self.peer.active(),'Test control did not stop during cleanup')
                self.peer.send(dict(t='settings',transport_mode=self.original['settings']['transport_mode'],input_enabled=self.original['settings']['input_enabled']))
                wait(lambda:self.peer.state and not self.peer.active() and self.peer.state['settings']['transport_mode']==self.original['settings']['transport_mode'] and self.peer.state['settings']['input_enabled']==self.original['settings']['input_enabled'],'Original settings were not restored')
                (self.artifacts/'windows-client.log').write_text(self.info()['log'])
        except Exception as error: errors.append(str(error))
        if self.peer:
            self.peer.close()
            try:
                with HubSocket(self.args.hub,self.args.cookie,self.args.hub_address) as viewer:
                    viewer.send(dict(t='hello',platform='web'));viewer.send(dict(t='forget',id=self.peer.id))
                    end=time.monotonic()+10
                    while time.monotonic()<end:
                        value=viewer.read(3)
                        if value['t']=='state' and not any(d['id']==self.peer.id for d in value['devices']):
                            for original in self.original['devices']:
                                current=next(d for d in value['devices'] if d['id']==original['id'])
                                for key in ('x','y','w','h'): require(current[key]==original[key],'Existing layout changed: '+original['platform'])
                            (self.artifacts/'state-after.json').write_text(json.dumps(value,ensure_ascii=False,indent=2));break
                    else: raise AssertionError('Temporary test device was not removed')
            except Exception as error: errors.append(str(error))
        if self.observer_started:
            try:self.command('quit')
            except Exception as error:errors.append('Close observer: '+str(error))
        if errors: raise RuntimeError('; '.join(errors))

def progress(value): print('AC_PROGRESS '+json.dumps(value,ensure_ascii=False),file=__import__('sys').stderr,flush=True)

def execute(args, scenarios):
    results=[];env=Environment(args);start=time.monotonic();failure=None
    progress(dict(event='phase',phase='setup',status='running',message='Connecting the real Windows client, isolated observer and native Linux transport'))
    try:
        env.prepare()
        progress(dict(event='phase',phase='setup',status='passed',message='Windows interactive session and account fixtures ready',duration_seconds=time.monotonic()-start))
        for scenario in scenarios:
            scenario_id=scenario['global_id'];key=scenario_id.rsplit('/',1)[-1]
            if key not in SCENARIOS:
                results.append(dict(scenario=scenario_id,status='skipped',error='No Windows relay implementation for this AC',duration_seconds=0));continue
            progress(dict(event='scenario',scenario=scenario_id,status='running'))
            started=time.monotonic();result=dict(scenario=scenario_id,status='passed',test_id='windows-relay/'+SCENARIOS[key])
            try:
                getattr(env,SCENARIOS[key])()
                require(not env.peer.error,'Peer connection failed: '+str(env.peer.error))
            except Exception as error:result.update(status='failed',error=str(error))
            result['duration_seconds']=time.monotonic()-started;result['debug_report']=str(args.artifacts)
            results.append(result);progress(dict(event='scenario',scenario=scenario_id,status=result['status'],duration_seconds=result['duration_seconds']))
            if result['status']=='failed':break
    except Exception as error:failure=str(error)
    finally:
        try:env.cleanup()
        except Exception as error:failure='Cleanup failed: '+str(error)+(('; '+failure) if failure else '')
    selected={r['scenario'] for r in results}
    for s in scenarios:
        if s['global_id'] not in selected:results.append(dict(scenario=s['global_id'],status='skipped',error=failure or 'Earlier scenario failed',duration_seconds=0))
    passed=not failure and all(r['status']=='passed' for r in results)
    response=dict(status='passed' if passed else 'failed',results=results)
    if failure:response['error']=failure
    (args.artifacts/'results.json').write_text(json.dumps(response,ensure_ascii=False,indent=2))
    return response

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--scenarios-file',type=Path,required=True)
    parser.add_argument('--ssh-helper',required=True)
    parser.add_argument('--copy-helper',required=True)
    parser.add_argument('--cookie-file',type=Path,required=True)
    parser.add_argument('--hub',default='lzc-mesh.ponzs.heiyu.space')
    parser.add_argument('--hub-address')
    parser.add_argument('--artifacts',type=Path,default=ROOT/'spec-tests/.run-ac/windows-parity')
    args=parser.parse_args();args.cookie=args.cookie_file.read_text().strip()
    scenarios=json.loads(args.scenarios_file.read_text())['scenarios']
    response=execute(args,scenarios)
    print(json.dumps(response,ensure_ascii=False));raise SystemExit(0 if response['status']=='passed' else 1)
