/* Execute the real Privacy renderer and save handler without a device. */
'use strict';
const assert=require('assert'),fs=require('fs'),vm=require('vm');
const source=fs.readFileSync('web/js/privacy-ui.js','utf8');
const onDevice='Wake word, speech recognition, and speech output are configured to run on this device.';
const unknown='Processing destinations could not be determined';
async function page(overrides={},failWrite='',pipelineResult=null) {
  const data={'/privacy':{local_only:false,audio_retention:'none',log_retention_hours:24},'/assistant':{provider:'openai-codex',enabled:false},'/voice-pipeline':{mode:'local',stt:{},tts:{}},'/live':{enabled:false},...overrides};
  const elements=new Map(),writes=[],toasts=[],content={innerHTML:''},state={busy:false};
  const $=id=>{if(!elements.has(id))elements.set(id,{value:'',checked:false});return elements.get(id);};
  const context={content,state,$,console,bindDirty(){},setBusy:value=>{state.busy=value;},toast:(message,error)=>toasts.push({message,error}),
    esc:value=>String(value).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])),
    panel:(title,body)=>title+body,toggle:()=>'',select:()=>'',field:()=>'',saveButton:()=>'',
    api:async(path,options)=>{
      if(options?.method==='PUT') {
        const body=JSON.parse(options.body);writes.push({path,body});
        if(path===failWrite)throw new Error('Disable failed');
        if(path==='/voice-pipeline')data[path]={...data[path],mode:body.mode,...pipelineResult};
        else data[path]={...data[path],...body};
        return data[path];
      }
      assert(Object.hasOwn(data,path),'unexpected API route '+path);
      if(data[path] instanceof Error)throw data[path];
      return data[path];
    }};
  vm.createContext(context);vm.runInContext(source,context,{filename:'privacy-ui.js'});
  await context.privacyPage();assert.equal(writes.length,0,'render must be read-only');
  return {html:content.innerHTML,writes,toasts,async disable(){
    $('#local-only').checked=false;$('#audio-retention-mode').value='none';$('#audio-retention-hours').value='24 hours';$('#audio-retention-max-mb').value='64 MiB';
    await $('#save-privacy').onclick();
  }};
}
(async()=>{
  const cases=[
    ['failed auxiliary reads',{'/assistant':new Error('Unavailable'),'/voice-pipeline':new Error('Unavailable')},unknown],
    ['failed assistant read',{'/assistant':new Error('Unavailable')},unknown],
    ['failed pipeline read',{'/voice-pipeline':new Error('Unavailable')},unknown],
    ['failed Live read',{'/live':new Error('Unavailable')},unknown],
    ['missing Live enabled',{'/live':{}},unknown],
    ['unknown pipeline mode',{'/voice-pipeline':{mode:'future'}},unknown],
    ['pending pipeline',{'/voice-pipeline':{mode:'local',restart:{state:'pending'}}},unknown],
    ['failed pipeline',{'/voice-pipeline':{mode:'local',restart:{state:'failed'}}},unknown],
    ['Home Assistant',{'/voice-pipeline':{mode:'home-assistant',home_assistant:{protocol:'esphome',port:6053,ready:true,connected:true}}},'Microphone audio is sent to the Home Assistant host'],
    ['GPT-Live',{'/live':{enabled:true}},'Microphone audio is sent to OpenAI Realtime'],
    ['Custom',{'/voice-pipeline':{mode:'custom',stt:{wyoming_uri:'tcp://whisper.example:10300'},tts:{wyoming_uri:'tcp://piper.example:10200'}}},'tcp://whisper.example:10300'],
    ['LAN model',{'/assistant':{provider:'openai-compatible',enabled:true,base_url:'http://llm.example/v1'}},'http://llm.example/v1'],
    ['subscription model',{'/assistant':{provider:'openai-codex',enabled:true}},'transcripts are sent to ChatGPT'],
    ['all local',{},onDevice]
  ];
  for(const [name,data,notice] of cases) {
    const result=await page(data);assert(result.html.includes(notice),name+': expected disclosure');
    if(name!=='all local')assert(!result.html.includes(onDevice),name+': must not claim on-device');
    console.log('privacy disclosure: '+name+': ok');
  }
  const ha=await page({'/voice-pipeline':{mode:'home-assistant',home_assistant:{port:6053,ready:true,connected:true}},'/live':{enabled:true}});
  assert(ha.html.includes('<dt>Home Assistant')&&ha.html.includes('6053')&&ha.html.includes('<dt>GPT-Live'));
  await ha.disable();
  assert(ha.writes.some(x=>x.path==='/live'&&x.body.enabled===false),'disable must disarm GPT-Live');
  assert(ha.writes.some(x=>x.path==='/voice-pipeline'&&x.body.mode==='local'),'disable must leave HA mode');
  const unavailable=await page({'/live':new Error('Unavailable')});await unavailable.disable();
  assert(!unavailable.toasts.some(x=>x.message==='Network AI services disabled'),'unknown routing must not report disabled');
  const failure=await page({'/live':{enabled:true}},'/live');await failure.disable();
  assert(failure.toasts.some(x=>x.error&&x.message==='Disable failed'));
  assert(!failure.writes.some(x=>x.path==='/privacy'),'failed disarm must not save local-only');
  for(const transition of ['pending','failed']) {
    const result=await page({'/voice-pipeline':{mode:'home-assistant'}},'',{restart:{state:transition,error:'Transition failed'}});await result.disable();
    assert(!result.toasts.some(x=>x.message==='Network AI services disabled'),transition+' must not claim disabled');
    assert(!result.writes.some(x=>x.path==='/privacy'),transition+' must not save local-only');
  }
  for(const provider of ['openai-compatible','openai-codex']) {
    const result=await page({'/assistant':{provider,enabled:true,base_url:'http://llm.example/v1'}});await result.disable();
    assert(result.writes.some(x=>x.path==='/assistant'&&x.body.provider===provider&&x.body.enabled===false),'disable must stop '+provider);
    assert(result.toasts.some(x=>x.message==='Network AI services disabled'));
  }
  const escaped=await page({'/assistant':{provider:'openai-compatible',enabled:true,base_url:'<img src=x>'}});
  assert(escaped.html.includes('&lt;img src=x&gt;')&&!escaped.html.includes('<img src=x>'),'destinations must be escaped');
  console.log('privacy disclosure: disable routes and failures: ok');
})().catch(error=>{console.error(error);process.exitCode=1;});
