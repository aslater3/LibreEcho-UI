#!/usr/bin/env node
/*
 * Owner equaliser card on the Audio page.
 *
 * The real web/js/app.js renders the page under a small DOM shim and the
 * card's handlers run for real: the preset and boost selects and the four
 * sliders show the device's values; choosing a preset moves the sliders and
 * applies at once; moving a slider switches the preset to Custom and applies
 * after the 250 ms debounce (one request for a burst of movement); the boost
 * select applies at once; an older API without "eq" shows no card; the German
 * catalog translates the card.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function element(id) {
  const listeners = {};
  return { id, innerHTML:'', textContent:'', value:'', checked:false, disabled:false, listeners,
    attributes:{}, classList:{add(){},remove(){},toggle(){}}, style:{}, dataset:{},
    addEventListener(type, fn){ (listeners[type] = listeners[type] || []).push(fn); },
    appendChild(){}, querySelectorAll(){return [];},
    querySelector(){return null;}, closest(){return null;}, focus(){},
    setAttribute(name, value){ this.attributes[name] = String(value); }, removeAttribute(){},
    getAttribute(name){ return this.attributes[name] ?? null; },
    parentElement:{querySelector(){return element('output');}} };
}
const elements = new Map(), content = element('content');
globalThis.document = {
  querySelector(s){
    if(s==='#content')return content;
    if(!elements.has(s))elements.set(s,element(s));
    return elements.get(s);
  },
  querySelectorAll(){return [];}, createElement:element,
  addEventListener(){}, body:element('body'), activeElement:null,
  documentElement:{lang:'en'}, readyState:'complete'
};
globalThis.window = {addEventListener(){}};
globalThis.location = {pathname:'/',hash:'',host:'fixture',replace(){}};
globalThis.history = {pushState(){},replaceState(){}};
function storage(){const m=new Map();return {
  getItem:k=>m.has(k)?m.get(k):null,setItem:(k,v)=>m.set(k,String(v)),
  removeItem:k=>m.delete(k),clear:()=>m.clear()
};}
globalThis.localStorage = storage(); globalThis.sessionStorage = storage();
globalThis.confirm = () => false; globalThis.prompt = () => '';
globalThis.URL = {createObjectURL:()=>'',revokeObjectURL(){}};
/* Timers are collected and run by the test, so the debounce is observable. */
let timers = [], nextTimer = 1;
globalThis.setTimeout = (fn, ms) => { const id = nextTimer++; timers.push({id, fn, ms}); return id; };
globalThis.clearTimeout = id => { timers = timers.filter(t => t.id !== id); };
function runTimers(){ const due = timers; timers = []; due.forEach(t => t.fn()); }
globalThis.fetch = () => new Promise(()=>{});

const scripts = [...fs.readFileSync('web/index.html','utf8')
  .matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
for(const src of scripts)
  vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});
const state = vm.runInThisContext('state');

const base = {volume:50,notification_volume:50,microphone_gain:50,
  microphone_muted:false,startup_sound:false,amplifier_on:true,volume_leveling:false,
  output_available:true,tts_voice:'southern-female',noise:{},reply_volume:50,reply_volume_custom:false};
let puts = [];
const settle = () => new Promise(resolve => setImmediate(resolve));

async function render(audio){
  elements.clear(); puts = []; timers = [];
  globalThis.api = async (path, opt = {}) => {
    if(opt.method === 'PUT'){puts.push({path, body:JSON.parse(opt.body)});return Object.assign({},audio);}
    if(path === '/audio')return Object.assign({},audio);
    return {};
  };
  state.page = 'Audio'; state.renderGeneration = (state.renderGeneration||0) + 1;
  await vm.runInThisContext('audioPage')();
  timers = timers.filter(t => t.ms === 250);   /* only the card's own debounce matters here */
  return content.innerHTML;
}
const $ = s => document.querySelector(s);
const fire = (sel, type) => ($(sel).listeners[type] || []).forEach(fn => fn());
const sliders = () => ['bass','low_mid','high_mid','treble'].map(id => Number($('#eq-' + id).value));

(async () => {
  const vocal = Object.assign({}, base, {eq:{preset:'vocal',bass:-2,low_mid:1,high_mid:4,treble:1}, boost_db:3});
  let html = await render(vocal);
  assert(/<h3>Equaliser<\/h3>/.test(html), 'the Equaliser card is shown');
  assert(/<option value="vocal" selected>Vocal<\/option>/.test(html), 'the device preset is selected');
  for (const name of ['Flat','Bass boost','Treble boost','Vocal','Loudness','Late night','Warm','Custom'])
    assert(html.includes(`>${name}</option>`), `preset ${name} is offered`);
  assert(/<input id="eq-bass" class="eq-slider" type="range" min="-6" max="6" step="0.5" value="-2" aria-valuetext="−2 dB">/.test(html),
         'bass slider: range, step, value and spoken value');
  assert(/Low mid<\/span> <small class="muted">400 Hz<\/small> <output id="eq-low_mid-out">\+1 dB<\/output>/.test(html),
         'low mid label, frequency and current dB');
  assert(/High mid<\/span> <small class="muted">2.5 kHz<\/small>/.test(html), 'high mid frequency');
  assert(/<option value="3" selected>\+3 dB<\/option>/.test(html), 'the device boost is selected');
  assert(/<option value="0" >Off<\/option>/.test(html), 'boost can be switched off');
  assert(/speaker protection still limits loud passages/.test(html), 'the boost note is shown');

  /* Choosing a preset moves the sliders and applies at once. */
  $('#eq-preset').value = 'warm'; $('#eq-preset').onchange(); await settle();
  assert.deepStrictEqual(sliders(), [3, 1, -1, -3], 'Warm moves the sliders');
  assert.strictEqual($('#eq-treble-out').textContent, '−3 dB', 'the shown dB follows');
  assert.deepStrictEqual(puts.map(p => p.body), [{eq:{preset:'warm',bass:3,low_mid:1,high_mid:-1,treble:-3}}],
                         'the preset is applied at once');

  /* Moving a slider switches to Custom and applies after the debounce, once per burst. */
  puts = []; timers = [];
  for (const v of ['3.5', '4', '4.5']) { $('#eq-bass').value = v; fire('#eq-bass', 'input'); }
  assert.strictEqual($('#eq-preset').value, 'custom', 'moving a slider selects Custom');
  assert.strictEqual($('#eq-bass-out').textContent, '+4.5 dB', 'half steps are shown');
  assert.strictEqual($('#eq-bass').getAttribute('aria-valuetext'), '+4.5 dB', 'the spoken value follows');
  await settle();
  assert.strictEqual(puts.length, 0, 'nothing is sent while the slider is moving');
  assert.strictEqual(timers.length, 1, 'one pending debounce for the burst');
  assert.strictEqual(timers[0].ms, 250, 'debounced by 250 ms');
  runTimers(); await settle();
  assert.deepStrictEqual(puts.map(p => p.body), [{eq:{preset:'custom',bass:4.5,low_mid:1,high_mid:-1,treble:-3}}],
                         'one request with the final values');

  /* Speaker boost applies at once. */
  puts = [];
  $('#eq-boost').value = '6'; $('#eq-boost').onchange(); await settle();
  assert.deepStrictEqual(puts.map(p => p.body), [{boost_db:6}], 'boost is applied at once');
  puts = [];
  $('#eq-boost').value = '0'; $('#eq-boost').onchange(); await settle();
  assert.deepStrictEqual(puts.map(p => p.body), [{boost_db:0}], 'boost off is applied');

  /* Back to Flat. */
  puts = [];
  $('#eq-preset').value = 'flat'; $('#eq-preset').onchange(); await settle();
  assert.deepStrictEqual(sliders(), [0, 0, 0, 0], 'Flat zeroes the sliders');
  assert.deepStrictEqual(puts.map(p => p.body), [{eq:{preset:'flat',bass:0,low_mid:0,high_mid:0,treble:0}}], 'Flat is applied');

  /* A custom device setting shows Custom. */
  html = await render(Object.assign({}, base, {eq:{preset:'custom',bass:1.5,low_mid:0,high_mid:0,treble:-0.5}, boost_db:0}));
  assert(/<option value="custom" selected>Custom<\/option>/.test(html), 'custom is selected');
  assert(/<output id="eq-treble-out">−0.5 dB<\/output>/.test(html), 'a negative half step is shown');

  /* An older API without the equaliser shows no card. */
  html = await render(Object.assign({}, base));
  assert(!/Equaliser/.test(html) && !/eq-preset/.test(html), 'no card without eq in the audio state');

  /* German shell. */
  localStorage.setItem('libreecho-lang', 'de');
  html = await render(vocal);
  assert(/<h3>Equalizer<\/h3>/.test(html), 'German card title');
  for (const text of ['Voreinstellung', 'Untere Mitten', 'Obere Mitten', 'Höhen', 'Benutzerdefiniert',
                      'Lautsprecher-Verstärkung', '>Aus</option>', 'Lautsprecherschutz'])
    assert(html.includes(text), `German: ${text}`);
  localStorage.removeItem('libreecho-lang');
  console.log('equaliser UI: ok');
})().catch(error => { console.error(error); process.exit(1); });
