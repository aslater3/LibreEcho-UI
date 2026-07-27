/* Privacy copy reflects configured remote AI endpoints. */
async function privacyPage(){
  const pair=await Promise.all([api('/privacy'),api('/assistant').catch(()=>({}))]),p=pair[0],a=pair[1];
  const remote=a.provider==='openai-compatible'&&a.base_url;
  const warning=remote?'<div class="notice unsupported"><strong>Remote AI endpoint configured</strong><span>Transcripts are sent to '+esc(a.base_url)+' when the voice assistant responds. Disable the provider in Integrations to keep response processing on-device.</span></div>':'<div class="privacy-callout">No remote AI endpoint is configured. Voice processing remains on-device.</div>';
  content.innerHTML='<div class="settings-grid">'+panel('Processing',warning+toggle('Allow remote response processing',!p.local_only,'local-only')+toggle('Retain microphone audio',p.audio_retention!=='none','audio-retention')+toggle('Diagnostic telemetry',p.diagnostic_telemetry,'telemetry')+toggle('Crash reports',p.crash_reports,'crash-reports')+saveButton('save-privacy'))+panel('Retention',select('Log retention',String(p.log_retention_hours)+' hours','retention',['24 hours','168 hours','720 hours'])+'<div class="privacy-callout">Secrets, API keys, and microphone audio are excluded from configuration exports.</div>'+saveButton('save-retention'))+'</div>';
  bindDirty(['#local-only','#audio-retention','#telemetry','#crash-reports'],'#save-privacy');bindDirty(['#retention'],'#save-retention');
  $('#save-privacy').onclick=()=>mutate('/privacy',{local_only:!$('#local-only').checked,audio_retention:$('#audio-retention').checked?'24h':'none',diagnostic_telemetry:$('#telemetry').checked,crash_reports:$('#crash-reports').checked},'Privacy changes saved');
  $('#save-retention').onclick=()=>mutate('/privacy',{log_retention_hours:parseInt($('#retention').value,10)},'Retention changes saved');
}
