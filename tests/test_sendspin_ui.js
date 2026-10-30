'use strict';
/* Drive the released integrations page for the Sendspin card. API responses
 * are fixtures: host browser-behaviour evidence only. */
const fs=require('fs'),vm=require('vm'),assert=require('assert');
function element(id) {
  return {id,value:'',checked:false,disabled:false,hidden:false,style:{},dataset:{},
    classList:{add(){},remove(){},toggle(){}},addEventListener(){},
    appendChild(){},querySelectorAll(){return [];},closest(){return null;},focus(){}};
}
const elements=new Map(),content=element('content');
const index=fs.readFileSync('web/index.html','utf8');
const staticIds=new Set([...index.matchAll(/\bid="([^"]+)"/g)].map(x=>x[1]));
let html='',strictDOM=false;
/* Sub-panels write their own innerHTML; fold it into the page so assertions
   see what the owner sees. */
function subtree(id){const e=element(id);let inner='';Object.defineProperty(e,'innerHTML',{get:()=>inner,set:v=>{
  const open='id="'+id+'"';const at=html.indexOf(open);
  if(at>=0){const start=html.indexOf('>',at)+1;const end=html.indexOf('</div>',start+inner.length);html=html.slice(0,start)+v+html.slice(end);}
  inner=v;}});return e;}
Object.defineProperty(content,'innerHTML',{get:()=>html,set:value=>{html=value;elements.clear();}});
globalThis.document={
  querySelector(s) {
    if(s==='#content')return content;
    if(strictDOM&&s.startsWith('#')&&!staticIds.has(s.slice(1))&&!html.includes('id="'+s.slice(1)+'"'))return null;
    if(!elements.has(s))elements.set(s,s==='#sendspin-detail'||s==='#sendspin-token'?subtree(s.slice(1)):element(s));
    return elements.get(s);
  },
  querySelectorAll(){return [];},createElement:element,addEventListener(){},body:element('body'),activeElement:null
};
globalThis.window={addEventListener(){}};
globalThis.location={pathname:'/',hash:'',host:'fixture',hostname:'fixture',replace(){}};
globalThis.history={pushState(){},replaceState(){}};
function storage(){const m=new Map();return {getItem:k=>m.get(k)||null,setItem:(k,v)=>m.set(k,String(v)),removeItem:k=>m.delete(k),clear:()=>m.clear(),dump:()=>[...m.values()].join('|')};}
globalThis.localStorage=storage();globalThis.sessionStorage=storage();
globalThis.confirm=()=>false;globalThis.prompt=()=>'';
globalThis.fetch=()=>new Promise(()=>{});
const scripts=[...index.matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x=>x[1]);
for(const src of scripts)vm.runInThisContext(fs.readFileSync('web'+src,'utf8'),{filename:src});
strictDOM=true;
const state=vm.runInThisContext('state'),page=vm.runInThisContext('integrationsPage');
globalThis.setTimeout=()=>0;globalThis.clearTimeout=()=>{};
globalThis.render=async()=>{};
let toasts=[];globalThis.toast=(m,bad)=>toasts.push({m,bad});vm.runInThisContext('toast=globalThis.toast');
const TOKEN='SP:TESTTOKEN&<"x">';
let integration,status,requests;
globalThis.api=async(path,options={})=>{
  const method=options.method||'GET';
  requests.push({path,method});
  if(path==='/integrations')return {items:[integration]};
  if(path==='/voice-pipeline')return {mode:'local',stt:{},tts:{}};
  if(path==='/home-assistant'||path==='/privacy'||path==='/assistant'||path==='/live')return {};
  if(path==='/sendspin'){if(status instanceof Error)throw status;return status;}
  if(path==='/sendspin/pairing'&&method==='POST')return {pairing_token:TOKEN};
  if(path==='/sendspin/unpair'&&method==='POST')return {reset:true};
  if(path.startsWith('/integrations/')||path==='/home-location')return {};
  throw Error('Unexpected API path '+path);
};
async function show(i,s){integration=i;status=s;requests=[];toasts=[];state.page='Integrations';state.busy=false;await page();}
const posts=()=>requests.filter(r=>r.path==='/sendspin/pairing');
(async()=>{
  // Disabled: listed with its toggle, no player detail, no status request.
  await show({id:'sendspin',name:'Sendspin (Music Assistant)',enabled:false,installed:true},null);
  assert(html.includes('Sendspin (Music Assistant)')&&html.includes('id="int-sendspin"'));
  assert(html.includes('Music Assistant')&&!html.includes('sendspin-detail'));
  assert.equal(requests.filter(r=>r.path==='/sendspin').length,0);

  // Not installed: no toggle, no detail.
  await show({id:'sendspin',name:'Sendspin (Music Assistant)',enabled:false,installed:false},null);
  assert(!html.includes('id="int-sendspin"')&&html.includes('Not installed in this image'));

  // Enabled and connected: status shown, server name escaped, no token yet.
  const connected={installed:true,enabled:true,running:true,state:'connected',listening:true,discoverable:true,
    connected:true,paired:true,streaming:false,time_synced:true,volume:40,muted:false,server:'Hall <b>MA</b>'};
  await show({id:'sendspin',name:'Sendspin (Music Assistant)',enabled:true,installed:true},connected);
  assert(html.includes('Connected to Hall &lt;b&gt;MA&lt;/b&gt;'),'server name not escaped');
  assert(!html.includes('<b>MA</b>'));
  assert(html.includes('id="sendspin-pair"')&&!html.includes('TESTTOKEN'));
  assert.equal(posts().length,0,'token fetched without the owner asking');

  // Show token: one POST, escaped into a read-only field, never stored.
  await document.querySelector('#sendspin-pair').onclick();
  assert.equal(posts().length,1);
  assert(html.includes('id="sendspin-token-value" readonly')&&html.includes('SP:TESTTOKEN&amp;&lt;&quot;x&quot;&gt;'));
  assert(!localStorage.dump().includes('TESTTOKEN')&&!sessionStorage.dump().includes('TESTTOKEN'));
  assert(!JSON.stringify(state).includes('TESTTOKEN'));
  document.querySelector('#sendspin-token-hide').onclick();
  assert(!html.includes('TESTTOKEN'),'Hide left the token on the page');

  // Forget paired servers: only after the owner confirms, then reported.
  const unpairs=()=>requests.filter(r=>r.path==='/sendspin/unpair');
  assert(html.includes('id="sendspin-unpair"'));
  let asked='';
  globalThis.confirm=m=>{asked=m;return false;};
  await document.querySelector('#sendspin-unpair').onclick();
  assert(asked.includes('pair')&&unpairs().length===0,'unpair sent without confirmation');
  globalThis.confirm=()=>true;
  await document.querySelector('#sendspin-unpair').onclick();
  assert.equal(unpairs().length,1);
  assert(toasts.some(t=>!t.bad&&/forgot|forgotten/i.test(t.m)),'unpair not reported');
  globalThis.confirm=()=>false;

  // Other states.
  for(const [s,text] of [[{...connected,state:'waiting',connected:false,server:null},'Waiting for Music Assistant'],
                         [{...connected,state:'playing',streaming:true},'Playing from Hall'],
                         [{...connected,state:'starting',connected:false},'Starting the player'],
                         [{...connected,paired:false},'(not paired yet)']]) {
    await show({id:'sendspin',name:'Sendspin (Music Assistant)',enabled:true,installed:true},s);
    assert(html.includes(text),'missing state text '+text);
  }
  // Status failure is reported, not thrown.
  await show({id:'sendspin',name:'Sendspin (Music Assistant)',enabled:true,installed:true},Error('Sendspin status is unavailable'));
  assert(html.includes('Sendspin status is unavailable'));
  console.log('test_sendspin_ui: Sendspin integration card PASS');
})().catch(e=>{console.error(e);process.exit(1);});
