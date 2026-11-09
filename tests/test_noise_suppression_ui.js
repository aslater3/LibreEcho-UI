#!/usr/bin/env node
/*
 * #401: the Audio page offers the noise suppression switch for voice requests
 * from GET /voice-pipeline, shows it off by default, saves it through
 * PUT /voice-pipeline, and hides it when the pipeline does not report it.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function element(id) {
  return { id, innerHTML:'', textContent:'', value:'', checked:false, disabled:false,
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
  microphone_muted:false,startup_sound:true,amplifier_on:true,
  output_available:true,tts_voice:'southern-female',noise:{}};
const puts = [];

async function audioHtml(pipeline){
  puts.length = 0; elements.clear();
  globalThis.api = async (path, options) => {
    if(options && options.method === 'PUT'){puts.push([path, JSON.parse(options.body)]);return {};}
    if(path === '/audio')return Object.assign({},audio);
    if(path === '/buttons')return {};
    if(path === '/voice-pipeline'){
      if(pipeline instanceof Error)throw pipeline;
      return pipeline;
    }
    throw new Error('unexpected API path '+path);
  };
  await vm.runInThisContext('audioPage')();
  return content.innerHTML;
}

(async () => {
  const off = await audioHtml({mode:'local',listening:{max_utterance_ms:6000,
    end_silence_ms:1000,vad_floor_rms:16,noise_suppression:false}});
  assert(/Noise suppression \(experimental\)/.test(off), 'switch missing');
  assert(/id="voice-ns" type="checkbox"  /.test(off), 'switch not off by default');
  assert(/save-voice-ns/.test(off), 'save button missing');
  document.querySelector('#voice-ns').checked = true;
  vm.runInThisContext('render = async () => {}; toast = () => {};');
  await document.querySelector('#save-voice-ns').onclick();
  assert.deepStrictEqual(puts[0], ['/voice-pipeline', {noise_suppression:true}],
    'saving did not PUT only noise_suppression to /voice-pipeline');

  const on = await audioHtml({mode:'home-assistant',listening:{max_utterance_ms:6000,
    end_silence_ms:1000,vad_floor_rms:16,noise_suppression:true}});
  assert(/id="voice-ns" type="checkbox" checked/.test(on), 'enabled state not shown');

  const older = await audioHtml({mode:'local',listening:{max_utterance_ms:6000,
    end_silence_ms:1000,vad_floor_rms:16}});
  assert(!/voice-ns/.test(older), 'switch shown for a pipeline that does not report it');
  const failed = await audioHtml(new Error('unavailable'));
  assert(!/voice-ns/.test(failed), 'switch shown when /voice-pipeline failed');
  assert(/Microphone gain/.test(failed), 'Audio page broke when /voice-pipeline failed');
  console.log('noise suppression UI: ok');
})().catch(error => { console.error(error); process.exit(1); });
