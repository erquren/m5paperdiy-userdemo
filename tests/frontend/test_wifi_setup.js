'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const { chromium } = require('playwright');
const htmlPath = process.env.WIFI_SETUP_HTML || [path.join(__dirname, '../../main/assets/wifi_setup.html'), path.join(__dirname, '../repo/main/assets/wifi_setup.html')].find(candidate => fs.existsSync(candidate));
assert(htmlPath, 'Set WIFI_SETUP_HTML to the wifi_setup.html path');
const html = fs.readFileSync(htmlPath, 'utf8');
const screenshotDir = process.env.SCREENSHOT_DIR || __dirname;
fs.mkdirSync(screenshotDir, {recursive:true});
new vm.Script(html.match(/<script>([\s\S]*?)<\/script>/)[1]);
assert(/<form[^>]*method="post"[^>]*action="\/api\/connect"/.test(html));
assert(!/<(?:script|link|img)[^>]+(?:src|href)=['"]https?:/i.test(html));
const delay = (ms) => new Promise(resolve => setTimeout(resolve, ms));
const networks = [
  {ssid: '<img src=x onerror=alert(1)>', rssi: -40, secure: true},
  {ssid: '家里的网络', rssi: -65, secure: true},
  {ssid: 'Open network', rssi: -80, secure: false},
  {ssid: '家里的网络', rssi: -70, secure: true},
  {ssid: '', rssi: -60, secure: true},
];
let browser;
async function device(overrides = {}, viewport = {width: 390, height: 844}) {
  const model = Object.assign({state:'provisioning', message:'', ssid:'', seconds_remaining:299, scanBusy:0, scanDelay:0, connectStatus:202, offline:false, scanPostDelay:0}, overrides);
  const context = await browser.newContext({viewport, isMobile:true});
  const page = await context.newPage();
  const calls = [], errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/*', async route => {
    const request = route.request();
    const url = new URL(request.url());
    if (url.pathname === '/') return route.fulfill({status:200, contentType:'text/html; charset=utf-8', body:html});
    calls.push({path:url.pathname, url:request.url(), method:request.method(), body:request.postData()});
    const json = (body, status=200) => route.fulfill({status, contentType:'application/json', body:JSON.stringify(body)});
    if (url.pathname === '/api/session') return json({token:'test-session'});
    if (url.pathname === '/api/status') {
      if (model.offline) return route.abort('failed');
      return json({state:model.state, message:model.message, ssid:model.ssid, seconds_remaining:model.seconds_remaining});
    }
    if (url.pathname === '/api/scan' && request.method() === 'POST') {
      if (model.scanPostDelay) await delay(model.scanPostDelay);
      return json({}, 202);
    }
    if (url.pathname === '/api/scan' && request.method() === 'GET') {
      if (model.scanDelay) await delay(model.scanDelay);
      const busy = model.scanBusy-- > 0;
      return json({busy, networks});
    }
    if (url.pathname === '/api/connect') {
      if (model.connectAbort) return route.abort('failed');
      return json({}, model.connectStatus);
    }
    return json({error:'not found'}, 404);
  });
  await page.goto('http://device.test/');
  return {page, context, model, calls, errors};
}
async function ready(d) {
  await d.page.waitForFunction(() => document.querySelectorAll('.network').length === 3 && !document.getElementById('scanButton').disabled);
}
async function run(name, fn) {
  await fn(); console.log('PASS', name);
}
(async () => {
  browser = await chromium.launch({executablePath:process.env.CHROMIUM_PATH || '/usr/bin/chromium',headless:true,args:['--no-sandbox']});
  try {
    await run('Chinese mobile page, offline assets, safe SSIDs and deduplicated list', async () => {
      const d = await device(); await ready(d);
      assert.equal(await d.page.getAttribute('html','lang'), 'zh-CN');
      assert.equal(await d.page.locator('.network-name').first().textContent(), networks[0].ssid);
      assert.equal(await d.page.locator('#networks img').count(), 0);
      assert.equal(await d.page.locator('#indicator').getAttribute('data-state'), 'idle');
      assert.equal(await d.page.locator('#connectButton').isEnabled(), true);
      assert.deepEqual(JSON.parse(d.calls.find(c=>c.path==='/api/scan'&&c.method==='POST').body),{token:'test-session'});
      assert.equal(await d.page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      await d.page.screenshot({path:path.join(screenshotDir,'wifi-setup-mobile-mock-preview.png'),fullPage:true});
      assert.deepEqual(d.errors,[]); await d.context.close();
    });
    await run('Small-screen layout has no horizontal overflow', async () => {
      const d = await device({}, {width:320,height:568}); await ready(d);
      assert.equal(await d.page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      await d.page.screenshot({path:path.join(screenshotDir,'wifi-setup-small-mock-preview.png'),fullPage:true});
      await d.context.close();
    });
    await run('POST scan is awaited; repeated clicks never trigger concurrent scans', async () => {
      const d = await device({scanPostDelay:500,scanBusy:1,scanDelay:150});
      await d.page.waitForFunction(() => document.getElementById('scanButton').disabled);
      await d.page.evaluate(() => { for(let i=0;i<8;i++) document.getElementById('scanButton').dispatchEvent(new Event('click')); });
      assert.equal(d.calls.filter(c=>c.path==='/api/scan'&&c.method==='GET').length,0);
      await ready(d);
      assert.equal(d.calls.filter(c=>c.path==='/api/scan'&&c.method==='POST').length,1);
      assert.equal(d.calls.filter(c=>c.path==='/api/scan'&&c.method==='GET').length,2);
      await d.context.close();
    });
    await run('UTF-8 SSID and password validation, 64-hex support and whitespace preserved', async () => {
      const d = await device(); await ready(d);
      await d.page.fill('#ssid','中'.repeat(11)); await d.page.fill('#password','12345678'); await d.page.click('#connectButton');
      assert.equal(await d.page.locator('#ssid').getAttribute('aria-invalid'),'true');
      await d.page.fill('#ssid',' hidden '); await d.page.fill('#password','1234567'); await d.page.click('#connectButton');
      assert.equal(await d.page.locator('#password').getAttribute('aria-invalid'),'true');
      await d.page.fill('#password','中'.repeat(22)); await d.page.click('#connectButton');
      assert.equal(await d.page.locator('#password').getAttribute('aria-invalid'),'true');
      await d.page.fill('#password','g'.repeat(64)); await d.page.click('#connectButton');
      assert.equal(await d.page.locator('#password').getAttribute('aria-invalid'),'true');
      assert.equal(d.calls.filter(c=>c.path==='/api/connect').length,0);
      await d.page.fill('#password','ab'.repeat(32)); await d.page.click('#connectButton');
      await d.page.waitForFunction(() => document.getElementById('password').value === '');
      const sent = JSON.parse(d.calls.find(c=>c.path==='/api/connect').body);
      assert.equal(sent.ssid,' hidden '); assert.equal(sent.password,'ab'.repeat(32)); assert.equal(sent.token,'test-session');
      assert(d.calls.every(c=>!c.url.includes('password'))); await d.context.close();
    });
    await run('Secure selection rejects empty password, open selection sends empty password', async () => {
      const d = await device(); await ready(d);
      await d.page.locator('.network').filter({hasText:'家里的网络'}).click(); await d.page.click('#connectButton');
      assert.equal(await d.page.locator('#password').getAttribute('aria-invalid'),'true');
      await d.page.fill('#password','old-secret');
      await d.page.locator('.network').filter({hasText:'Open network'}).click();
      assert.equal(await d.page.inputValue('#password'),''); assert.equal(await d.page.locator('#password').isDisabled(),true);
      await d.page.click('#connectButton');
      await d.page.waitForFunction(() => document.getElementById('indicator').dataset.state === 'connecting');
      assert.equal(JSON.parse(d.calls.find(c=>c.path==='/api/connect').body).password,''); await d.context.close();
    });
    await run('Accepted connect clears password, handles stale provisioning and prevents resubmit', async () => {
      const d = await device(); await ready(d);
      await d.page.fill('#ssid','Hidden SSID'); await d.page.fill('#password','secret-123'); await d.page.check('#showPassword');
      await d.page.click('#connectButton'); await d.page.waitForFunction(() => document.getElementById('password').value === '');
      assert.equal(await d.page.getAttribute('#password','type'),'password');
      assert.equal(await d.page.isChecked('#showPassword'),false);
      await d.page.evaluate(() => document.getElementById('setupForm').dispatchEvent(new Event('submit',{cancelable:true})));
      await delay(1800);
      assert.equal(await d.page.locator('#indicator').getAttribute('data-state'),'connecting');
      assert.equal(d.calls.filter(c=>c.path==='/api/connect').length,1);
      assert.equal(await d.page.locator('#connectButton').isDisabled(),true);
      assert.equal((await d.page.locator('body').innerText()).includes('secret-123'),false);
      await d.context.close();
    });
    await run('Temporary AP disconnect recovers; success ends polling and safely renders SSID', async () => {
      const d = await device(); await ready(d);
      await d.page.fill('#ssid','Test'); await d.page.fill('#password','secret-123'); await d.page.click('#connectButton');
      d.model.offline=true;
      await d.page.waitForFunction(() => !document.getElementById('offline').hidden);
      d.model.offline=false; d.model.state='connected'; d.model.ssid='<script>bad()</script>'; d.model.message='secret-123';
      await d.page.waitForFunction(() => document.getElementById('indicator').dataset.state === 'connected');
      assert.equal(await d.page.locator('#offline').isHidden(),true);
      assert.equal(await d.page.locator('#connectedSsid').textContent(),'当前网络：<script>bad()</script>');
      assert.equal(await d.page.locator('#connectedSsid script').count(),0);
      assert.equal((await d.page.locator('body').innerText()).includes('secret-123'),false);
      const count=d.calls.filter(c=>c.path==='/api/status').length;
      d.model.offline=true; await delay(1800);
      assert.equal(d.calls.filter(c=>c.path==='/api/status').length,count);
      assert.equal(await d.page.locator('#offline').isHidden(),true);
      assert.deepEqual(d.errors,[]); await d.context.close();
    });
    await run('Connection failure unlocks retry and never displays raw server message', async () => {
      const d = await device(); await ready(d);
      await d.page.fill('#ssid','Test'); await d.page.fill('#password','secret-123'); await d.page.click('#connectButton');
      d.model.state='error'; d.model.message='Password was secret-123';
      await d.page.waitForFunction(() => document.getElementById('indicator').dataset.state === 'error');
      assert.equal(await d.page.locator('#connectButton').isEnabled(),true);
      assert.equal((await d.page.locator('body').innerText()).includes('secret-123'),false);
      await d.context.close();
    });
    await run('Ambiguous POST disconnect is not retried automatically; idle status allows retry', async () => {
      const d = await device({connectAbort:true}); await ready(d);
      await d.page.fill('#ssid','Test'); await d.page.fill('#password','secret-123'); await d.page.click('#connectButton');
      await d.page.waitForFunction(() => document.getElementById('indicator').dataset.state === 'connecting');
      await d.page.waitForFunction(() => document.getElementById('indicator').dataset.state === 'idle');
      assert.equal(d.calls.filter(c=>c.path==='/api/connect').length,1);
      assert.equal(await d.page.locator('#connectButton').isEnabled(),true);
      await d.context.close();
    });
    console.log('All 9 browser scenarios passed.');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode=1; });
