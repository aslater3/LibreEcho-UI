'use strict';
/* Deterministic busy cleanup, navigation races and Bluetooth draft contracts. */
const fs=require('fs'),vm=require('vm'),assert=require('assert');
function element(){return {innerHTML:'',textContent:'',value:'',checked:false,disabled:true,dataset:{},style:{setProperty(){}},classList:{add(){},remove(){},toggle(){},contains(){return false}},querySelectorAll(){return []},addEventListener(){},setAttribute(){},appendChild(){},closest(){return null}};}
function boot(){
 const nodes=new Map(),node=s=>{if(!nodes.has(s))nodes.set(s,element());return nodes.get(s)};
 const storage={getItem:()=>null,setItem(){},removeItem(){}};
 const context={console,URL,TextEncoder,TextDecoder,document:{body:element(),querySelector:node,querySelectorAll:()=>[],getElementById:id=>node('#'+id),createElement:element,addEventListener(){},activeElement:null},window:{addEventListener(){}},location:{pathname:'/',hash:'',host:'fixture',replace(){}},history:{pushState(){},replaceState(){}},sessionStorage:storage,localStorage:storage,setTimeout:()=>0,clearTimeout(){},confirm:()=>false,prompt:()=>'',fetch:()=>new Promise(()=>{})};
 vm.createContext(context);
 for(const file of ['bluetooth.js','app.js','integrations-ui.js','privacy-ui.js'])vm.runInContext(fs.readFileSync('web/js/'+file,'utf8'),context,{filename:file});
 context.state=vm.runInContext('state',context);
 return {context,node};
}
async function busyCleanup(){
 const {context:c,node}=boot();let puts=0;
 c.api=async(path,opt={})=>{if(opt.method==='PUT'){puts++;return {}}throw new Error('refresh failed')};
 await c.setLiveProvider(true,{});
 assert.strictEqual(c.state.busy,false,'rejected integrations refresh must release busy');
 assert.strictEqual(puts,1,'save should complete before the refresh fails');
 for(const id of ['#save-privacy','#save-retention']){
  c.api=async()=>({});await c.privacyPage();
  c.api=async(path,opt={})=>{if(opt.method==='PUT'){puts++;return {}}throw new Error('privacy refresh failed')};
  await node(id).onclick();
  assert.strictEqual(c.state.busy,false,id+' rejected refresh must release busy');
  assert.strictEqual(node('#toast').textContent,'privacy refresh failed');
 }
}
async function navigationRace(reject){
 const {context:c,node}=boot();let resolveDevice,rejectDevice;
 c.api=async path=>{
  if(path==='/device')return new Promise((resolve,rejection)=>{resolveDevice=resolve;rejectDevice=rejection});
  if(path==='/logs')return {entries:[],capacity:128};
  if(path==='/diagnostics')return {};
  if(path==='/light')return null;
  throw new Error('unexpected '+path);
 };
 c.state.page='Device';const old=c.render();
 c.state.page='Logs';await c.render();
 assert(node('#content').innerHTML.includes('Service logs'),'Logs should render first');
 const logs=node('#content').innerHTML;
 if(reject)rejectDevice(new Error('late Device error'));else resolveDevice({});
 await old;
 assert.strictEqual(node('#content').innerHTML,logs,'late Device '+(reject?'error':'response')+' must not overwrite Logs');
}
async function bluetoothDraft(){
 for(const id of ['#bt-pairing-pin','#bt-pairing-value'])for(const focused of [false,true]){
  const {context:c,node}=boot();c.state.page='Bluetooth';
  c.api=async()=>({enabled:true,pairing:true,pending_pairing:{address:'fixture',method:'confirm',value:123},capabilities:{}});
  node('#content').innerHTML='draft sentinel';node(id).value=focused?'':'123';
  if(focused)c.document.activeElement=node(id);
  await c.refreshBluetooth();
  assert.strictEqual(node('#content').innerHTML,'draft sentinel',id+' draft/focus must survive poll');
  assert.strictEqual(node('#bt-pairing-live').textContent,'Pairing confirmation: 000123','live pairing announcements must continue');
 }
 const {context:c,node}=boot();c.state.page='Bluetooth';c.api=async()=>({capabilities:{}});
 node('#content').innerHTML='idle sentinel';await c.refreshBluetooth();
 assert.notStrictEqual(node('#content').innerHTML,'idle sentinel','idle pairing form should refresh normally');
}
(async()=>{let failed=false;for(const [name,test] of [['busy cleanup',busyCleanup],['late page response',()=>navigationRace(false)],['late page error',()=>navigationRace(true)],['Bluetooth draft and focus',bluetoothDraft]]){try{await test();console.log('PASS '+name)}catch(error){failed=true;console.error(error)}}if(failed)process.exitCode=1;})().catch(error=>{console.error(error);process.exitCode=1});
