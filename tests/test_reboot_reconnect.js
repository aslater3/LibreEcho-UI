#!/usr/bin/env node
'use strict';
// #255: exercise the real power/reconnect functions with a fake DOM and clock.
// No server, device, dependency, or real power action is used.
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const app = fs.readFileSync('web/js/app.js', 'utf8');
const functions = app.slice(app.indexOf('async function waitForDevice('),
                            app.indexOf('async function overview('));
assert(functions.startsWith('async function waitForDevice('));

async function scenario({legacy, path='reboot', refused=false, confirmed=true,
                         busy=false, slow=false}={}) {
  let now=0, ticker=null, reloaded=false, dialog=null, polls=0, downPaints=0;
  const timers=new Map(), calls=[], messages=[];
  const status={textContent:''}, hint={textContent:''}, bar={};
  function checkWaiting() {
    if (!dialog || reloaded || status.textContent==='Back online. Reloading…')return;
    assert(!Object.hasOwn(bar,'value'), 'progress became determinate before readiness');
    assert(!/seconds? left|last boot|longer than/.test(status.textContent+hint.textContent));
    if(status.textContent==='Still restarting…')downPaints++;
  }
  const context=vm.createContext({
    state:{busy}, esc:s=>s, confirm:()=>confirmed,
    toast:(s)=>messages.push(s), AbortController,
    Date:{now:()=>now},
    document:{body:{appendChild(){}}, createElement(tag){
      assert.strictEqual(tag,'dialog');
      dialog={innerHTML:'',showModal(){},querySelector(s){
        return {'#reboot-status':status,'#reboot-hint':hint,'#reboot-progress':bar}[s];
      }};
      return dialog;
    }},
    location:{reload(){reloaded=true;}},
    setInterval(fn,ms){assert.strictEqual(ms,500);ticker=fn;return 1;},
    clearInterval(){ticker=null;},
    setTimeout(fn,ms){
      // The 2s probe timer is fired only for the deliberately hung probe.
      if(ms===2000){const id={};timers.set(id,fn);return id;}
      now+=ms;
      if(ticker)ticker();
      checkWaiting();
      fn();return 0;
    },
    clearTimeout(id){timers.delete(id);},
    async api(p,options){
      calls.push(p);
      if(p==='/system')return legacy; // Old/missing estimates must never be read.
      assert.strictEqual(p,`/system/${path}`);
      assert.strictEqual(options.method,'POST');
      assert.strictEqual(options.headers['X-LibreEcho-Confirm'],'confirm-device-action');
      if(refused)throw new Error('Request failed (403)');
      return {accepted:true};
    },
    async fetch(p,options){
      assert.strictEqual(p,'/healthz');
      assert.strictEqual(options.cache,'no-store');
      assert(options.signal);
      polls++;
      checkWaiting();
      if(polls===1)return {ok:true}; // Still-up is not readiness after restart.
      if(polls===2 && slow){
        const pending=new Promise((resolve,reject)=>{
          options.signal.addEventListener('abort',()=>reject(new Error('aborted')));
        });
        const timer=[...timers.values()][0];
        assert(timer, 'probe had no deadline');timer();
        return pending;
      }
      if(polls===2)throw new Error('connection refused');
      // Simulate a restart beyond the previous 23/45/60/120s estimates.
      if(polls===3){now=600000;if(ticker)ticker();checkWaiting();return {ok:false};}
      return {ok:true};
    }
  });
  vm.runInContext(functions,context,{timeout:1000});
  await context.power(path,'Restart');
  assert(!calls.includes('/system'), 'untrusted restart estimate was fetched');
  if(!confirmed || busy){assert.strictEqual(calls.length,0);assert.strictEqual(dialog,null);return;}
  if(refused || path==='shutdown'){
    assert.strictEqual(dialog,null);assert.strictEqual(polls,0);assert(messages.length);return;
  }
  assert(reloaded, 'down/up transition did not reload');
  assert.strictEqual(polls,4);
  assert(downPaints>0, 'missing honest still-restarting message');
  assert.strictEqual(status.textContent,'Back online. Reloading…');
  assert.strictEqual(bar.value,1000);
  assert(!/<progress[^>]*\bvalue=/.test(dialog.innerHTML), 'initial progress was determinate');
  assert.strictEqual(ticker,null, 'display timer leaked');
  assert.strictEqual(timers.size,0, 'probe timer leaked');
}

(async()=>{
  // No measurement, older-build/stale measurements, uptime-derived estimates,
  // and a longer replacement must all be ignored on the indeterminate path.
  for(const legacy of [undefined,{boot_estimate_seconds:null},
      {boot_estimate_seconds:23},{boot_estimate_seconds:45,build:'old'},
      {boot_estimate_seconds:60},{boot_estimate_seconds:120}])await scenario({legacy});
  await scenario({slow:true});
  await scenario({path:'factory-reset'});
  await scenario({path:'shutdown'});
  await scenario({refused:true});
  await scenario({confirmed:false});
  await scenario({busy:true});
  // Source contract: daemon-only starts cannot manufacture an estimate.
  const api=fs.readFileSync('src/api.c','utf8');
  assert(!api.includes('measure_boot_seconds') && !api.includes('LE_BOOT_ESTIMATE_'));
  assert(!api.includes('/proc/uptime'));
  assert(api.includes('\\"boot_estimate_seconds\\":null'));
  assert(!fs.readFileSync('src/api.h','utf8').includes('boot_estimate_seconds'));
  const spec=JSON.parse(fs.readFileSync('web/openapi.json','utf8'));
  assert(spec.paths['/system'].get.description.includes('boot_estimate_seconds field is always null'));
  assert(fs.readFileSync('docs/API.md','utf8').includes('"boot_estimate_seconds": null'));
  assert(fs.readFileSync('tests/run_tests.sh','utf8').includes('node tests/test_reboot_reconnect.js'));
  console.log('reboot reconnect regression: PASS (indeterminate progress, legacy estimates ignored, bounded probes, down/up readiness, power guards, API/docs contract)');
})().catch(error=>{console.error(error);process.exitCode=1;});
