/* Exercise the renderer in the real page's script order (0.13.15 #237). */
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
const elements=new Map(), content=element('content');
globalThis.document={
  querySelector(s){
    if(s==='#content')return content;
    if(!elements.has(s))elements.set(s,element(s));
    return elements.get(s);
  },
  querySelectorAll(){return [];}, createElement:element,
  addEventListener(){}, body:element('body'), activeElement:null
};
globalThis.window={addEventListener(){}};
globalThis.location={pathname:'/',hash:'',host:'fixture',replace(){}};
globalThis.history={pushState(){},replaceState(){}};
function storage(){const m=new Map();return {
  getItem:k=>m.get(k)||null,setItem:(k,v)=>m.set(k,String(v)),
  removeItem:k=>m.delete(k),clear:()=>m.clear()
};}
globalThis.localStorage=storage();globalThis.sessionStorage=storage();
globalThis.confirm=()=>false;globalThis.prompt=()=>'';
// Startup remains pending; individual cases explicitly drive the page.
globalThis.fetch=()=>new Promise(()=>{});
globalThis.URL={createObjectURL:()=>'',revokeObjectURL(){}};
globalThis.setTimeout=()=>0;globalThis.clearTimeout=()=>{};
const scripts=[...fs.readFileSync('web/index.html','utf8').matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
assert(scripts.includes('/js/app.js')&&scripts.includes('/js/integrations-ui.js'));
for(const src of scripts)vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});
const render=vm.runInThisContext('integrationsPage');
const healthy={provider:'openai-compatible',provider_name:'Local LLM',base_url:'http://fixture/v1',
  enabled:true,model:'local-model',prompt:'Be brief.',home_location:'Fixture',
  latitude:'51',longitude:'0',weather_provider:'open-meteo'};
async function page(homeAssistant,assistant){
  let writes=0;
  globalThis.api=async(path,options)=>{
    if(options?.method&&options.method!=='GET')writes++;
    if(path==='/integrations')return {items:[
      {id:'home-assistant',name:'Home Assistant',enabled:homeAssistant},
      {id:'spotify',name:'Spotify',enabled:false,installed:false}
    ]};
    if(path==='/assistant'){
      if(!assistant)throw new Error('Voice assistant service is unavailable');
      return assistant;
    }
    if(path==='/voice-pipeline')return {mode:'local',stt:{},tts:{}};
    throw new Error('unexpected API path '+path);
  };
  await render();assert.equal(writes,0,'rendering must not mutate voice state');
  return content.innerHTML;
}
(async()=>{
  // Even a still-responsive local service must not offer controls in HA mode.
  for(const assistant of [null,healthy]){
    const html=await page(true,assistant);
    assert(html.includes('Managed by Home Assistant')&&html.includes('Wyoming'));
    assert(!html.includes('Voice assistant service is unavailable'));
    for(const id of ['use-local-provider','use-device-provider','save-wx'])
      assert(!html.includes('id="'+id+'"'),id+' leaked into HA mode');
    assert(html.includes('int-home-assistant'),'HA must remain disableable');
  }
  const local=await page(false,healthy);
  for(const id of ['local-base-url','local-api-key','local-model','stt-wyoming-uri','tts-wyoming-uri','save-wx'])
    assert(local.includes('id="'+id+'"'),'0.14 control lost: '+id);
  assert(!local.includes('Managed by Home Assistant'));
  assert(!local.includes('id="int-spotify"'),'uninstalled integration became actionable');
  const down=await page(false,null);
  assert(down.includes('Not supported')&&down.includes('Voice assistant service is unavailable'));
  console.log('voice assistant effective-renderer mode and 0.14 preservation: ok');
})().catch(error=>{console.error(error);process.exitCode=1;});
