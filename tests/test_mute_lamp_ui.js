#!/usr/bin/env node
/*
 * The mute state shown on the Audio page must not imply a lit mute button lamp
 * (0.14 #258).
 *
 * The lamp is wired to the kernel's hardware privacy latch, so a software mute
 * lights the ring and leaves the lamp dark. The page has to say which of the two
 * is in force, and must not claim the latch is released when the daemon has no
 * fresh reading of it.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function element(id) {
  return { id, innerHTML:'', textContent:'', value:'', disabled:false,
    classList:{add(){},remove(){},toggle(){}}, style:{}, dataset:{},
    addEventListener(){}, appendChild(){}, querySelectorAll(){return [];},
    closest(){return null;}, focus(){} };
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

const audio = {volume:50,notification_volume:50,microphone_gain:50,
  microphone_muted:true,startup_sound:true,amplifier_on:true,
  output_available:true,tts_voice:'southern-female',noise:{}};

async function audioHtml(buttons){
  globalThis.api = async path => {
    if(path === '/audio')return Object.assign({},audio);
    if(path === '/buttons')return buttons;
    throw new Error('unexpected API path '+path);
  };
  await vm.runInThisContext('audioPage')();
  return content.innerHTML;
}

(async () => {
  /* Software mute with the latch released: the lamp is dark, and that is said. */
  const soft = await audioHtml({microphone_muted:true,privacy_latch:false});
  assert(/Software mute only/.test(soft),
         'a software mute did not explain the dark lamp');
  assert(/lamp stays dark/.test(soft), 'the dark lamp was not named');

  /* Latch engaged: the lamp is lit by the button, and software cannot release it. */
  const latched = await audioHtml({microphone_muted:true,privacy_latch:true});
  assert(/lamp is lit/.test(latched), 'an engaged latch was not reported as lit');
  assert(/Press the button to release it/.test(latched),
         'the only way to release the latch was not stated');
  assert(!/Software mute only/.test(latched),
         'an engaged latch was still described as a software-only mute');

  /* No fresh reading: do not claim the latch is released. */
  const unknown = await audioHtml({microphone_muted:true,privacy_latch:null});
  assert(/not part of this path/.test(unknown),
         'an unknown latch state was reported as if it had been read');
  assert(!/lamp stays dark/.test(unknown),
         'an unknown latch state claimed the lamp was dark');

  /* Unmuted with no reading: nothing to say about the lamp. */
  audio.microphone_muted = false;
  const idle = await audioHtml({microphone_muted:false,privacy_latch:null});
  assert(!/lamp/i.test(idle), 'the lamp was described while it had no state to report');

  console.log('mute lamp honesty on the Audio page: ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
