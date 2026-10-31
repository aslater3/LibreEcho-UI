#!/usr/bin/env node
/*
 * Owner sounds on the Audio page (#410): the panel lists each event with its
 * state, offers preview/select/remove only for an uploaded sound, and the
 * browser-side conversion produces the 48 kHz mono 16-bit WAV the device
 * accepts and refuses an over-long sound before uploading it.
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
  microphone_muted:false,startup_sound:true,amplifier_on:true,
  output_available:true,tts_voice:'southern-female',noise:{}};
const sounds = {rate:48000,format:'wav',max_upload_bytes:1048576,sounds:[
  {id:'wake',name:'Listening chime',selected:'custom',uploaded:true,duration_ms:400,max_ms:1000,default:'chirp'},
  {id:'end',name:'Finished-listening chime',selected:'default',uploaded:false,duration_ms:0,max_ms:2000,default:'none'},
  {id:'timer',name:'Timer and alarm ring',selected:'default',uploaded:true,duration_ms:3000,max_ms:5000,default:'two-tone'}]};

async function audioHtml(list){
  globalThis.api = async path => {
    if(path === '/audio')return Object.assign({},audio);
    if(path === '/buttons')return {};
    if(path === '/sounds'){if(list)return list;throw new Error('not found');}
    throw new Error('unexpected API path '+path);
  };
  await vm.runInThisContext('audioPage')();
  return content.innerHTML;
}

function section(html,id){
  const start=html.indexOf(`data-own-sound="${id}"`);
  assert(start>=0,'missing row for '+id);
  const next=html.indexOf('data-own-sound="',start+10);
  return html.slice(start,next<0?undefined:next);
}

(async () => {
  const html = await audioHtml(sounds);
  assert(html.includes('Your own sounds'), 'panel is rendered on the Audio page');
  const wake = section(html,'wake'), end = section(html,'end'), timer = section(html,'timer');
  assert(wake.includes('Your sound (0.4 s)') && wake.includes('Use built-in') && wake.includes('Play on device') && wake.includes('Remove'));
  assert(end.includes('No sound') && !end.includes('Play on device') && !end.includes('Remove') && !end.includes('Use mine'),
    'an event with nothing uploaded offers upload only');
  assert(timer.includes('Built-in sound · your sound (3.0 s) is kept') && timer.includes('Use mine'));
  for(const row of [wake,end,timer]){
    assert(/type="file" accept="audio\/\*,\.wav,\.opus,\.ogg/.test(row), 'file picker accepts WAV and Opus');
    assert(/data-own-sound-upload="[a-z]+" disabled/.test(row), 'upload waits for a chosen file');
  }
  assert(wake.includes('At most 1.0 s') && timer.includes('At most 5.0 s'));
  console.log('  owner sounds panel lists state and per-event actions: ok');

  const older = await audioHtml(null);
  assert(!older.includes('Your own sounds') && older.includes('Microphones'),
    'an image without the sounds API still renders the Audio page');
  console.log('  Audio page survives a daemon without /sounds: ok');

  const toWav = vm.runInThisContext('soundPcmToWav');
  const wav = toWav(Float32Array.from([0, 1, -1, 2, -0.5]));
  const view = new DataView(wav.buffer);
  const tag = at => String.fromCharCode(...wav.slice(at, at+4));
  assert.strictEqual(tag(0),'RIFF'); assert.strictEqual(tag(8),'WAVE'); assert.strictEqual(tag(36),'data');
  assert.strictEqual(view.getUint16(20,true),1);     // PCM
  assert.strictEqual(view.getUint16(22,true),1);     // mono
  assert.strictEqual(view.getUint32(24,true),48000);
  assert.strictEqual(view.getUint16(34,true),16);
  assert.strictEqual(view.getUint32(40,true),10);
  assert.deepStrictEqual([0,1,2,3,4].map(i=>view.getInt16(44+i*2,true)),[0,32767,-32768,32767,-16384]);
  console.log('  PCM is encoded as 48 kHz mono 16-bit WAV with clamping: ok');

  // A stand-in OfflineAudioContext: decode reports a duration, render returns
  // that many mono frames at 48 kHz.
  let decodedSeconds = 0.5;
  class FakeOffline {
    constructor(channels, frames, rate){ this.frames = frames; this.rate = rate; this.destination = {}; }
    async decodeAudioData(){ return {duration:decodedSeconds,length:Math.round(decodedSeconds*44100)}; }
    createBufferSource(){ return {connect(){}, start(){}}; }
    async startRendering(){ const f=this.frames; return {getChannelData:()=>new Float32Array(f).fill(0.25)}; }
  }
  globalThis.window.OfflineAudioContext = FakeOffline;
  globalThis.Blob = class { constructor(parts, opts){ this.parts = parts; this.type = opts.type; } };
  const convert = vm.runInThisContext('soundFileToWav');
  const file = {arrayBuffer: async () => new ArrayBuffer(8)};
  const blob = await convert(file, 1000);
  assert.strictEqual(blob.type, 'audio/wav');
  assert.strictEqual(blob.parts[0].length, 44 + 24000*2, 'half a second at 48 kHz mono');
  decodedSeconds = 1.5;
  await assert.rejects(convert(file, 1000), /allows at most 1\.0 s/, 'over-long sounds are refused before upload');
  console.log('  browser conversion resamples to 48 kHz and enforces the event limit: ok');
})().catch(e => { console.error(e); process.exit(1); });
