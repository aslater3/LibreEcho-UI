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
function voicePipeline(homeAssistant,overrides={}) {
  return {mode:homeAssistant?'home-assistant':'local',stt:{},tts:{},
    home_assistant:{protocol:'esphome',port:6053,ready:homeAssistant,connected:homeAssistant},
    restart:{state:'ready',error:''},...overrides};
}
function voiceSection(html) {
  const match=html.match(/<section class="panel setting-panel voice-assistants wide">([\s\S]*?)<\/section>/);
  assert(match,'Voice Assistants section missing');
  return match[1];
}
function haIntegration(html) {
  const match=html.match(/<details\b[^>]*><summary><h3>Home Assistant<\/h3>[\s\S]*?<\/details>/);
  assert(match,'Home Assistant integration card missing');
  return match[0];
}
async function page(homeAssistant,assistant,pipeline){
  if(arguments.length<3)pipeline=voicePipeline(homeAssistant);
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
    if(path==='/voice-pipeline'){
      if(pipeline instanceof Error)throw pipeline;
      return pipeline;
    }
    if(path==='/home-assistant')return globalThis.homeAssistantDoc||{url:'http://homeassistant.local:8123',default_url:'http://homeassistant.local:8123',encryption_key:''};
    if(path==='/live')return {enabled:false,mode:'inactive',transport:'realtime',last_event:'idle',session:{state:'idle',last_end:'none',sessions_completed:0,delegations:0},transport_metrics:{transport:'websocket',session_ready:false}};
    throw new Error('unexpected API path '+path);
  };
  await render();assert.equal(writes,0,'rendering must not mutate voice state');
  return content.innerHTML;
}
async function checkHaStatus(enabled,pipeline,status,okay=false) {
  const html=await page(enabled,healthy,pipeline);
  const dot=`<span class="status-dot ${okay?'ok':''}"></span>`;
  assert(haIntegration(html).includes(dot+'<span>'+status+'</span>'),
    'integration must report '+status+' without inferring health from the toggle');
  assert(haIntegration(html).includes('id="int-home-assistant"'),
    'status reporting must preserve the integration toggle');
  if(enabled) {
    assert(voiceSection(html).includes(dot+status),'voice must report '+status);
    assert.equal(voiceSection(html).includes('status-dot ok'),okay);
    if(!okay) {
      assert(!voiceSection(html).includes('Managed by Home Assistant'),
        'unready voice must not claim Home Assistant is managing it');
      assert(!voiceSection(html).includes('Voice is handled by Home Assistant'),
        'unready voice must not claim an active connection');
    }
  } else {
    for(const id of ['use-local-provider','use-device-provider','use-live-provider','save-wx'])
      assert(html.includes('id="'+id+'"'),
        'disabled HA must preserve '+id);
  }
  return html;
}
(async()=>{
  const rollbackError='Voice pipeline transition failed; previous pipeline could not be restored';
  const regressions=[
    ['failed activation',async()=>{
      const failed=await page(true,healthy,voicePipeline(true,{
        home_assistant:{protocol:'esphome',port:6053,ready:false,connected:false},
        restart:{state:'failed',error:rollbackError}
      }));
      assert(!voiceSection(failed).includes('status-dot ok'),
        'failed HA transition must not render a green voice status');
      assert(!haIntegration(failed).includes('status-dot ok'),
        'failed HA transition must not render a green integration status');
      assert(voiceSection(failed).includes('Failed'),'failed HA voice state must be visible');
      assert(!failed.includes('Voice is handled by Home Assistant'),
        'failed HA activation must not claim voice is being handled');
      assert(failed.includes(rollbackError),'completed rollback error must be visible');
    }],
    ['unavailable satellite',()=>checkHaStatus(true,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:false,connected:false}
    }),'Unavailable')],
    ['ready awaiting HA',()=>checkHaStatus(true,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:true,connected:false}
    }),'Ready — awaiting Home Assistant')],
    ['connected satellite',()=>checkHaStatus(true,voicePipeline(true),'Connected',true)],
    ['pending enable overrides old health',()=>checkHaStatus(true,voicePipeline(true,{
      restart:{state:'pending',error:''}
    }),'Enabling')],
    ['pending disable preserves local controls',()=>checkHaStatus(false,voicePipeline(false,{
      restart:{state:'pending',error:''}
    }),'Disabling')],
    ['failed restart overrides old health',()=>checkHaStatus(true,voicePipeline(true,{
      restart:{state:'failed',error:rollbackError}
    }),'Failed')],
    ['absent or unknown status',async()=>{
      for(const pipeline of [null,undefined,{},new Error('Status unavailable'),
          voicePipeline(true,{home_assistant:{}}),voicePipeline(true,{restart:{}}),
          voicePipeline(true,{restart:{state:'unknown',error:''}}),
          voicePipeline(true,{mode:'custom'}),voicePipeline(true,{mode:'local'}),
          voicePipeline(true,{home_assistant:{ready:'true',connected:'true'}})]) {
        await checkHaStatus(true,pipeline,'Configured — status unavailable');
      }
    }],
    ['connected without readiness is unavailable',()=>checkHaStatus(true,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:false,connected:true}
    }),'Unavailable')],
    ['disabled rollback failure and escaped errors',async()=>{
      const error=`<img src=x onerror="alert('voice')"> & rollback failed`;
      const escaped='&lt;img src=x onerror=&quot;alert(&#39;voice&#39;)&quot;&gt; &amp; rollback failed';
      for(const enabled of [true,false]) {
        const html=await checkHaStatus(enabled,voicePipeline(enabled,{
          mode:enabled?'home-assistant':'custom',restart:{state:'failed',error}
        }),'Failed');
        assert(haIntegration(html).includes(escaped),'transition/rollback error must be escaped and visible');
        assert(!html.includes(error),'raw transition/rollback markup must not reach innerHTML');
        assert(!html.includes('<img src=x'),'error must not create an HTML element');
      }
    }]
  ];
  const failures=[];
  for(const [name,check] of regressions) {
    try {await check();console.log('HA status: '+name+': ok');}
    catch(error) {failures.push(name);console.error('HA status: '+name+': '+error.message);}
  }
  assert.equal(failures.length,0,'HA status regressions: '+failures.join(', '));
  // Home Assistant onboarding: key + editable link, mirroring the ESPHome flow.
  {
    const key='q2XQ1Yc3oQ9mN0bP7t4Lr8sVw6Zy5Ae1Bc2Df3Gh4Jk=';
    globalThis.homeAssistantDoc={url:'http://homeassistant.local:8123',encryption_key:key};
    const awaiting=await page(true,healthy,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:true,connected:false}
    }));
    const voice=voiceSection(awaiting);
    assert(voice.includes('Set up in Home Assistant'),'setup call-to-action missing');
    assert(voice.includes('href="http://homeassistant.local:8123/config/integrations/dashboard/add?domain=esphome"'),'default HA link missing');
    assert(voice.includes('target="_blank" rel="noopener noreferrer"'),'HA link must open safely in a new tab');
    assert(voice.includes('id="ha-url"')&&voice.includes('value="http://homeassistant.local:8123"'),'HA address must be editable with the default');
    assert(voice.includes('data-copy-value="'+key+'"'),'encryption key must be copyable');
    assert(!voice.replace('data-copy-value="'+key+'"','').includes(key),'key must be masked until revealed');
    assert(/<details[^>]*voice-assistants|<details class="panel setting-panel integration-section assistant-mode" open>/.test(voice)||voice.includes('assistant-mode" open'),'setup must be expanded while awaiting HA');
    globalThis.homeAssistantDoc={url:'https://ha.example:8443/',encryption_key:key};
    const custom=await page(true,healthy,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:true,connected:true}
    }));
    assert(voiceSection(custom).includes('href="https://ha.example:8443/config/integrations/dashboard/add?domain=esphome"'),'saved HA address must drive the link');
    globalThis.homeAssistantDoc={url:'javascript:alert(1)',encryption_key:'"><img src=x>'};
    const hostile=await page(true,healthy,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:true,connected:false}
    }));
    assert(!voiceSection(hostile).includes('href="javascript:'),'non-http address must never become a link');
    assert(!voiceSection(hostile).includes('<img src=x>'),'key must be escaped');
    globalThis.homeAssistantDoc={url:'http://homeassistant.local:8123',encryption_key:''};
    const pending=await page(true,healthy,voicePipeline(true,{
      home_assistant:{protocol:'esphome',port:6053,ready:false,connected:false}
    }));
    assert(voiceSection(pending).includes('encryption key is generated'),'missing key must explain itself');
    globalThis.homeAssistantDoc=undefined;
    console.log('HA setup panel: ok');
  }
  // Even a still-responsive local service must not offer controls in HA mode.
  for(const assistant of [null,healthy]){
    const html=await page(true,assistant);
    assert(html.includes('Managed by Home Assistant')&&html.includes('ESPHome'));
    assert(!html.includes('Voice assistant service is unavailable'));
    for(const id of ['use-local-provider','use-device-provider','use-live-provider','save-wx'])
      assert(!html.includes('id="'+id+'"'),id+' leaked into HA mode');
    assert(html.includes('int-home-assistant'),'HA must remain disableable');
  }
  const local=await page(false,healthy);
  for(const id of ['local-base-url','local-api-key','local-model','stt-wyoming-uri','tts-wyoming-uri','save-wx','use-live-provider'])
    assert(local.includes('id="'+id+'"'),'0.14 control lost: '+id);
  assert(local.includes('GPT-Live'),'GPT-Live provider option is missing');
  assert(!local.includes('Managed by Home Assistant'));
  assert(!local.includes('id="int-spotify"'),'uninstalled integration became actionable');
  for(const mode of ['local','custom']) {
    const html=await page(false,healthy,voicePipeline(false,{
      mode,stt:{wyoming_uri:'tcp://fixture:10300',model:'saved-whisper'},
      tts:{wyoming_uri:'tcp://fixture:10200',voice:'saved-piper'}
    }));
    for(const value of ['tcp://fixture:10300','saved-whisper','tcp://fixture:10200','saved-piper'])
      assert(html.includes('value="'+value+'"'),mode+' lost saved Custom settings: '+value);
    assert(/id="use-local-provider"[^>]*checked/.test(html),mode+' lost selected Local LLM');
  }
  // Status rendering must not replace the targeted integration update with a
  // mask-wide write, nor change the existing GPT-Live disarm on HA enable.
  await page(true,healthy);
  const oldMutate=globalThis.mutate,oldApi=globalThis.api,writes=[];
  let mutation;
  globalThis.mutate=(path,body)=>{mutation={path,body};};
  globalThis.api=async(path,options)=>{
    if(options?.method&&options.method!=='GET')writes.push({path,body:JSON.parse(options.body)});
    return oldApi(path,options);
  };
  try {
    for(const enabled of [false,true]) {
      elements.get('#int-home-assistant').checked=enabled;
      await elements.get('#save-int-home-assistant').onclick();
      assert.deepStrictEqual(mutation,{path:'/integrations/home-assistant',body:{enabled}},
        'HA toggle must preserve unrelated integration bits through its targeted update');
    }
    assert.deepStrictEqual(writes,[{path:'/live',body:{enabled:false}}],
      'enabling HA must keep its existing GPT-Live disarm and no other writes');
  } finally {globalThis.mutate=oldMutate;globalThis.api=oldApi;}
  const down=await page(false,null);
  assert(down.includes('Not supported')&&down.includes('Voice assistant service is unavailable'));
  console.log('voice assistant effective-renderer mode and 0.14 preservation: ok');
})().catch(error=>{console.error(error);process.exitCode=1;});
