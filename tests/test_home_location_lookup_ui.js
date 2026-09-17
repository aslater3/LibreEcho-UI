#!/usr/bin/env node
/*
 * The Integrations renderer must bind the Home location controls (0.14 #257).
 *
 * integrations-ui.js replaced app.js's Integrations renderer and bound the
 * card itself, but only the save button: "Look up coordinates" and the
 * stale-coordinate advisory stayed on screen with nothing attached. This test
 * drives the real page in script order and clicks the real button, so a
 * renderer that draws the controls without binding them fails here.
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
/* Startup stays blocked on a promise that never settles: only the cases below
   are driven, and they drive the page directly. */
globalThis.fetch = () => new Promise(()=>{});

const scripts = [...fs.readFileSync('web/index.html','utf8')
  .matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
assert(scripts.includes('/js/app.js') && scripts.includes('/js/integrations-ui.js'));
for(const src of scripts)
  vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});

/* UK postcodes are answered by postcodes.io; place names by the geocoder. */
const requests = [];
globalThis.fetch = async url => {
  const text = String(url);
  requests.push(text);
  if(text.startsWith('https://api.postcodes.io/postcodes/')){
    const postcode = decodeURIComponent(text.split('/').pop());
    if(postcode === 'PR1 2AB')
      return {ok:true,status:200,json:async()=>({result:{
        postcode:'PR1 2AB',latitude:53.758554,longitude:-2.703633,
        admin_district:'Preston',country:'England'}})};
    return {ok:false,status:404,json:async()=>({status:404,error:'Postcode not found'})};
  }
  const name = decodeURIComponent(text.split('name=')[1] || '');
  if(name === 'Carnforth')
    return {ok:true,status:200,json:async()=>({results:[{
      name:'Carnforth',admin1:'England',country_code:'GB',
      latitude:54.13163,longitude:-2.76914}]})};
  if(name === 'Preston')
    return {ok:true,status:200,json:async()=>({results:[
      {name:'Preston',admin1:'England',country_code:'GB',
       latitude:53.76282,longitude:-2.70452},
      {name:'Preston',admin1:'Idaho',country_code:'US',
       latitude:42.09631,longitude:-111.87662}]})};
  /* Two places that agree on every named field, differing only in position. */
  if(name === 'Springfield')
    return {ok:true,status:200,json:async()=>({results:[
      {name:'Springfield',admin1:'Illinois',country_code:'US',
       latitude:39.7817,longitude:-89.6501},
      {name:'Springfield',admin1:'Illinois',country_code:'US',
       latitude:39.9,longitude:-89.7}]})};
  return {ok:true,status:200,json:async()=>({results:[]})};
};

const assistant = {provider:'openai-compatible',provider_name:'Local LLM',
  base_url:'http://fixture/v1',enabled:true,model:'local-model',prompt:'Be brief.',
  home_location:'',latitude:'',longitude:'',weather_provider:'open-meteo'};
globalThis.api = async path => {
  if(path === '/integrations')return {items:[
    {id:'home-assistant',name:'Home Assistant',enabled:false}]};
  if(path === '/assistant')return assistant;
  if(path === '/voice-pipeline')return {mode:'local',stt:{},tts:{}};
  throw new Error('unexpected API path '+path);
};
const el = id => elements.get(id);

(async () => {
  await vm.runInThisContext('integrationsPage')();

  assert(content.innerHTML.includes('id="wx-lookup"'),
         'the Home location card must render the lookup button');
  const lookup = document.querySelector('#wx-lookup');
  assert.equal(typeof lookup.onclick,'function',
               '"Look up coordinates" was rendered with no handler');
  assert(content.innerHTML.includes('placeholder="Town or postcode"'),
         'the place field still shows a US-only example');

  /* A UK postcode has to resolve: the geocoder answers nothing for one. */
  el('#wx-location').value = 'PR1 2AB';
  await el('#wx-lookup').onclick();
  assert(requests[0].startsWith('https://api.postcodes.io/postcodes/'),
         'a UK postcode must not be sent to the place-name geocoder first');
  assert.equal(el('#wx-lat').value,'53.7586','postcode latitude not filled');
  assert.equal(el('#wx-lon').value,'-2.7036','postcode longitude not filled');
  assert(el('#wx-location').value.includes('Preston'),
         'postcode place name not set: '+el('#wx-location').value);
  assert(/Found/.test(el('#wx-lookup-note').textContent),
         'postcode lookup reported nothing: '+el('#wx-lookup-note').textContent);
  assert.equal(el('#save-wx').disabled,false,
               'a successful lookup left Save disabled');

  /*
   * A qualifier the geocoder rejects is retried, but the retry is offered, not
   * accepted: the bare name can match another region or country, and filling it
   * in silently is how one town's weather gets reported under another's name.
   */
  el('#wx-location').value = 'Carnforth, Lancashire';
  await el('#wx-lookup').onclick();
  assert.equal(el('#wx-lat').value,'53.7586',
               'the retry replaced the coordinates without asking');
  const retryNote = el('#wx-lookup-note').innerHTML;
  assert(/Nothing matched/.test(retryNote), 'the dropped qualifier was not reported');
  assert(/Carnforth, England, GB/.test(retryNote),
         'the candidate did not name its country: '+retryNote);
  assert(/wx-candidate/.test(retryNote), 'no candidate was offered: '+retryNote);

  /* An ambiguous name is offered as well, with every country spelled out. */
  el('#wx-location').value = 'Preston';
  await el('#wx-lookup').onclick();
  const ambiguous = el('#wx-lookup-note').innerHTML;
  assert(/2 places match/.test(ambiguous), 'the ambiguity was not reported: '+ambiguous);
  assert(/Preston, England, GB/.test(ambiguous) && /Preston, Idaho, US/.test(ambiguous),
         'both candidates were not offered: '+ambiguous);
  assert.equal(el('#wx-lat').value,'53.7586',
               'an ambiguous match was filled in without asking');
  /* The coordinates on screen are still the previous place's, so Save must not
     be usable until a candidate is chosen. */
  assert.equal(el('#save-wx').disabled,true,
               'Save was left enabled while a match was pending');

  /* Two places sharing every named field must still be distinguishable. */
  el('#wx-location').value = 'Springfield';
  await el('#wx-lookup').onclick();
  const twins = el('#wx-lookup-note').innerHTML;
  assert(/Springfield, Illinois, US 39\.78, -89\.65/.test(twins) &&
         /Springfield, Illinois, US 39\.90, -89\.70/.test(twins),
         'candidates with identical names were not told apart: '+twins);

  /*
   * The fallback binder (used when Home Assistant draws the page) must be able
   * to save at all, and must not save a renamed place against the old
   * coordinates: its guard reads the saved place, which used to be out of scope
   * and threw before the request.
   */
  {
    const saved = Object.assign({}, assistant, {home_location:'Old Place',
      latitude:'53.7630',longitude:'-2.7030'});
    content.innerHTML = vm.runInThisContext('weatherCard')(saved);
    let mutated = null;
    globalThis.mutate = (path, body) => { mutated = {path, body}; };
    vm.runInThisContext('bindWeather')(saved);
    document.querySelector('#save-wx').onclick();
    assert(mutated && mutated.path === '/assistant',
           'the fallback Home location card could not be saved');
    mutated = null;
    el('#wx-location').value = 'A Different Place';
    document.querySelector('#save-wx').onclick();
    assert(mutated === null,
           'the fallback binder saved a renamed place with the old coordinates');
    el('#wx-lat').value = '54.1316';
    document.querySelector('#save-wx').onclick();
    assert(mutated && mutated.body.latitude === '54.1316',
           'the fallback binder blocked a legitimate save');
  }

  /*
   * Save is shut for the whole lookup, not only once a list of candidates
   * arrives: on a slow request, saving would store this name against the
   * coordinates the previous lookup left behind.
   */
  {
    const realFetch = globalThis.fetch, realMutate = globalThis.mutate;
    let mutated = null;
    globalThis.fetch = () => new Promise(()=>{});
    globalThis.mutate = (path, body) => { mutated = {path, body}; };
    el('#wx-location').value = 'Somewhere Slow';
    el('#wx-lookup').onclick();
    assert.equal(el('#save-wx').disabled,true,
                 'Save stayed open while the lookup was in flight');
    /* bindDirty re-enables Save on the first keystroke, so the handlers must
       refuse on the pending flag rather than on the button's state. */
    el('#save-wx').disabled = false;
    el('#wx-location').value = 'Typed While Slow';
    document.querySelector('#save-wx').onclick();
    assert(mutated === null,
           'an edit during a lookup let the previous coordinates be saved');
    globalThis.fetch = realFetch;
    globalThis.mutate = realMutate;
  }

  /*
   * Two lookups in a row: the older response must not overwrite the newer one
   * when it lands late, and only the newest may touch the fields.
   */
  {
    const realFetch = globalThis.fetch;
    let releaseSlow = null;
    const slow = new Promise(resolve => { releaseSlow = resolve; });
    globalThis.fetch = url => String(url).includes('name=Slowplace')
      ? slow.then(()=>({ok:true,status:200,json:async()=>({results:[
          {name:'Slowplace',admin1:'Slowshire',country_code:'GB',
           latitude:1.0,longitude:2.0}]})}))
      : Promise.resolve({ok:true,status:200,json:async()=>({results:[
          {name:'Fastplace',admin1:'Fastshire',country_code:'GB',
           latitude:53.9,longitude:-2.9}]})});
    el('#wx-location').value = 'Slowplace';
    const slowLookup = el('#wx-lookup').onclick();
    el('#wx-location').value = 'Fastplace';
    await el('#wx-lookup').onclick();
    assert.equal(el('#wx-lat').value,'53.9000','the newer lookup did not fill');
    releaseSlow();
    await slowLookup;
    assert.equal(el('#wx-lat').value,'53.9000',
                 'a superseded lookup overwrote the newer result');
    globalThis.fetch = realFetch;
  }

  /*
   * Once candidates are on offer the card stays pending through an edit:
   * bindDirty re-enables Save, and the coordinates on screen are the previous
   * lookup's, so saving would pair them with the edited name.
   */
  {
    const realFetch = globalThis.fetch, realMutate = globalThis.mutate;
    let mutated = null;
    globalThis.mutate = (path, body) => { mutated = {path, body}; };
    el('#wx-location').value = 'Preston';
    await el('#wx-lookup').onclick();
    assert(/2 places match/.test(el('#wx-lookup-note').innerHTML),
           'the ambiguous case did not offer candidates');
    el('#save-wx').disabled = false;            /* what bindDirty does */
    el('#wx-location').value = 'Preston, edited';
    document.querySelector('#save-wx').onclick();
    assert(mutated === null,
           'an edited name was saved against an earlier lookup\'s coordinates');
    globalThis.mutate = realMutate;
    globalThis.fetch = realFetch;
  }

  /*
   * A re-render re-binds the card: a lookup that was already in flight must not
   * land in the new binding, so the sequence has to live outside it.
   */
  {
    const realFetch = globalThis.fetch;
    let release = null;
    const slow = new Promise(resolve => { release = resolve; });
    globalThis.fetch = url => String(url).includes('name=Rebind')
      ? slow.then(()=>({ok:true,status:200,json:async()=>({results:[
          {name:'Rebind',admin1:'Somewhere',country_code:'GB',
           latitude:9.9,longitude:9.9}]})}))
      : Promise.resolve({ok:true,status:200,json:async()=>({results:[]})});
    el('#wx-location').value = 'Rebind';
    const inFlight = el('#wx-lookup').onclick();
    content.innerHTML = vm.runInThisContext('weatherCard')(assistant);
    vm.runInThisContext('bindWeather')(assistant);
    release();
    await inFlight;
    assert.equal(el('#wx-lat').value,'',
                 'a lookup from before the re-render wrote into the new card');
    globalThis.fetch = realFetch;
  }

  /*
   * Rejecting the offered candidates by typing coordinates ends the pending
   * state: the user has taken over, so Save works again.
   */
  {
    const realFetch = globalThis.fetch, realMutate = globalThis.mutate;
    let mutated = null;
    globalThis.mutate = (path, body) => { mutated = {path, body}; };
    el('#wx-location').value = 'Preston';
    await el('#wx-lookup').onclick();
    assert(/wx-candidate/.test(el('#wx-lookup-note').innerHTML),
           'the ambiguous case did not offer candidates');
    el('#wx-lat').value = '53.7586';
    el('#wx-lon').value = '-2.7036';
    el('#wx-lat').oninput();          /* one half of the pair is not enough */
    el('#save-wx').disabled = false;
    document.querySelector('#save-wx').onclick();
    assert(mutated === null,
           'one coordinate edit was treated as taking over the pair');
    el('#wx-lon').oninput();
    document.querySelector('#save-wx').onclick();
    assert(mutated && mutated.path === '/assistant',
           'both coordinate edits did not end the pending offer');
    globalThis.mutate = realMutate;
    globalThis.fetch = realFetch;
  }

  /* The advisory has to be wired too: it is how a missing coordinate shows. */
  el('#wx-location').value = 'Somewhere without coordinates';
  el('#wx-lat').value = '';
  el('#wx-lon').value = '';
  el('#wx-location').oninput();
  assert(/no coordinates yet/.test(el('#wx-warn').textContent),
         'the missing-coordinate advisory never appeared: '+el('#wx-warn').textContent);

  /* And a miss has to say so rather than looking like a dead button. */
  el('#wx-location').value = 'Nowhere at all';
  await el('#wx-lookup').onclick();
  assert(/No match for that place/.test(el('#wx-lookup-note').textContent),
         'an unresolvable place did not say so: '+el('#wx-lookup-note').textContent);

  console.log('home location lookup binding and non-US lookup: ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
