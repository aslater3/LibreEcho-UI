/* Privacy copy reflects configured remote AI endpoints. */
async function privacyPage(){
  const pair=await Promise.all([api('/privacy'),api('/assistant').catch(()=>({})),api('/voice-pipeline').catch(()=>({mode:'local',stt:{},tts:{}}))]),p=pair[0],a=pair[1],pipeline=pair[2];
  const localStack=a.provider==='openai-compatible'&&a.base_url,pipelineRemote=pipeline.mode==='custom';
  const routes=(localStack||pipelineRemote)?`<div class="notice unsupported"><strong>LAN voice processing is configured</strong><span>Wake-word detection stays on this device. Microphone audio is sent to ${esc(pipeline.stt?.wyoming_uri||'the configured Whisper service')}; transcripts are sent to ${esc(a.base_url||'the configured language model')}; generated reply text is sent to ${esc(pipeline.tts?.wyoming_uri||'the configured Piper service')}.</span></div>`:'<div class="privacy-callout">Wake word, speech recognition, and speech output are configured to run on this device.</div>';
  const health=pipelineRemote?`<dl class="facts"><dt>Whisper</dt><dd class="${pipeline.stt?.reachable?'connected':''}">${pipeline.stt?.reachable?'Reachable':'Unavailable'}</dd><dt>Language model</dt><dd>${a.base_url?esc(a.base_url):'Not configured'}</dd><dt>Piper</dt><dd class="${pipeline.tts?.reachable?'connected':''}">${pipeline.tts?.reachable?'Reachable':'Unavailable'}</dd></dl>`:'';
  content.innerHTML='<div class="settings-grid">'+panel('Processing',routes+health+toggle('Allow configured network AI services',!p.local_only,'local-only')+toggle('Retain microphone audio',p.audio_retention!=='none','audio-retention')+toggle('Diagnostic telemetry',p.diagnostic_telemetry,'telemetry')+toggle('Crash reports',p.crash_reports,'crash-reports')+saveButton('save-privacy'))+panel('Retention',select('Log retention',String(p.log_retention_hours)+' hours','retention',['24 hours','168 hours','720 hours'])+'<div class="privacy-callout">Secrets, API keys, and microphone audio are excluded from configuration exports.</div>'+saveButton('save-retention'))+'</div>';
  bindDirty(['#local-only','#audio-retention','#telemetry','#crash-reports'],'#save-privacy');bindDirty(['#retention'],'#save-retention');
  $('#save-privacy').onclick=async()=>{
    if(state.busy)return;
    const allowNetwork=$('#local-only').checked;
    setBusy(true);
    try {
      if(!allowNetwork&&(pipelineRemote||(localStack&&a.enabled))) {
        if(localStack&&a.enabled) {
          await api('/assistant',{method:'PUT',body:JSON.stringify({
            provider:'openai-compatible',
            enabled:false
          })});
        }
        if(pipelineRemote) {
          await api('/voice-pipeline',{method:'PUT',body:JSON.stringify({
            mode:'local',
            stt_wyoming_uri:pipeline.stt?.wyoming_uri||'',
            stt_model:pipeline.stt?.model||'whisper-small',
            tts_wyoming_uri:pipeline.tts?.wyoming_uri||'',
            tts_voice:pipeline.tts?.voice||'en_GB-alan-medium'
          })});
        }
      }
      await api('/privacy',{method:'PUT',body:JSON.stringify({
        local_only:!allowNetwork,
        audio_retention:$('#audio-retention').checked?'24h':'none',
        diagnostic_telemetry:$('#telemetry').checked,
        crash_reports:$('#crash-reports').checked
      })});
      toast(allowNetwork?'Network AI services allowed':'Network AI services disabled');
    } catch(error) {
      toast(error.message,true);
    } finally {
      await privacyPage();
      setBusy(false);
    }
  };
  $('#save-retention').onclick=()=>mutate('/privacy',{log_retention_hours:parseInt($('#retention').value,10)},'Retention changes saved');
}
