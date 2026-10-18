'use strict';
// Dependency-free behavioral smoke tests. This small DOM fixture is not a layout engine.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const htmlPath = process.env.WIFI_SETUP_HTML || [path.join(__dirname, '../../main/assets/wifi_setup.html'), path.join(__dirname, '../repo/main/assets/wifi_setup.html')].find(candidate => fs.existsSync(candidate));
assert(htmlPath, 'Set WIFI_SETUP_HTML to the wifi_setup.html path');
const html = fs.readFileSync(htmlPath,'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
new vm.Script(script);
assert(/<form[^>]*method="post"[^>]*action="\/api\/connect"/.test(html));
assert(!/<(?:script|link|img)[^>]+(?:src|href)=['"]https?:/i.test(html));
class Element {
  constructor(tag='div') { this.tagName=tag;this.children=[];this.listeners={};this.attributes={};this.value='';this.hidden=false;this.disabled=false;this.checked=false;this.type='';this._text=''; }
  set textContent(value) { this.children=[];this._text=String(value); }
  get textContent() { return this._text+this.children.map(c=>c.textContent).join(''); }
  get firstChild() { return this.children[0] || null; }
  appendChild(child) { this.children.push(child);return child; }
  removeChild(child) { this.children.splice(this.children.indexOf(child),1);return child; }
  addEventListener(name,callback) { (this.listeners[name] ||= []).push(callback); }
  setAttribute(name,value) { this.attributes[name]=String(value); }
  getAttribute(name) { return this.attributes[name] ?? null; }
  removeAttribute(name) { delete this.attributes[name]; }
  focus() { this.focused=true; }
  async emit(name) { for(const listener of this.listeners[name] || []) await listener({preventDefault(){}}); }
}
const settle = async () => { for(let n=0;n<30;n++) await Promise.resolve(); };
const fixtures = [
  {ssid:'<img src=x onerror=alert(1)>',rssi:-40,secure:true},
  {ssid:'家里的网络',rssi:-65,secure:true},
  {ssid:'Open network',rssi:-80,secure:false},
  {ssid:'家里的网络',rssi:-85,secure:true},
  {ssid:'',rssi:-40,secure:true},
];
async function makeDevice(overrides={}) {
  const elements={};
  for(const match of html.matchAll(/<([\w]+)([^>]*\sid="([^"]+)"[^>]*)>/g)) {
    const e=new Element(match[1]);e.hidden=/\bhidden\b/.test(match[2]);e.disabled=/\bdisabled\b/.test(match[2]);
    for(const a of match[2].matchAll(/([\w-]+)="([^"]*)"/g))e.setAttribute(a[1],a[2]);
    e.type=e.getAttribute('type')||'';elements[match[3]]=e;
  }
  const model=Object.assign({state:'provisioning',ssid:'',message:'',seconds_remaining:299,networks:fixtures,scanBusy:0,connectStatus:202,offline:false},overrides);
  let clock=0,id=0;
  const timers=new Map(),calls=[];
  function setTimeout(callback,ms){const key=++id;timers.set(key,{at:clock+ms,callback});return key;}
  const clearTimeout=(key)=>timers.delete(key);
  async function fetch(url,options={}) {
    const method=options.method||'GET';calls.push({url,method,body:options.body});
    if(url==='/api/status'&&model.offline)throw Error('offline');
    let status=200,data;
    if(url==='/api/session') data={token:'test-token'};
    else if(url==='/api/status') {
      if(model.statusGate)await model.statusGate;
      data={state:model.state,ssid:model.ssid,message:model.message,seconds_remaining:model.seconds_remaining};
    } else if(url==='/api/scan'&&method==='POST') {if(model.scanGate)await model.scanGate;status=model.scanStatus||202;data={};}
    else if(url==='/api/scan')data={busy:model.scanBusy-->0,networks:model.networks};
    else if(url==='/api/connect') {
      if(model.connectAbort)throw Error('offline');
      if(model.connectGate)await model.connectGate;
      status=model.connectStatus;data={};
    } else throw Error('Unexpected endpoint');
    const snapshot=JSON.parse(JSON.stringify(data));
    return {ok:status>=200&&status<300,status,json:async()=>snapshot};
  }
  const context={document:{getElementById:(id)=>elements[id],createElement:(tag)=>new Element(tag)},setTimeout,clearTimeout,fetch,TextEncoder,AbortController};
  vm.runInNewContext(script,context);await settle();
  async function tick(ms) {
    const target=clock+ms;
    for (;;) {
      const due=[...timers].filter(([,t])=>t.at<=target).sort((a,b)=>a[1].at-b[1].at)[0];
      if(!due)break;
      clock=due[1].at;timers.delete(due[0]);due[1].callback();await settle();
    }
    clock=target;await settle();
  }
  const fill=async(id,value)=>{elements[id].value=value;await elements[id].emit('input');await settle();};
  const submit=async()=>{await elements.setupForm.emit('submit');await settle();};
  return {ui:elements,model,calls,tick,fill,submit,timers};
}
async function test(name,fn){await fn();console.log('PASS',name);}
(async()=>{
  await test('No external assets; form native fallback uses POST; startup provisioning is idle-ready',async()=>{
    const d=await makeDevice();assert.equal(d.ui.indicator.getAttribute('data-state'),'idle');assert.equal(d.ui.connectButton.disabled,false);assert.equal(d.ui.networks.children.length,3);
    assert.deepEqual(JSON.parse(d.calls.find(c=>c.url==='/api/scan'&&c.method==='POST').body),{token:'test-token'});
  });
  await test('SSID injection stays text, duplicate/hidden entries filtered, RSSI sorted',async()=>{
    const d=await makeDevice();const buttons=d.ui.networks.children.map(row=>row.children[0]);
    assert.equal(buttons[0].children[0].textContent,fixtures[0].ssid);assert.equal(buttons[0].children[0].children.length,0);assert.equal(buttons.length,3);
    assert.equal(buttons[2].children[1].textContent,'开放 · 弱');
  });
  await test('Scan trigger awaits POST; duplicate clicks and busy polling do not start another scan',async()=>{
    let release;const scanGate=new Promise(resolve=>{release=resolve;});const d=await makeDevice({scanGate,scanBusy:1});
    assert.equal(d.calls.filter(c=>c.url==='/api/scan'&&c.method==='GET').length,0);
    for(let i=0;i<6;i++)await d.ui.scanButton.emit('click');
    assert.equal(d.calls.filter(c=>c.url==='/api/scan'&&c.method==='POST').length,1);
    release();await settle();assert.equal(d.ui.scanButton.disabled,true);
    await d.tick(1200);assert.equal(d.calls.filter(c=>c.url==='/api/scan'&&c.method==='GET').length,2);assert.equal(d.ui.scanButton.disabled,false);
  });
  await test('SSID and password byte limits reject invalid Unicode lengths and short passwords',async()=>{
    const d=await makeDevice();
    await d.fill('ssid','');await d.submit();assert.equal(d.ui.ssid.getAttribute('aria-invalid'),'true');
    await d.fill('ssid','中'.repeat(11));await d.fill('password','12345678');await d.submit();assert.equal(d.ui.ssid.getAttribute('aria-invalid'),'true');
    await d.fill('ssid','Hidden');
    for(const password of ['1234567','中'.repeat(22),'z'.repeat(64)]){await d.fill('password',password);await d.submit();assert.equal(d.ui.password.getAttribute('aria-invalid'),'true');}
    assert.equal(d.calls.filter(c=>c.url==='/api/connect').length,0);
  });
  await test('Hidden SSIDs preserve spaces and accept 64-hex keys with token only in JSON body',async()=>{
    const d=await makeDevice();await d.fill('ssid',' Hidden ');await d.fill('password','aB'.repeat(32));await d.submit();
    const call=d.calls.find(c=>c.url==='/api/connect');assert.deepEqual(JSON.parse(call.body),{ssid:' Hidden ',password:'aB'.repeat(32),token:'test-token'});
    assert(d.calls.every(c=>!c.url.includes('?')));assert.equal(d.ui.password.value,'');
  });
  await test('8-byte and 63-byte passwords accepted; 32-byte SSID accepted',async()=>{
    for(const password of ['12345678','x'.repeat(63),'中文密码']){const d=await makeDevice();await d.fill('ssid','s'.repeat(32));await d.fill('password',password);await d.submit();assert.equal(d.calls.filter(c=>c.url==='/api/connect').length,1);}
  });
  await test('Encrypted networks require passwords; selecting open network clears old credentials',async()=>{
    const d=await makeDevice();const buttons=d.ui.networks.children.map(row=>row.children[0]);
    await buttons[1].emit('click');await d.submit();assert.equal(d.ui.password.getAttribute('aria-invalid'),'true');
    await d.fill('password','old-secret');await buttons[2].emit('click');assert.equal(d.ui.password.value,'');assert.equal(d.ui.password.disabled,true);
    await d.submit();assert.equal(JSON.parse(d.calls.find(c=>c.url==='/api/connect').body).password,'');
  });
  await test('Accepted connect clears password and visibility, locks resubmit, ignores stale provisioning',async()=>{
    const d=await makeDevice();await d.fill('ssid','Hidden');await d.fill('password','secret-123');d.ui.showPassword.checked=true;await d.ui.showPassword.emit('change');await d.submit();
    assert.equal(d.ui.password.value,'');assert.equal(d.ui.password.type,'password');assert.equal(d.ui.showPassword.checked,false);
    await d.submit();await d.tick(1500);assert.equal(d.ui.indicator.getAttribute('data-state'),'connecting');assert.equal(d.ui.connectButton.disabled,true);assert.equal(d.calls.filter(c=>c.url==='/api/connect').length,1);
  });
  await test('AP disconnect retries; success renders SSID safely, ignores server secrets and ends polling',async()=>{
    const d=await makeDevice();await d.fill('ssid','Hidden');await d.fill('password','secret-123');await d.submit();
    d.model.offline=true;await d.tick(1500);assert.equal(d.ui.offline.hidden,false);
    d.model.offline=false;d.model.state='connected';d.model.ssid='<script>bad()</script>';d.model.message='secret-123';await d.tick(1500);
    assert.equal(d.ui.indicator.getAttribute('data-state'),'connected');assert.equal(d.ui.offline.hidden,true);assert.equal(d.ui.connectedSsid.textContent,'当前网络：<script>bad()</script>');assert.equal(d.ui.connectedSsid.children.length,0);
    assert(!Object.values(d.ui).some(e=>e.textContent.includes('secret-123')));
    const count=d.calls.length;d.model.offline=true;await d.tick(10000);assert.equal(d.calls.length,count);assert.equal(d.ui.offline.hidden,true);
  });
  await test('Error status unlocks retry without echoing raw credential-bearing server messages',async()=>{
    const d=await makeDevice();await d.fill('ssid','Hidden');await d.fill('password','secret-123');await d.submit();d.model.state='error';d.model.message='password secret-123';await d.tick(1500);
    assert.equal(d.ui.connectButton.disabled,false);assert(!Object.values(d.ui).some(e=>e.textContent.includes('secret-123')));
  });
  await test('Ambiguous POST failure never retries automatically; idle status releases form',async()=>{
    const d=await makeDevice({connectAbort:true});await d.fill('ssid','Hidden');await d.fill('password','secret-123');await d.submit();assert.equal(d.ui.connectButton.disabled,true);
    await d.tick(1500);assert.equal(d.calls.filter(c=>c.url==='/api/connect').length,1);assert.equal(d.ui.connectButton.disabled,false);
  });
  await test('Rejected session token refreshes without automatically resubmitting credentials',async()=>{
    const d=await makeDevice({connectStatus:403});await d.fill('ssid','Hidden');await d.fill('password','secret-123');await d.submit();
    assert.equal(d.calls.filter(c=>c.url==='/api/session').length,2);assert.equal(d.calls.filter(c=>c.url==='/api/connect').length,1);assert.equal(d.ui.formError.hidden,false);assert.equal(d.ui.connectButton.disabled,false);
  });
  console.log('All 12 dependency-free DOM scenarios passed. Browser/layout testing is separate.');
})().catch(error=>{console.error(error);process.exitCode=1;});
