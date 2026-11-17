#!/usr/bin/env node
/*
 * Separate reply volume on the Audio page (LibreEcho-UI #409).
 *
 * The real web/js/app.js renders the Output panel under a small DOM shim and
 * its Save handler runs for real: while replies follow the master the reply
 * slider is disabled and shows the master level; the switch enables it; Save
 * sends only what changed (reply_volume to set a level, reply_volume_custom
 * false to follow the master again), so an older audio service without the
 * setting still saves the other output controls.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function element(id) {
  return { id, innerHTML:'', textContent:'', value:'', checked:false, disabled:false,
    classList:{add(){},remove(){},toggle(){}}, style:{}, dataset:{},
    addEventListener(){}, appendChild(){}, querySelectorAll(){return [];},
    querySelector(){return null;}, closest(){return null;}, focus(){},
    setAttribute(){}, removeAttribute(){}, getAttribute(){return null;},
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
  addEventListener(){}, body:element('body'), activeElement:null
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
globalThis.setTimeout = () => 0; globalThis.clearTimeout = () => {};
globalThis.fetch = () => new Promise(()=>{});

const scripts = [...fs.readFileSync('web/index.html','utf8')
  .matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
for(const src of scripts)
  vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});
const state = vm.runInThisContext('state');

const base = {volume:50,notification_volume:50,microphone_gain:50,
  microphone_muted:false,startup_sound:false,amplifier_on:true,volume_leveling:false,
  output_available:true,tts_voice:'southern-female',noise:{}};
let puts = [];

async function render(audio){
  elements.clear(); puts = [];
  globalThis.api = async (path, opt = {}) => {
    if(opt.method === 'PUT'){puts.push({path, body:JSON.parse(opt.body)});return Object.assign({},audio);}
    if(path === '/audio')return Object.assign({},audio);
    if(path === '/buttons')return {};
    return {};
  };
  state.page = 'Audio'; state.renderGeneration = (state.renderGeneration||0) + 1;
  await vm.runInThisContext('audioPage')();
  return content.innerHTML;
}
const $ = s => document.querySelector(s);
async function save(controls){
  $('#volume').value = String(controls.volume);
  $('#volume-leveling').checked = false;
  $('#reply-volume-custom').checked = controls.custom;
  $('#reply-volume').value = String(controls.reply);
  state.busy = false;
  await $('#save-output').onclick();
  assert.strictEqual(puts.length, 1, 'Save sent one PUT');
  assert.strictEqual(puts[0].path, '/audio');
  return puts[0].body;
}

(async () => {
  /* Following the master (default; also an older audiod with no fields). */
  let html = await render(Object.assign({}, base, {reply_volume:50, reply_volume_custom:false}));
  assert(/Separate reply volume/.test(html), 'the switch is shown');
  assert(/Reply volume: <output>50%<\/output><\/span><input id="reply-volume" disabled/.test(html),
         'while following, the reply slider shows the master level and is disabled');
  assert(!/Notification volume/.test(html), 'the dead notification slider is gone');
  assert(/Alarms always use the master volume/.test(html), 'what the setting covers is explained');
  $('#reply-volume-custom').checked = true; $('#reply-volume-custom').onchange();
  assert.strictEqual($('#reply-volume').disabled, false, 'turning the switch on enables the slider');
  let body = await save({volume:50, custom:false, reply:50});
  assert.deepStrictEqual(body, {volume:50, volume_leveling:false},
                         'unchanged reply settings are not sent');
  html = await render(Object.assign({}, base));
  assert(/<input id="reply-volume" disabled/.test(html), 'a missing field from an older audiod means following');
  body = await save({volume:60, custom:true, reply:80});
  assert.deepStrictEqual(body, {volume:60, volume_leveling:false, reply_volume:80},
                         'switching on sends the reply level');

  /* A separate level is set. */
  html = await render(Object.assign({}, base, {reply_volume:30, reply_volume_custom:true}));
  assert(/Reply volume: <output>30%<\/output><\/span><input id="reply-volume" type="range"/.test(html),
         'a separate level is shown and editable');
  body = await save({volume:50, custom:true, reply:30});
  assert.deepStrictEqual(body, {volume:50, volume_leveling:false}, 'an unchanged level is not re-sent');
  await render(Object.assign({}, base, {reply_volume:30, reply_volume_custom:true}));
  body = await save({volume:50, custom:true, reply:45});
  assert.deepStrictEqual(body, {volume:50, volume_leveling:false, reply_volume:45}, 'a new level is sent');
  await render(Object.assign({}, base, {reply_volume:30, reply_volume_custom:true}));
  body = await save({volume:50, custom:false, reply:30});
  assert.deepStrictEqual(body, {volume:50, volume_leveling:false, reply_volume_custom:false},
                         'switching off returns replies to the master');
  console.log('reply volume UI: ok');
})().catch(error => { console.error(error); process.exit(1); });
