'use strict';

function assistantProviderPanel(options) {
  const {
    title,
    description,
    status,
    statusOkay,
    toggleLabel,
    toggleId,
    enabled,
    body,
    open
  } = options;
  return `<details class="panel setting-panel integration-section assistant-provider"${open?' open':''}>
    <summary>
      <div class="provider-summary-copy">
        <h3>${esc(title)}</h3>
        <small>${esc(description)}</small>
      </div>
      <div class="provider-summary-controls">
        <span class="assistant-state"><span class="status-dot ${statusOkay?'ok':''}"></span>${esc(status)}</span>
        ${toggle(toggleLabel,enabled,toggleId)}
        <span class="integration-toggle" aria-hidden="true">Show details</span>
      </div>
    </summary>
    <div class="integration-section-body">${body}</div>
  </details>`;
}

function assistantTelemetry(a) {
  const latency=Number(a.last_speech_end_to_first_pcm_ms||0);
  return `<dl class="facts">
    <dt>Wake audio</dt><dd class="${a.wake_connected?'connected':''}">${a.wake_connected?'Connected':'Unavailable'}</dd>
    <dt>Post-AEC stream</dt><dd class="${a.audio_connected?'connected':''}">${a.audio_connected?'Connected':'Unavailable'}</dd>
    <dt>Speech recognition</dt><dd>${a.recognizing?'Recognizing':'Ready'}</dd>
    <dt>Completed voice turns</dt><dd>${Number(a.completed_transcripts||0)}</dd>
    <dt>Last voice capture</dt><dd>${Number(a.last_stt_audio_ms||0)||'—'}${a.last_stt_audio_ms?' ms':''}</dd>
    <dt>STT finalization</dt><dd>${Number(a.last_stt_processing_ms||0)||'—'}${a.last_stt_processing_ms?' ms':''}</dd>
    <dt>Speech end to first PCM</dt><dd class="${latency&&latency<=3000?'connected':''}">${latency?latency+' ms':'Not measured'}</dd>
    <dt>Latency target</dt><dd>${Number(a.latency_target_ms||3000)} ms</dd>
  </dl>`;
}

function pipelineStatus(pipeline) {
  const stt=pipeline?.stt||{},tts=pipeline?.tts||{};
  return `<dl class="facts">
    <dt>Wake word</dt><dd class="connected">On device</dd>
    <dt>Speech recognition</dt><dd class="${stt.reachable?'connected':''}">${stt.reachable?'Whisper reachable':stt.configured?'Whisper unavailable':'Not configured'}</dd>
    <dt>Speech output</dt><dd class="${tts.reachable?'connected':''}">${tts.reachable?'Piper reachable':tts.configured?'Piper unavailable':'Not configured'}</dd>
    <dt>Pipeline mode</dt><dd>${esc(pipeline?.mode||'local')}</dd>
  </dl>`;
}

function localAssistantBody(a,selected,pipeline) {
  const configured=Boolean(a.base_url);
  const stt=pipeline?.stt||{},tts=pipeline?.tts||{};
  return `<div class="assistant-heading">
    <div>
      <span class="source-pill">LAN endpoint</span>
      <h4>OpenAI-compatible local stack</h4>
      <p class="muted">Sends transcripts to Gemma, llama.cpp, vLLM, or another compatible server on your network.</p>
    </div>
  </div>
  <div class="settings-grid assistant-settings">
    <div>
      ${field('Endpoint URL',a.base_url||'','local-base-url','url','placeholder="http://192.168.1.10:8000/v1"')}
      <label class="field"><span>API key (optional)</span><input id="local-api-key" type="password" autocomplete="off" placeholder="${a.api_key_configured?'Configured; leave blank to keep it':'Leave blank when the endpoint does not require one'}"></label>
      ${field('Model',a.model||'','local-model')}
      ${field('Whisper Wyoming endpoint',stt.wyoming_uri||'','stt-wyoming-uri','text','placeholder="tcp://192.168.1.10:10300"')}
      ${field('Whisper model',stt.model||'whisper-small','stt-model')}
      ${field('Piper Wyoming endpoint',tts.wyoming_uri||'','tts-wyoming-uri','text','placeholder="tcp://192.168.1.10:10200"')}
      ${field('Piper voice',tts.voice||'en_GB-alan-medium','tts-voice')}
      <label class="field"><span>Voice response prompt</span><textarea id="local-prompt" rows="8">${esc(a.prompt||'')}</textarea></label>
      ${saveButton('save-local-assistant')}
      ${!selected?'<p class="muted">Saving selects Local LLM without enabling the voice loop. Use the switch above when you are ready to test it.</p>':''}
    </div>
    <div>
      ${pipelineStatus(pipeline)}
      ${selected?assistantTelemetry(a):'<div class="privacy-callout">Local LLM is not currently selected.</div>'}
      ${selected&&configured?`<label class="field"><span>Test prompt</span><input id="local-test-text" value="Say hello in one short sentence."></label>${action('Speak test response','local-test')}`:''}
    </div>
  </div>`;
}

function deviceAssistantBody(a,selected) {
  if(!selected) {
    return `<div class="assistant-heading">
      <div>
        <span class="source-pill">Subscription</span>
        <h4>ChatGPT</h4>
        <p class="muted">Uses ChatGPT device login without storing an API key or falling back to metered API billing.</p>
      </div>
    </div>
    <div class="privacy-callout">Enable this assistant to view or change its ChatGPT sign-in.</div>`;
  }
  const signedIn=Boolean(a.authenticated),waiting=a.auth_state==='waiting';
  return `<div class="assistant-heading">
    <div>
      <span class="source-pill">Subscription</span>
      <h4>${esc(a.provider_name||'ChatGPT')}</h4>
      <p class="muted">Uses ChatGPT device login without storing an API key or falling back to metered API billing.</p>
    </div>
  </div>
  ${waiting?`<div class="device-code"><span>Enter this code</span><strong>${esc(a.user_code)}</strong><a class="primary-btn action-link" href="${esc(a.verification_url)}" target="_blank" rel="noopener">Open ChatGPT sign-in</a></div>`:''}
  <div class="settings-grid assistant-settings">
    <div>
      ${field('Provider',a.provider_name||a.provider,'assistant-provider','text','disabled')}
      ${field('Model',a.model||'','assistant-model')}
      <label class="field"><span>Voice response prompt</span><textarea id="assistant-prompt" rows="8">${esc(a.prompt||'')}</textarea></label>
      ${saveButton('save-assistant')}
    </div>
    <div>
      ${assistantTelemetry(a)}
      <div class="button-row">
        ${!signedIn&&!waiting?action('Connect ChatGPT','assistant-auth-start','primary-btn'):''}
        ${waiting?action('Check sign-in','assistant-auth-poll','primary-btn'):''}
        ${signedIn?action('Disconnect','assistant-logout','danger-btn'):''}
      </div>
      ${signedIn?`<label class="field"><span>Test prompt</span><input id="assistant-test-text" value="Say hello in one short sentence."></label>${action('Speak test response','assistant-test')}`:''}
    </div>
  </div>`;
}

async function setAssistantProvider(provider,enabled,pipeline) {
  if(state.busy)return;
  setBusy(true);
  try {
    if(enabled) {
      const local=provider==='openai-compatible';
      if(local) {
        await api('/privacy',{method:'PUT',body:JSON.stringify({local_only:false})});
      }
      await api('/voice-pipeline',{method:'PUT',body:JSON.stringify({
        mode:local?'custom':'local',
        stt_wyoming_uri:pipeline?.stt?.wyoming_uri||'',
        stt_model:pipeline?.stt?.model||'whisper-small',
        tts_wyoming_uri:pipeline?.tts?.wyoming_uri||'',
        tts_voice:pipeline?.tts?.voice||'en_GB-alan-medium'
      })});
    }
    await api('/assistant',{method:'PUT',body:JSON.stringify({provider,enabled})});
    if(!enabled&&provider==='openai-compatible'&&pipeline?.mode==='custom') {
      await api('/voice-pipeline',{method:'PUT',body:JSON.stringify({
        mode:'local',
        stt_wyoming_uri:pipeline?.stt?.wyoming_uri||'',
        stt_model:pipeline?.stt?.model||'whisper-small',
        tts_wyoming_uri:pipeline?.tts?.wyoming_uri||'',
        tts_voice:pipeline?.tts?.voice||'en_GB-alan-medium'
      })});
    }
    toast(enabled?(provider==='openai-compatible'?'Local LLM enabled':'On Device Voice Assistant enabled'):'Voice assistant disabled');
    await integrationsPage();
  } catch(error) {
    toast(error.message,true);
    await integrationsPage();
  } finally {
    setBusy(false);
  }
}

function bindProviderToggle(id,provider,pipeline) {
  const input=$(id),row=input?.closest('.switch-row');
  if(!input)return;
  if(row)row.onclick=event=>event.stopPropagation();
  input.onchange=()=>setAssistantProvider(provider,input.checked,pipeline);
}

async function integrationsPage() {
  const [d,a,pipeline]=await Promise.all([
    api('/integrations'),
    api('/assistant').catch(error=>({unsupported:error.message})),
    api('/voice-pipeline').catch(()=>({mode:'local',stt:{},tts:{}}))
  ]);
  const integrations=d.items.map(x=>collapsiblePanel(x.name,
    `<p class="muted">${x.id==='rest'?'Versioned local device API.':'Optional local integration; no cloud connection required.'}</p>
    ${toggle('Enabled',x.enabled,'int-'+x.id)}
    <div class="status-line"><span class="status-dot ${x.enabled?'ok':''}"></span><span>${x.enabled?'Enabled':'Not configured'}</span></div>
    ${saveButton('save-int-'+x.id)}`
  )).join('');

  if(a.unsupported) {
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${unsupported(a.unsupported)}</section>
      ${integrations}
    </div>`;
  } else {
    const localSelected=a.provider==='openai-compatible';
    const deviceSelected=a.provider==='openai-codex';
    const localEnabled=localSelected&&Boolean(a.enabled);
    const deviceEnabled=deviceSelected&&Boolean(a.enabled);
    const localConfigured=Boolean(a.base_url);
    const localStatus=localEnabled?'Enabled':localSelected?'Disabled':localConfigured?'Configured':'Not configured';
    const deviceStatus=deviceEnabled?(a.authenticated?'Enabled':'Sign-in required'):deviceSelected?'Disabled':'Inactive';
    const localPanel=assistantProviderPanel({
      title:'Local LLM',
      description:'OpenAI-compatible server on your network',
      status:localStatus,
      statusOkay:localEnabled&&localConfigured,
      toggleLabel:'Use Local LLM',
      toggleId:'use-local-provider',
      enabled:localEnabled,
      body:localAssistantBody(a,localSelected,pipeline),
      open:localSelected
    });
    const devicePanel=assistantProviderPanel({
      title:'On Device Voice Assistant',
      description:'ChatGPT subscription with device login',
      status:deviceStatus,
      statusOkay:deviceEnabled&&Boolean(a.authenticated),
      toggleLabel:'Use On Device Voice Assistant',
      toggleId:'use-device-provider',
      enabled:deviceEnabled,
      body:deviceAssistantBody(a,deviceSelected),
      open:deviceSelected
    });
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${localPanel}${devicePanel}</section>
      ${integrations}
    </div>`;

    bindProviderToggle('#use-local-provider','openai-compatible',pipeline);
    bindProviderToggle('#use-device-provider','openai-codex',pipeline);

    bindDirty(['#local-base-url','#local-model','#local-prompt','#local-api-key','#stt-wyoming-uri','#stt-model','#tts-wyoming-uri','#tts-voice'],'#save-local-assistant');
    $('#save-local-assistant').onclick=async()=>{
      if(state.busy)return;
      setBusy(true);
      try {
        if(localEnabled) {
          await api('/privacy',{method:'PUT',body:JSON.stringify({local_only:false})});
        }
        await api('/voice-pipeline',{method:'PUT',body:JSON.stringify({
          mode:localEnabled?'custom':'local',
          stt_wyoming_uri:$('#stt-wyoming-uri').value.trim(),
          stt_model:$('#stt-model').value.trim(),
          tts_wyoming_uri:$('#tts-wyoming-uri').value.trim(),
          tts_voice:$('#tts-voice').value.trim()
        })});
        const body={
          provider:'openai-compatible',
          enabled:localEnabled,
          base_url:$('#local-base-url').value.trim(),
          model:$('#local-model').value.trim(),
          prompt:$('#local-prompt').value.trim()
        };
        const key=$('#local-api-key').value;
        if(key)body.api_key=key;
        await api('/assistant',{method:'PUT',body:JSON.stringify(body)});
        toast('Local AI stack settings saved');
      } catch(error) {
        toast(error.message,true);
      } finally {
        await integrationsPage();
        setBusy(false);
      }
    };
    if($('#local-test'))$('#local-test').onclick=()=>post('/assistant/respond',{text:$('#local-test-text').value},'Test response queued');

    if(deviceSelected) {
      bindDirty(['#assistant-model','#assistant-prompt'],'#save-assistant');
      $('#save-assistant').onclick=()=>mutate('/assistant',{
        provider:'openai-codex',
        enabled:deviceEnabled,
        model:$('#assistant-model').value.trim(),
        prompt:$('#assistant-prompt').value.trim()
      },'On Device Voice Assistant settings saved');
      if($('#assistant-auth-start'))$('#assistant-auth-start').onclick=()=>assistantAction('/assistant/auth/start','ChatGPT device sign-in started');
      if($('#assistant-auth-poll'))$('#assistant-auth-poll').onclick=()=>assistantAction('/assistant/auth/poll','Sign-in status checked');
      if($('#assistant-logout'))$('#assistant-logout').onclick=()=>assistantAction('/assistant/logout','ChatGPT disconnected');
      if($('#assistant-test'))$('#assistant-test').onclick=()=>post('/assistant/respond',{text:$('#assistant-test-text').value},'Test response queued');
      if(a.auth_state==='waiting')state.timer=setTimeout(async()=>{
        if(state.page!=='Integrations')return;
        try {
          await api('/assistant/auth/poll',{method:'POST',body:'{}'});
          await integrationsPage();
        } catch(error) {
          toast(error.message,true);
        }
      },3000);
    }
  }

  d.items.forEach(x=>{
    bindDirty(['#int-'+x.id],'#save-int-'+x.id);
    $('#save-int-'+x.id).onclick=()=>mutate('/integrations/'+x.id,{enabled:$('#int-'+x.id).checked},x.name+' changes saved');
  });
}
