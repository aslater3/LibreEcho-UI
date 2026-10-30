/* UI#391: health/usage and crash reporting do not exist in this release, so
   the Privacy page, the setup screen and the setup review must not imply that
   anything is (or is not) sent because of those switches, and the stored
   consent values must still round-trip unchanged. */
'use strict';
const assert=require('assert'),fs=require('fs'),vm=require('vm');
const source=fs.readFileSync('web/js/privacy-ui.js','utf8');
async function render(privacy){
  const elements=new Map(),writes=[],content={innerHTML:''},state={busy:false};
  const $=id=>{if(!elements.has(id))elements.set(id,{value:'',checked:false});return elements.get(id);};
  const data={'/privacy':privacy,'/assistant':{provider:'openai-codex',enabled:false},'/voice-pipeline':{mode:'local',stt:{},tts:{}},'/live':{enabled:false}};
  const context={content,state,$,console,bindDirty(){},setBusy:v=>{state.busy=v;},toast(){},
    esc:v=>String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])),
    panel:(title,body)=>title+body,select:()=>'',field:()=>'',saveButton:()=>'',
    toggle:(label,on,id)=>`<label data-id="${id}" data-on="${on?1:0}">${label}</label>`,
    api:async(path,options)=>{if(options?.method==='PUT'){const body=JSON.parse(options.body);writes.push({path,body});data[path]={...data[path],...body};return data[path];}return data[path];}};
  vm.createContext(context);vm.runInContext(source,context,{filename:'privacy-ui.js'});
  await context.privacyPage();
  return {html:content.innerHTML,writes,$};
}
(async()=>{
  for(const [telemetry,crash] of [[true,true],[false,false],[true,false]]){
    const page=await render({local_only:true,audio_retention:'none',log_retention_hours:24,diagnostic_telemetry:telemetry,crash_reports:crash});
    const block=page.html.slice(page.html.indexOf('id="future-reporting"'));
    assert(page.html.includes('id="future-reporting"'),'future reporting block rendered');
    assert(!/Help improve LibreEcho/.test(page.html),'no switch may suggest the owner is currently helping by sending data');
    assert(!/Diagnostic telemetry/.test(page.html),'no bare telemetry switch');
    assert((block.match(/Not available in this release: nothing is sent/g)||[]).length===2,'both reports state that nothing is sent');
    assert(block.includes('nothing is sent whether these switches are on or off'),'callout says the switch changes nothing today');
    assert(block.includes(`data-id="telemetry" data-on="${telemetry?1:0}">Allow anonymous health and usage reports if a future release adds them`),'telemetry switch is labelled as future consent and reflects the stored value');
    assert(block.includes(`data-id="crash-reports" data-on="${crash?1:0}">Allow anonymous crash reports if a future release adds them`),'crash switch is labelled as future consent and reflects the stored value');
    // Saving other privacy settings keeps the stored consent exactly as the switches show it.
    page.$('#local-only').checked=false;page.$('#audio-retention-mode').value='none';page.$('#audio-retention-hours').value='24 hours';page.$('#audio-retention-max-mb').value='64 MiB';
    page.$('#telemetry').checked=telemetry;page.$('#crash-reports').checked=crash;
    await page.$('#save-privacy').onclick();
    const put=page.writes.find(w=>w.path==='/privacy');
    assert(put&&put.body.diagnostic_telemetry===telemetry&&put.body.crash_reports===crash,'consent values are saved unchanged');
    console.log(`privacy future reporting: telemetry=${telemetry} crash=${crash}: ok`);
  }
  const setupHtml=fs.readFileSync('web/setup.html','utf8');
  assert(!/Help improve LibreEcho/.test(setupHtml),'setup must not suggest data is sent');
  assert((setupHtml.match(/Not available in this release: nothing is sent, on or off\./g)||[]).length===2,'setup states nothing is sent for both choices');
  assert(setupHtml.includes('<input id="setup-telemetry" type="checkbox" checked>')&&setupHtml.includes('<input id="setup-crash-reports" type="checkbox" checked>'),'stored default consent is unchanged');
  const setupJs=fs.readFileSync('web/js/setup.js','utf8');
  const fn=setupJs.match(/function consentReview\(allowed\)\{[^\n]*\}/);
  assert(fn,'setup review uses consentReview');
  assert(!/'Shared'/.test(setupJs),'setup review must never say Shared');
  const ctx={};vm.createContext(ctx);vm.runInContext(fn[0],ctx);
  assert.equal(ctx.consentReview(true),'Allowed if a future release adds them; nothing is sent in this release');
  assert.equal(ctx.consentReview(false),'Not allowed; nothing is sent in this release');
  console.log('setup future reporting copy: ok');
})().catch(e=>{console.error(e);process.exit(1);});
