// Private, headless Chromium integration target. CDP is loopback-only and is
// closed on stdin EOF; no existing browser profile or user session is touched.
import {spawn} from 'node:child_process';
import {mkdtempSync,rmSync,writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createInterface} from 'node:readline';
const directory=mkdtempSync(join(tmpdir(),'lzc-mesh-browser-'));
const chrome=spawn('google-chrome-stable',['--headless=new','--no-sandbox','--disable-gpu',`--user-data-dir=${directory}`,'--remote-debugging-port=0','about:blank'],{stdio:['ignore','ignore','pipe']});
let ws;
try {
 const url=await new Promise((resolve,reject)=>{
  const timeout=setTimeout(()=>reject(new Error('Chromium startup timed out')),15000);
  chrome.stderr.on('data',data=>{const match=String(data).match(/ws:\/\/\S+/);if(match){clearTimeout(timeout);resolve(match[0]);}});
  chrome.on('error',reject);
 });
 ws=new WebSocket(url);await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
 let next=0;const pending=new Map();let nextDialog;
 ws.onmessage=event=>{
  const message=JSON.parse(event.data);
  if(message.method==='Page.javascriptDialogOpening' && nextDialog!==undefined){const accept=nextDialog;nextDialog=undefined;call('Page.handleJavaScriptDialog',{accept},message.sessionId).catch(()=>{});}
  if(pending.has(message.id)){pending.get(message.id)(message);pending.delete(message.id);}
 };
 const call=(method,params={},sessionId)=>new Promise((resolve,reject)=>{
  const id=++next;const timeout=setTimeout(()=>{pending.delete(id);reject(new Error('CDP command timed out: '+method));},15000);
  pending.set(id,result=>{clearTimeout(timeout);result.error?reject(new Error(JSON.stringify(result.error))):resolve(result.result);});
  ws.send(JSON.stringify({id,method,params,sessionId}));
 });
 const {targetId}=await call('Target.createTarget',{url:'about:blank'});
 const {sessionId}=await call('Target.attachToTarget',{targetId,flatten:true});
 const target=(method,params)=>call(method,params,sessionId);
 await target('Network.enable');
 await target('Network.setExtraHTTPHeaders',{headers:{'X-HC-User-ID':'ui-test'}});
 await target('Emulation.setDeviceMetricsOverride',{width:1280,height:1000,deviceScaleFactor:1,mobile:false});
 await target('Page.enable');
 await target('Page.navigate',{url:process.argv[2]});
 console.log(JSON.stringify({ready:true}));
 for await (const line of createInterface({input:process.stdin,crlfDelay:Infinity})) {
  let command;
  try {
   command=JSON.parse(line);let result;
   if(command.op==='eval') {
    const value=await target('Runtime.evaluate',{expression:command.expression,returnByValue:true,awaitPromise:true});
    if(value.exceptionDetails)throw new Error(value.exceptionDetails.text);
    result=value.result.value;
   } else if(command.op==='confirm') {nextDialog=!!command.accept;result=true;
   } else if(command.op==='key') {
    for(const type of ['keyDown','keyUp']) await target('Input.dispatchKeyEvent',{type,key:command.key,code:command.code,windowsVirtualKeyCode:command.vk,nativeVirtualKeyCode:command.vk});
    result=true;
   } else if(command.op==='viewport') {
    result=await target('Emulation.setDeviceMetricsOverride',{width:command.width,height:command.height,deviceScaleFactor:1,mobile:!!command.mobile});
    await target('Emulation.setTouchEmulationEnabled',{enabled:!!command.mobile});
   } else if(command.op==='touch') result=await target('Input.dispatchTouchEvent',{type:command.type,touchPoints:command.points??[]});
   else if(command.op==='text') result=await target('Input.insertText',{text:command.text});
   else if(command.op==='color') result=await target('Emulation.setEmulatedMedia',{features:[{name:'prefers-color-scheme',value:command.scheme}]});
   else if(command.op==='shot') {const shot=await target('Page.captureScreenshot',{format:'png'});writeFileSync(command.path,Buffer.from(shot.data,'base64'));result=true;}
   else if(command.op==='reload') result=await target('Page.reload');
   else if(command.op==='quit') break;
   else throw new Error('Unknown browser test operation');
   console.log(JSON.stringify({id:command.id,result:result??null}));
  } catch(error) {console.log(JSON.stringify({id:command?.id,error:String(error)}));}
 }
 await call('Browser.close').catch(()=>{});
} finally {
 ws?.close();chrome.kill();
 await new Promise(resolve=>setTimeout(resolve,300));
 rmSync(directory,{recursive:true,force:true});
}
