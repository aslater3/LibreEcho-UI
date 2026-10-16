'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const source=fs.readFileSync('web/js/app.js','utf8');
const page=source.slice(source.indexOf('async function wakePage()'),source.indexOf('function ledRing('));
assert(page.startsWith('async function wakePage()'));
const esc=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
async function check(word,enabled=true){
 const content={innerHTML:''},save={},test={},sensitivity={value:'73'},calls=[];
 const reported={wake_word:word,enabled,sensitivity:60,model_status:'loaded',cooldown_ms:2000};
 const context={state:{renderGeneration:1,page:'Wake Word'},content,esc,
  api:async path=>{assert.equal(path,'/wake-word');return reported},
  panel:(title,body)=>`<section>${title}${body}</section>`,
  select:()=>{throw Error('No unsupported model selector may be rendered')},
  range:()=>'<input id="sensitivity">',saveButton:()=>'<button id="save-wake">Save</button>',
  action:()=>'<button id="wake-test">Test</button>',bindRange:()=>{},
  bindDirty:(fields,target)=>{assert.deepEqual(Array.from(fields),['#sensitivity']);assert.equal(target,'#save-wake')},
  $:selector=>({'#save-wake':save,'#wake-test':test,'#sensitivity':sensitivity,'#wake-word':{value:'Custom model'}}[selector]),
  mutate:async(path,data)=>calls.push({path,data}),post:async()=>{}};
 vm.createContext(context);vm.runInContext(page,context);await context.wakePage();
 assert(content.innerHTML.includes(`<dt>Adapter-reported wake word</dt><dd>${esc(word||'Unavailable')}</dd>`));
 assert(!content.innerHTML.includes('<select'));
 assert(!content.innerHTML.includes('<option'));
 assert(content.innerHTML.includes('uploading a custom model is not supported here'));
 assert(content.innerHTML.includes('Custom voice mode changes speech recognition and synthesis, not the wake model'));
 assert(content.innerHTML.includes('select or disable the installed wake model in Home Assistant'));
 assert(content.innerHTML.includes(enabled?'Enabled':'Disabled'));
 await save.onclick();
 assert.equal(calls.length,1);assert.equal(calls[0].path,'/wake-word');
 assert.deepEqual(Object.keys(calls[0].data),['sensitivity']);assert.equal(calls[0].data.sensitivity,73);
 assert.equal(reported.wake_word,word);assert.equal(reported.enabled,enabled);
 // Even a stale/injected old selector cannot submit an unsupported model.
 assert(!Object.hasOwn(calls[0].data,'wake_word'));
}
(async()=>{
 for(const word of ['Alexa','alexa','LibreEcho','Computer','Echo','Custom model','Other <word>',''])await check(word);
 await check('Alexa',false);
 console.log('wake UI: truthful read-only label, no unsupported selection, sensitivity-only save preserves word/enablement: ok');
})().catch(error=>{console.error(error);process.exitCode=1});
