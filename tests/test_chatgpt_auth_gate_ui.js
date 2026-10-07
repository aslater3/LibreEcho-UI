'use strict';
/* Drive the released page scripts, bound controls and auth-poll re-renders.
 * API responses are fixtures: this is host browser-behavior evidence only. */
const fs=require('fs'),vm=require('vm'),assert=require('assert');
function element(id) {
  return {id,value:'',checked:false,disabled:false,style:{},dataset:{},
    classList:{add(){},remove(){},toggle(){}},addEventListener(){},
    appendChild(){},querySelectorAll(){return [];},closest(){return null;},focus(){}};
}
const elements=new Map(),content=element('content');
const index=fs.readFileSync('web/index.html','utf8');
const staticIds=new Set([...index.matchAll(/\bid="([^"]+)"/g)].map(x=>x[1]));
let html='',strictDOM=false;
Object.defineProperty(content,'innerHTML',{get:()=>html,set:value=>{html=value;elements.clear();}});
globalThis.document={
  querySelector(s) {
    if(s==='#content')return content;
    if(strictDOM&&s.startsWith('#')&&!staticIds.has(s.slice(1))&&!html.includes('id="'+s.slice(1)+'"'))return null;
    if(!elements.has(s))elements.set(s,element(s));
    return elements.get(s);
  },
  querySelectorAll(){return [];},createElement:element,addEventListener(){},body:element('body'),activeElement:null
};
globalThis.window={addEventListener(){}};
globalThis.location={pathname:'/',hash:'',host:'fixture',hostname:'fixture',replace(){}};
globalThis.history={pushState(){},replaceState(){}};
function storage(){const m=new Map();return {getItem:k=>m.get(k)||null,setItem:(k,v)=>m.set(k,String(v)),removeItem:k=>m.delete(k),clear:()=>m.clear()};}
globalThis.localStorage=storage();globalThis.sessionStorage=storage();
globalThis.confirm=()=>false;globalThis.prompt=()=>'';
globalThis.fetch=()=>new Promise(()=>{}); // prevent startup from racing the cases
const scripts=[...fs.readFileSync('web/index.html','utf8').matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
assert(scripts.includes('/js/integrations-ui.js'));
for(const src of scripts)vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});
strictDOM=true;
const state=vm.runInThisContext('state'),page=vm.runInThisContext('integrationsPage');
let timers=[],requests=[],assistant,live;
globalThis.setTimeout=(fn,ms)=>{const t={fn,ms};timers.push(t);return t;};
globalThis.clearTimeout=t=>{timers=timers.filter(x=>x!==t);};
globalThis.render=async()=>{};
function fixture(extra={}) {return Object.assign({ready:true,provider:'openai-codex',provider_name:'ChatGPT',subscription_auth:true,
  enabled:false,authenticated:false,auth_state:'signed_out',user_code:'',verification_url:'',model:'gpt-5.4',prompt:'Reply briefly.',clock_format:'12',base_url:''},extra);}
globalThis.api=async(path,options={})=>{
  const method=options.method||'GET',body=options.body?JSON.parse(options.body):null;
  requests.push({path,method,body});
  if(path==='/integrations')return {items:[]};
  if(path==='/voice-pipeline')return {mode:'local',stt:{},tts:{}};
  if(path==='/home-assistant'||path==='/privacy')return {};
  if(path==='/assistant') {
    if(!assistant)throw Error('Voice assistant service is unavailable');
    if(method==='PUT')Object.assign(assistant,body);
    return assistant;
  }
  if(path==='/live') {
    if(live.unsupported)throw Error(live.unsupported);
    if(method==='PUT')Object.assign(live,body);
    return live;
  }
  if(path==='/assistant/auth/start') {
    assert.equal(assistant.provider,'openai-codex','current daemon needs ChatGPT selected before device login');
    Object.assign(assistant,{authenticated:false,auth_state:'waiting',user_code:'WXYZ-4242',verification_url:'https://chatgpt.com/device'});
    return assistant;
  }
  if(path==='/assistant/auth/poll')return assistant;
  if(path==='/assistant/logout') {
    Object.assign(assistant,{authenticated:false,auth_state:'signed_out',user_code:'',verification_url:''});
    return assistant;
  }
  throw Error('Unexpected API path '+path);
};
async function setup(a=fixture(),l={enabled:false}) {
  assistant=a;live=l;requests=[];timers=[];state.page='Integrations';state.busy=false;
  await page();
}
function panel(id) {
  const seg=html.split('<details').find(x=>x.includes('id="'+id+'"'));
  assert(seg,'missing panel '+id);return '<details'+seg;
}
function open(id){return /\sopen/.test(panel(id).split('>')[0]);}
function label(id,text){return panel(id).includes('</span>'+text+'</span>');}
function tag(id){const m=html.match(new RegExp('<input class="toggle-input" id="'+id+'"[^>]*>'));assert(m,'missing toggle '+id);return m[0];}
function disabled(id){return /\sdisabled/.test(tag(id));}
function checked(id){return /\schecked/.test(tag(id));}
function writes(path){return requests.filter(r=>r.path===path&&r.method!=='GET');}
function noEnable(){assert.deepEqual(requests.filter(r=>r.method!=='GET'&&r.body?.enabled===true),[]);}
async function change(id){const e=document.querySelector('#'+id);assert.equal(typeof e.onchange,'function');e.checked=true;await e.onchange();}
(async()=>{
  await setup();
  for(const id of ['use-device-provider','use-live-provider']) {
    assert(label(id,'Sign in required'));assert(disabled(id));assert(open(id));
    await change(id);noEnable();
  }
  assert(!disabled('use-local-provider'));
  assert.equal(typeof document.querySelector('#assistant-auth-start').onclick,'function');
  assert.equal(typeof document.querySelector('#live-auth-start').onclick,'function');
  await vm.runInThisContext("setAssistantProvider('openai-codex',true,{})");
  await vm.runInThisContext('setLiveProvider(true,null)');
  assert.equal(requests.filter(r=>r.method==='PUT').length,0,'blocked toggles must not change other services');

  // Local endpoint readiness must not masquerade as ChatGPT authentication.
  await setup(fixture({provider:'openai-compatible',subscription_auth:false,authenticated:true,auth_state:'signed_in',enabled:true,base_url:'http://192.0.2.10:8000/v1'}));
  assert(disabled('use-device-provider')&&disabled('use-live-provider'));
  assert(!disabled('use-local-provider')&&checked('use-local-provider'));
  assert(!html.includes('id="assistant-clock-format"'));
  await document.querySelector('#live-auth-start').onclick();
  assert.deepEqual(writes('/assistant')[0].body,{provider:'openai-codex',enabled:false});
  assert.equal(writes('/assistant/auth/start').length,1);noEnable();
  assert(open('use-device-provider')&&html.includes('WXYZ-4242'));
  assert(label('use-device-provider','Waiting for sign-in'));
  assert(label('use-live-provider','Waiting for sign-in'));
  assert(!document.querySelector('#assistant-auth-start'));
  assert.equal(typeof document.querySelector('#assistant-auth-poll').onclick,'function');
  const poll=timers.find(t=>t.ms===3000);assert(poll);
  await poll.fn();
  assert.equal(writes('/assistant/auth/poll').length,1);
  assert(open('use-device-provider')&&open('use-live-provider'));
  assert(html.includes('WXYZ-4242')&&html.includes('https://chatgpt.com/device'));
  assert.equal(timers.filter(t=>t.ms===3000).length,1,'polling must not multiply on refresh');
  noEnable();
  assistant.authenticated=true;assistant.auth_state='signed_in';
  await timers.find(t=>t.ms===3000).fn();
  for(const id of ['use-device-provider','use-live-provider']) {
    assert(label(id,'Disabled'));assert(!disabled(id));assert(!open(id));
  }
  assert(!document.querySelector('#assistant-auth-poll'));
  assert.equal(timers.filter(t=>t.ms===3000).length,0);
  await change('use-device-provider');
  assert(writes('/assistant').some(r=>r.body.enabled===true));assert(label('use-device-provider','Enabled'));
  await change('use-live-provider');
  assert(writes('/live').some(r=>r.body.enabled===true));assert(label('use-live-provider','Enabled'));

  await setup(fixture(),{unsupported:'GPT-Live is not installed'});
  await document.querySelector('#assistant-auth-start').onclick();
  assert(html.includes('Cancel sign-in'));
  await document.querySelector('#assistant-logout').onclick();
  assert(!html.includes('WXYZ-4242'));
  assert.equal(timers.filter(t=>t.ms===3000).length,0);
  noEnable();
  assert(label('use-live-provider','Unavailable'));assert(disabled('use-live-provider'));assert(!open('use-live-provider'));
  assert(label('use-device-provider','Sign in required'));
  await setup(null);
  assert(label('use-live-provider','Sign-in status unavailable'));assert(disabled('use-live-provider'));
  await change('use-live-provider');noEnable();

  await setup(fixture({enabled:true}));
  assert(disabled('use-device-provider')&&checked('use-device-provider'));
  document.querySelector('#assistant-model').value='gpt-5.4';
  document.querySelector('#assistant-clock-format').value='12';
  document.querySelector('#assistant-prompt').value='Reply briefly.';
  await document.querySelector('#save-assistant').onclick();
  assert.equal(writes('/assistant')[0].body.enabled,false);noEnable();

  await setup(fixture({provider:'openai-compatible',base_url:'http://192.0.2.10:8000/v1'}));
  await change('use-local-provider');
  assert(writes('/assistant').some(r=>r.body.provider==='openai-compatible'&&r.body.enabled===true));
  console.log('ChatGPT auth-only UI: gating, sign-in, polling persistence, service states and Local LLM independence ok');
})().catch(error=>{console.error(error);process.exitCode=1;});
