#!/usr/bin/env node
/*
 * The mute state shown on the Audio and LED & Buttons pages must not imply a lit
 * mute button lamp (0.14 #258).
 *
 * The lamp is wired to the kernel's hardware privacy latch. An image whose
 * kernel exposes the mute_lamp control lets software light it directly; one
 * without it lights the ring from a software mute and leaves the lamp dark. The
 * pages have to say which of the two is in force, must not claim software can
 * switch a lamp it cannot, and must not claim the latch is released when the
 * daemon has no fresh reading of it. Writing that control lights the lamp; it is
 * not the button's hardware privacy latch, so no copy driven by a software mute
 * may tell the user the microphones were cut in hardware.
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

const led = {colour:{r:255,g:0,b:0},brightness:60,profiles:{},night:{}};
const buttonSettings = {short_press:'Start listening',long_press:'Open pairing mode',
  hardware_mute:true,action:'sound',action_brightness:70,mute_brightness:60,
  tones:true,action_sounds:'',available_sounds:[]};

async function ledHtml(buttons){
  globalThis.api = async path => {
    if(path === '/led')return Object.assign({},led);
    if(path === '/audio')return Object.assign({},audio);
    if(path === '/buttons')return buttons;
    throw new Error('unexpected API path '+path);
  };
  await vm.runInThisContext('ledPage')();
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

  /* With the kernel's lamp control a software mute lights the lamp, and the
     note has to say so instead of describing a dark one. */
  audio.microphone_muted = true;
  const softLamp = await audioHtml({microphone_muted:true,privacy_latch:false,lamp_control:true});
  assert(/lamp is lit/.test(softLamp), 'a controllable lamp was not reported as lit');
  assert(!/stays dark/.test(softLamp), 'a controllable lamp was still described as dark');
  /* Codex review of 0.14 #258: writing the lamp line is the lamp indication, not
     the button's hardware privacy latch, so the software-mute copy must not tell
     the user the microphones were cut in hardware. */
  assert(!/cut in hardware/.test(softLamp),
         'a software mute claimed a hardware microphone cut it did not make');
  assert(/software cannot assert or release that latch/.test(softLamp),
         'the software-driven lamp was not distinguished from the hardware latch');
  audio.microphone_muted = false;
  const softLampIdle = await audioHtml({microphone_muted:false,privacy_latch:false,lamp_control:true});
  assert(!/lamp is lit/.test(softLampIdle), 'an unmuted device claimed a lit lamp');

  /* The latch outranks the software lamp even when software can drive it: while
     it is engaged the button owns the lamp, so the page must not promise that
     unmuting puts it out. */
  audio.microphone_muted = true;
  const latchedLamp = await audioHtml({microphone_muted:true,privacy_latch:true,lamp_control:true});
  assert(/Press the button to release it/.test(latchedLamp),
         'the engaged latch was not named as the only release');
  assert(!/Unmuting puts the lamp out/.test(latchedLamp),
         'software was said to own a lamp the engaged latch owns');

  /* Unmuted with no reading: nothing to say about the lamp. */
  audio.microphone_muted = false;
  const idle = await audioHtml({microphone_muted:false,privacy_latch:null});
  const idleNote = /<p class="muted" id="mute-lamp-note">([^<]*)<\/p>/.exec(idle);
  assert(idleNote && idleNote[1] === '',
         'the lamp was described while it had no state to report: '+idle);

  /*
   * The button is on the device: pressing it while the page is open moves the
   * latch, so the note has to be re-read rather than left showing the state the
   * page loaded with.
   */
  {
    globalThis.api = async path => {
      if(path === '/buttons')return {privacy_latch:true};
      if(path === '/audio')return Object.assign({}, audio);
      throw new Error('unexpected API path '+path);
    };
    /* state is a lexical binding in the page scripts, not a global property. */
    vm.runInThisContext('state.page="Audio"');
    const note = document.querySelector('#mute-lamp-note');
    note.outerHTML = '<p class="muted" id="mute-lamp-note"></p>';
    await vm.runInThisContext('refreshMuteLamp')();
    assert(/lamp is lit/.test(note.outerHTML),
           'the open page never re-read the latch: '+note.outerHTML);
  }

  /*
   * A same-page re-render (saving Audio settings calls render()) must invalidate
   * a poll that is already in flight, or the old response would overwrite the
   * new note and arm a second loop that never stops.
   */
  {
    globalThis.api = async path => {
      if(path === '/buttons')return {privacy_latch:false};
      if(path === '/audio')return Object.assign({}, audio);
      throw new Error('unexpected API path '+path);
    };
    vm.runInThisContext('state.page="Audio"');
    const note = document.querySelector('#mute-lamp-note');
    note.outerHTML = '<p class="muted" id="mute-lamp-note">untouched</p>';
    const inFlight = vm.runInThisContext('refreshMuteLamp')();
    vm.runInThisContext('state.renderGeneration=(state.renderGeneration||0)+1');
    await inFlight;
    assert(/untouched/.test(note.outerHTML),
           'a poll from before the re-render overwrote the new note: '+note.outerHTML);
  }

  /*
   * If /audio fails while /buttons answers, the note has to keep what it said:
   * replacing it with an empty one would claim nothing is muted on a device that
   * is still muted.
   */
  {
    globalThis.api = async path => {
      if(path === '/buttons')return {privacy_latch:false};
      if(path === '/audio')throw new Error('audiod unavailable');
      throw new Error('unexpected API path '+path);
    };
    vm.runInThisContext('state.page="Audio"');
    const note = document.querySelector('#mute-lamp-note');
    note.outerHTML = '<p class="muted" id="mute-lamp-note">Software mute only</p>';
    await vm.runInThisContext('refreshMuteLamp')();
    assert(/Software mute only/.test(note.outerHTML),
           'a failed audio request blanked the note: '+note.outerHTML);
  }

  /*
   * The LED & Buttons page makes the same claim in its standing copy, so it has
   * to follow the same capability: with the kernel control a software mute does
   * light the lamp, and saying software cannot switch it would be the same
   * mismatch in the other direction.
   */
  audio.microphone_muted = true;
  {
    const withLamp = await ledHtml(Object.assign({},buttonSettings,
      {microphone_muted:true,privacy_latch:false,lamp_control:true}));
    assert(!/software cannot switch it/.test(withLamp),
           'the LED page still said software cannot switch a lamp it can');
    assert(/software mute does light it/.test(withLamp),
           'the LED page did not say a software mute lights the lamp');
    assert(!/cut in hardware/.test(withLamp),
           'the LED page claimed a hardware microphone cut from the software mute');
    assert(/does not engage it/.test(withLamp),
           'the LED page did not say the software mute leaves the hardware latch alone');
    assert(/lamp is lit/.test(withLamp),
           'the LED page note did not report the lit lamp');
    /* The note is re-read while this page is open, so the poll needs the
       software mute state here too -- the buttons record cannot carry it. */
    vm.runInThisContext('state.page="LED & Buttons"');
    const ledNote = document.querySelector('#mute-lamp-note');
    ledNote.outerHTML = '<p class="muted" id="mute-lamp-note"></p>';
    await vm.runInThisContext('refreshMuteLamp')();
    assert(/lamp is lit/.test(ledNote.outerHTML),
           'the LED page never re-read the lamp: '+ledNote.outerHTML);
  }
  {
    const withoutLamp = await ledHtml(Object.assign({},buttonSettings,
      {microphone_muted:true,privacy_latch:false,lamp_control:false}));
    assert(/software cannot switch it/.test(withoutLamp),
           'an image without the control did not say the lamp follows the button');
    assert(/stays dark/.test(withoutLamp), 'the dark lamp was not named');
  }

  console.log('mute lamp honesty on the Audio and LED pages: ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
