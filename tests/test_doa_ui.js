#!/usr/bin/env node
/*
 * Direction-of-arrival preview on the Audio page (#61).
 *
 * The preview draws GET /microphones/beams as a polar plot viewed from above
 * with the front of the device at the bottom. API angles run clockwise from
 * the front viewed from above, so a source at 90 deg (the Mute side) has to
 * land on the left of the plot and 270 deg (Action) on the right; a plot that
 * mirrors this would point people the wrong way. It must not draw a direction
 * for a diffuse sound field, must say when no beamformed stream is running,
 * and must stop polling once the page is left or re-rendered.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function element(id) {
  const attributes = {};
  return { id, innerHTML:'', textContent:'', value:'', disabled:false, checked:false,
    classList:{add(){},remove(){},toggle(){}}, style:{}, dataset:{}, attributes,
    setAttribute(k,v){attributes[k]=String(v);}, getAttribute(k){return attributes[k];},
    addEventListener(){}, appendChild(){}, querySelectorAll(){return [];},
    closest(){return null;}, focus(){} };
}
let elements = new Map();
const content = element('content');
globalThis.document = {
  querySelector(s){
    if(s==='#content')return content;
    if(!elements.has(s))elements.set(s,element(s));
    return elements.get(s);
  },
  querySelectorAll(){return [];}, createElement:element,
  addEventListener(){}, body:element('body'), activeElement:null, hidden:false
};
globalThis.window = {addEventListener(){}};
globalThis.location = {pathname:'/',hash:'',host:'fixture',replace(){}};
globalThis.history = {pushState(){},replaceState(){}};
function storage(){const m=new Map();return {
  getItem:k=>m.get(k)||null,setItem:(k,v)=>m.set(k,String(v)),
  removeItem:k=>m.delete(k),clear:()=>m.clear()
};}
globalThis.localStorage = storage(); globalThis.sessionStorage = storage();
globalThis.confirm = () => false; globalThis.prompt = () => '';
globalThis.URL = {createObjectURL:()=>'',revokeObjectURL(){}};
let timers = [];
globalThis.setTimeout = (fn, ms) => { timers.push({fn, ms}); return timers.length; };
globalThis.clearTimeout = () => {};
globalThis.fetch = () => new Promise(()=>{});

const scripts = [...fs.readFileSync('web/index.html','utf8')
  .matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
for(const src of scripts)
  vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});
const run = code => vm.runInThisContext(code);

/* Twelve beams with a peak at peakDeg falling off by dropDb at the opposite side. */
function beams(peakDeg, dropDb, extra) {
  const list = [];
  for (let b = 0; b < 12; b++) {
    const d = Math.abs(((b * 30 - peakDeg + 540) % 360) - 180);
    list.push({deg:b*30, power_db:-40 - dropDb * d / 180, snr_db:6 - 6 * d / 180});
  }
  return Object.assign({running:true, age_ms:4, hops:1000, hop_ms:8, switches:3,
    selected_deg:peakDeg, locked:false, doa_deg:peakDeg, peak_db:-40,
    contrast_db:dropDb / 2, beams:list}, extra || {});
}
const arrowTip = () => {
  const a = document.querySelector('#doa-arrow').attributes;
  return [Number(a.x2), Number(a.y2)];
};

(async () => {
  const audio = {volume:50,notification_volume:50,microphone_gain:50,
    microphone_muted:false,startup_sound:true,amplifier_on:true,
    output_available:true,tts_voice:'southern-female',noise:{}};
  let beamReply = beams(90, 10);
  let beamCalls = 0;
  globalThis.api = async path => {
    if(path === '/audio')return Object.assign({},audio);
    if(path === '/buttons')return {microphone_muted:false,privacy_latch:false,lamp_control:true};
    if(path === '/microphones/beams'){beamCalls++;return beamReply;}
    throw new Error('unexpected API path '+path);
  };

  /* The panel is part of the Audio page and starts polling the beam API. */
  run('state.page="Audio"');
  await run('audioPage')();
  assert(/Direction of arrival/.test(content.innerHTML), 'the Audio page has no direction-of-arrival panel');
  assert(/id="doa-svg"/.test(content.innerHTML), 'the preview has no plot');
  for (const side of ['Front', 'Rear', 'Mute', 'Action'])
    assert(new RegExp('>' + side).test(content.innerHTML), 'the plot does not label the '+side+' side');
  await new Promise(r => setImmediate(r));
  assert(beamCalls >= 1, 'opening the Audio page did not read the beams');
  assert(timers.some(t => t.ms === run('DOA_POLL_MS')), 'the preview did not schedule its next poll');

  /* Mute (90 deg) is on the left of the plot, Action (270) on the right,
     front (0) at the bottom and rear (180) at the top. */
  const drawDoa = run('drawDoa');
  const cases = [[90, x => x < 40], [270, x => x > 160], [0, (x, y) => y > 160], [180, (x, y) => y < 40]];
  for (const [deg, where] of cases) {
    assert(drawDoa(beams(deg, 10)), 'drawing stopped while the panel was on screen');
    const [x, y] = arrowTip();
    assert(where(x, y), `a source at ${deg} deg pointed to (${x}, ${y})`);
    assert(/^\d+° — /.test(document.querySelector('#doa-direction').textContent),
           'a clear direction was not written out');
  }
  assert(/left \(Mute\)/.test((drawDoa(beams(90, 10)), document.querySelector('#doa-direction').textContent)),
         '90 deg was not named as the Mute side');
  assert.strictEqual((document.querySelector('#doa-wedges').innerHTML.match(/<path /g) || []).length, 12,
                     'not one wedge per beam');
  /* The strongest beam gets the longest wedge. */
  const radii = [...document.querySelector('#doa-wedges').innerHTML.matchAll(/A([\d.]+) /g)].map(m => Number(m[1]));
  assert.strictEqual(radii.indexOf(Math.max(...radii)), 3, 'the 90 deg wedge was not the longest');
  /* The UI's CSP is style-src 'self': inline style attributes are blocked. */
  for (const id of ['#doa-wedges', '#doa-trail'])
    assert(!/style=/.test(document.querySelector(id).innerHTML), id + ' uses an inline style the CSP blocks');
  assert(!/style=/.test(content.innerHTML), 'the panel uses an inline style the CSP blocks');

  /* A diffuse field: no arrow and no direction claimed. */
  drawDoa(beams(200, 2));
  const [cx, cy] = arrowTip();
  assert(cx === 100 && cy === 100, 'an arrow was drawn for a diffuse sound field');
  assert.strictEqual(document.querySelector('#doa-direction').textContent, 'No clear direction');

  /* Simulated and stale data are labelled as such. */
  drawDoa(beams(30, 10, {simulated:true}));
  assert(/Simulated/.test(document.querySelector('#doa-status').textContent), 'mock data was shown as live');
  drawDoa(beams(30, 10, {age_ms:4000}));
  assert(/stale/.test(document.querySelector('#doa-status').textContent), 'stale beam data was shown as live');

  /* No beamformed stream: idle, with the daemon's reason, and nothing drawn. */
  drawDoa({running:false, reason:'no beamformed stream is running'});
  assert(/^Idle: no beamformed stream/.test(document.querySelector('#doa-status').textContent),
         'an idle beamformer was not reported');
  assert.strictEqual(document.querySelector('#doa-wedges').innerHTML, '', 'wedges were left from a stopped stream');
  assert.strictEqual(document.querySelector('#doa-direction').textContent, '—');

  /* A failing read is shown, and polling backs off rather than stopping. */
  timers = [];
  globalThis.api = async () => { throw new Error('Microphone service is unavailable'); };
  await run('refreshDoa')(run('state.renderGeneration'), 'Audio');
  assert(/Unavailable: Microphone service/.test(document.querySelector('#doa-status').textContent),
         'a failed beam read was not shown');
  assert(timers.some(t => t.ms === 2000), 'a failed read did not back off');

  /* Leaving the page or re-rendering it ends the old poll loop. */
  timers = [];
  beamCalls = 0;
  globalThis.api = async path => { if (path === '/microphones/beams') beamCalls++; return beamReply; };
  const generation = run('state.renderGeneration');
  run('state.renderGeneration++');
  await run('refreshDoa')(generation, 'Audio');
  assert.strictEqual(beamCalls, 0, 'a superseded render kept polling the beams');
  assert.strictEqual(timers.length, 0, 'a superseded render re-armed its poll');
  run('state.page="LED & Buttons"');
  await run('refreshDoa')(run('state.renderGeneration'), 'Audio');
  assert.strictEqual(beamCalls, 0, 'leaving the Audio page did not stop the beam poll');

  console.log('Direction-of-arrival preview UI tests passed');
})().catch(e => { console.error(e); process.exit(1); });
