'use strict';
/*
 * The sidebar connection widget on a deep link.
 *
 * A refresh on any route other than Overview used to leave #device-online on
 * "Connecting…" and #sidebar-uptime on "—" until the user navigated back to
 * Overview: startup fetched /status and threw the result away, and only
 * overview()/refreshOverview() ever wrote the persistent widget.
 *
 * This runs the real startup chain in index.html order against a fake device,
 * parked on a non-Overview route, and asserts the widget is correct before
 * Overview is ever rendered. It also pins that the deep link does not spend a
 * second /status request to get there, and that a failed startup status still
 * reports Unavailable.
 */
const fs = require('fs'), vm = require('vm'), assert = require('assert');

function element() {
  const classes = new Set();
  return {innerHTML:'', textContent:'', dataset:{}, disabled:false, hidden:false, value:'', checked:false,
    classList:{add:c=>classes.add(c), remove:c=>classes.delete(c), contains:c=>classes.has(c),
      toggle(c,on){if(on===undefined)on=!classes.has(c);if(on)classes.add(c);else classes.delete(c);return on;}},
    style:{setProperty(){}}, setAttribute(){}, appendChild(){}, addEventListener(){}, focus(){},
    querySelectorAll(){return []}, closest(){return null;}};
}
function storage() {const data=new Map();return {getItem:k=>data.get(k)||null,setItem:(k,v)=>data.set(k,String(v)),removeItem:k=>data.delete(k)};}

const scripts=[...fs.readFileSync('web/index.html','utf8').matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(m=>m[1]);
assert(scripts.includes('/js/app.js'));

/* Startup reports idle with a little over a day of uptime; the Overview render
   that follows reports something else, so a stale widget cannot pass by luck. */
const STARTUP_STATUS={device_state:'idle', uptime_seconds:90061, backend:'linux', simulated:false,
  cpu_percent:4, cpu_percentage:4, memory_used_mb:120, memory_total_mb:512, memory_percent:23,
  temperature_c:41, storage_used_mb:900, storage_total_mb:4096, storage_percent:22, storage_available:true,
  uptime:'1d 1h 1m'};
const OVERVIEW_STATUS={device_state:'listening', uptime_seconds:3660, backend:'linux', simulated:false,
  cpu_percent:9, cpu_percentage:9, memory_used_mb:130, memory_total_mb:512, memory_percent:25,
  temperature_c:42, storage_used_mb:900, storage_total_mb:4096, storage_percent:22, storage_available:true};
const DEVICE={name:'LibreEcho', hostname:'libreecho', model:'Echo Gen 2', serial:'fixture',
  os_version:'LibreEcho OS 0.14.0', kernel:'6.18.1', hardware_revision:'fixture', backend:'linux'};

function boot(pathname, opt={}) {
  const elements=new Map(), body=element(), requests=[];
  const node=s=>{if(!elements.has(s))elements.set(s,element());return elements.get(s);};
  /* index.html ships the widget in this state before any script runs. */
  node('#device-online').innerHTML='<span></span>Connecting…';
  node('#sidebar-uptime').textContent='Uptime: —';
  let statusCalls=0, healed=false;
  const context={console,URL,TextEncoder,TextDecoder,
    document:{body, querySelector:node, querySelectorAll:()=>[], createElement:element, addEventListener(){}},
    window:{addEventListener(){}},
    localStorage:storage(), sessionStorage:storage(),
    location:{pathname, hash:'', host:'fixture', replace(){}},
    history:{pushState(){}, replaceState(){}},
    setTimeout:()=>0, clearTimeout(){}, confirm:()=>false, prompt:()=>'',
    async fetch(path) {
      requests.push(path);
      const data=()=>{
        if(path==='/api/v1/config')return {authentication:'development-disabled',csrf_token:'fixture'};
        if(path==='/api/v1/status'){
          if(opt.statusFails&&!healed)throw new Error('Device unreachable');
          statusCalls+=1;
          return statusCalls===1?STARTUP_STATUS:OVERVIEW_STATUS;
        }
        if(path==='/api/v1/device')return DEVICE;
        if(path==='/api/v1/system/update')return {supported:true,check_status:'up-to-date'};
        if(path==='/api/v1/system/features')return {simulation:false};
        if(path==='/api/v1/light')return {available:false};
        if(path==='/api/v1/playback')return {state:'idle',source:null,metadata:{available:false}};
        if(path==='/api/v1/privacy')return {log_retention_hours:24,local_only:true};
        return {};
      };
      try {return {ok:true, status:200, json:async()=>({ok:true, data:data()})};}
      catch (error) {throw error;}
    }
  };
  vm.createContext(context);
  for (const script of scripts)
    vm.runInContext(fs.readFileSync('web'+script,'utf8'), context, {filename:script});
  return {body, node, requests, context, heal:()=>{healed=true;},
    page:()=>vm.runInContext('state.page', context),
    statusRequests:()=>requests.filter(p=>p==='/api/v1/status').length};
}
const settle=async()=>{for(let i=0;i<12;i++)await new Promise(r=>setImmediate(r));};

(async()=>{
  /* ---- deep link: the widget is correct without visiting Overview -------- */
  for (const route of ['/device','/privacy']) {
    const t=boot(route);
    assert(t.body.classList.contains('auth-pending'),'shell exposed before the first route rendered');
    await settle();
    assert(!t.body.classList.contains('auth-pending'),`${route}: startup never finished`);
    assert.equal(t.page(),route==='/device'?'Device':'Privacy',`${route}: wrong route`);
    assert.equal(t.node('#device-online').innerHTML,'<span></span>idle',
      `${route}: sidebar must show the reported device state, not Connecting`);
    assert.equal(t.node('#sidebar-uptime').textContent,'Uptime: 1d 1h 1m',
      `${route}: sidebar must show the startup uptime`);
    assert(!t.node('#device-online').classList.contains('unavailable'),`${route}: healthy widget marked unavailable`);
    assert.equal(t.statusRequests(),1,`${route}: the sidebar must reuse the startup status, not refetch it`);
  }

  /* ---- Overview still updates the shared renderer ------------------------ */
  const t=boot('/device');
  await settle();
  await vm.runInContext("showPage('Overview')", t.context);
  await settle();
  assert.equal(t.page(),'Overview');
  assert.equal(t.node('#device-online').innerHTML,'<span></span>listening','Overview must still write the widget');
  assert.equal(t.node('#sidebar-uptime').textContent,'Uptime: 1h 1m','Overview must still write the uptime');
  assert.equal(t.statusRequests(),2,'Overview is expected to fetch its own status');

  /* ---- back/forward keeps the last known state, never "Connecting…" ------ */
  t.context.location.pathname='/device';
  await vm.runInContext("showPage(pageFromLocation(),false)", t.context);
  await settle();
  assert.equal(t.page(),'Device','history navigation must land on the deep-linked route');
  assert.equal(t.node('#device-online').innerHTML,'<span></span>listening',
    'going back to a deep link must keep the last known state');
  assert.equal(t.node('#sidebar-uptime').textContent,'Uptime: 1h 1m');

  /* ---- and a failed startup status still reports Unavailable ------------- */
  const f=boot('/device',{statusFails:true});
  await settle();
  assert(f.node('#device-online').innerHTML.includes('Unavailable'),
    'a failed startup status must leave the widget Unavailable');
  assert(!f.node('#device-online').innerHTML.includes('Connecting'),
    'a failed startup status must not keep saying Connecting');
  assert(f.node('#device-online').classList.contains('unavailable'),
    'the unavailable style must be applied');
  assert.equal(f.node('#sidebar-uptime').textContent,'Uptime: unavailable');
  assert(f.node('#content').innerHTML.includes('Unable to load this section'),'the failure must be visible');

  /* ---- a reachable device clears the unavailable state again ------------- */
  f.heal();
  await vm.runInContext("showPage('Overview')", f.context);
  await settle();
  assert.equal(f.node('#device-online').innerHTML,'<span></span>idle','a recovered device must replace Unavailable');
  assert(!f.node('#device-online').classList.contains('unavailable'),'the unavailable style must be cleared');
  assert.equal(f.node('#sidebar-uptime').textContent,'Uptime: 1d 1h 1m');

  console.log('sidebar status: deep-link startup shows device state and uptime without visiting Overview: ok');
})().catch(error=>{console.error(error);process.exitCode=1;});
