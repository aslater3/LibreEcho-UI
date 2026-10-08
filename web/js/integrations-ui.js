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
    open,
    disabled
  } = options;
  return `<details class="panel setting-panel integration-section assistant-provider"${open?' open':''}>
    <summary>
      <div class="provider-summary-copy">
        <h3>${esc(title)}</h3>
        <small>${esc(description)}</small>
      </div>
      <div class="provider-summary-controls">
        <span class="assistant-state"><span class="status-dot ${statusOkay?'ok':''}"></span>${esc(status)}</span>
        ${toggle(toggleLabel,enabled,toggleId,disabled)}
        <span class="integration-toggle" aria-hidden="true">Show details</span>
      </div>
    </summary>
    <div class="integration-section-body">${body}</div>
  </details>`;
}

function chatgptAccount(a) {
  /* Local endpoint readiness is not ChatGPT subscription authentication. */
  const selected=a?.provider==='openai-codex';
  const signedIn=selected&&Boolean(a.authenticated);
  const waiting=selected&&a.auth_state==='waiting';
  return {signedIn,waiting,status:signedIn?'Signed in':waiting?'Waiting for sign-in':selected&&a.auth_state==='error'?'Sign-in failed':'Sign in required'};
}

function chatgptSignInBlock(a,prefix) {
  const account=chatgptAccount(a);
  return `${account.waiting?`<div class="device-code"><span>Enter this code</span><strong>${esc(a.user_code)}</strong><a class="primary-btn action-link" href="${esc(a.verification_url)}" target="_blank" rel="noopener">Open ChatGPT sign-in</a></div>`:''}
  ${a.provider==='openai-codex'&&a.auth_state==='error'&&a.auth_error?`<p class="error-text">ChatGPT sign-in failed: ${esc(a.auth_error)}</p>`:''}
  <div class="button-row">
    ${!account.signedIn&&!account.waiting?action('Connect ChatGPT',prefix+'-auth-start','primary-btn'):''}
    ${account.waiting?action('Check sign-in',prefix+'-auth-poll','primary-btn'):''}
    ${account.waiting?action('Cancel sign-in',prefix+'-logout','secondary-btn'):''}
    ${account.signedIn?action('Disconnect',prefix+'-logout','danger-btn'):''}
  </div>
  ${a.provider!=='openai-codex'?'<p class="muted">Connecting selects ChatGPT without enabling it and stops Local LLM.</p>':''}
  ${a.base_url||a.api_key_configured?'<p class="muted">Cancelling sign-in or disconnecting ChatGPT also clears the saved Local LLM endpoint and API key. You will be asked to confirm.</p>':''}`;
}

function bindChatgptSignIn(prefix,a) {
  if($('#'+prefix+'-auth-start'))$('#'+prefix+'-auth-start').onclick=async()=>{
    if(state.busy)return;
    if(a.provider==='openai-codex')return assistantAction('/assistant/auth/start','ChatGPT device sign-in started');
    setBusy(true);
    try {
      await api('/assistant',{method:'PUT',body:JSON.stringify({provider:'openai-codex',enabled:false})});
      await api('/assistant/auth/start',{method:'POST',body:'{}'});
      toast('ChatGPT device sign-in started');
    } catch(error) { toast(error.message,true); }
    finally { setBusy(false); await integrationsPage(); }
  };
  if($('#'+prefix+'-auth-poll'))$('#'+prefix+'-auth-poll').onclick=()=>assistantAction('/assistant/auth/poll','Sign-in status checked');
  if($('#'+prefix+'-logout'))$('#'+prefix+'-logout').onclick=()=>{
    if(state.busy)return;
    if((a.base_url||a.api_key_configured)&&!confirm('Cancelling sign-in or disconnecting ChatGPT also clears the saved Local LLM endpoint and API key. Continue?'))return;
    return assistantAction('/assistant/logout',chatgptAccount(a).waiting?'Sign-in cancelled':'ChatGPT disconnected');
  };
}

function scheduleChatgptAuthPoll(a) {
  clearTimeout(state.timer);
  if(!chatgptAccount(a).waiting)return;
  state.timer=setTimeout(async()=>{
    if(state.page!=='Integrations')return;
    try {
      await api('/assistant/auth/poll',{method:'POST',body:'{}'});
      await integrationsPage();
    } catch(error) { toast(error.message,true); }
  },3000);
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
      ${field('Endpoint URL',a.base_url||'','local-base-url','url','placeholder="http://198.51.100.10:8000/v1"')}
      <label class="field"><span>API key (optional)</span><input id="local-api-key" type="password" autocomplete="off" placeholder="${a.api_key_configured?'Configured; leave blank to keep it':'Leave blank when the endpoint does not require one'}"></label>
      ${field('Model',a.model||'','local-model')}
      ${clockFormatField(a.clock_format,'local-clock-format')}
      ${field('Whisper Wyoming endpoint',stt.wyoming_uri||'','stt-wyoming-uri','text','placeholder="tcp://198.51.100.10:10300"')}
      ${field('Whisper model',stt.model||'whisper-small','stt-model')}
      ${field('Piper Wyoming endpoint',tts.wyoming_uri||'','tts-wyoming-uri','text','placeholder="tcp://198.51.100.10:10200"')}
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
    ${chatgptSignInBlock(a,'assistant')}`;
  }
  const signedIn=chatgptAccount(a).signedIn;
  return `<div class="assistant-heading">
    <div>
      <span class="source-pill">Subscription</span>
      <h4>${esc(a.provider_name||'ChatGPT')}</h4>
      <p class="muted">Uses ChatGPT device login without storing an API key or falling back to metered API billing.</p>
    </div>
  </div>
  ${chatgptSignInBlock(a,'assistant')}
  <div class="settings-grid assistant-settings">
    <div>
      ${field('Provider',a.provider_name||a.provider,'assistant-provider','text','disabled')}
      ${field('Model',a.model||'','assistant-model')}
      ${clockFormatField(a.clock_format,'assistant-clock-format')}
      <label class="field"><span>Voice response prompt</span><textarea id="assistant-prompt" rows="8">${esc(a.prompt||'')}</textarea></label>
      ${saveButton('save-assistant')}
    </div>
    <div>
      ${assistantTelemetry(a)}
      ${signedIn?`<label class="field"><span>Test prompt</span><input id="assistant-test-text" value="Say hello in one short sentence."></label>${action('Speak test response','assistant-test')}`:''}
    </div>
  </div>`;
}

function liveAssistantBody(live,a) {
  if(live.unsupported) return unsupported(live.unsupported);
  const session=live.session||{},metrics=live.transport_metrics||{};
  return `<div class="assistant-heading">
    <div>
      <span class="source-pill">Subscription</span>
      <h4>GPT-Live</h4>
      <p class="muted">Full-duplex speech-to-speech over the ChatGPT subscription. Post-AEC audio, including the short RAM-only wake preroll, leaves the device only after a wake starts a conversation.</p>
    </div>
  </div>
  ${a&&!a.unsupported&&!chatgptAccount(a).signedIn?chatgptSignInBlock(a,'live'):''}
  <div class="settings-grid assistant-settings">
    <div>
      <dl class="facts">
        <dt>Transport</dt><dd>${esc(metrics.transport||live.transport||'WebSocket')}</dd>
        <dt>Connection</dt><dd class="${metrics.session_ready?'connected':''}">${metrics.session_ready?'Ready':'Waiting for wake'}</dd>
        <dt>Conversation</dt><dd>${esc(session.state||'idle')}</dd>
        <dt>Last end</dt><dd>${esc(session.last_end||'—')}</dd>
        <dt>Completed sessions</dt><dd>${Number(session.sessions_completed||0)}</dd>
        <dt>Delegations</dt><dd>${Number(session.delegations||0)}</dd>
      </dl>
    </div>
    <div>
      <div class="privacy-callout">GPT-Live is not enabled at boot. Turning it on arms the local wake-word path; it does not expose idle microphone audio.</div>
      ${live.last_event&&live.last_event!=='idle'?`<p class="muted">Last event: ${esc(live.last_event)}</p>`:''}
    </div>
  </div>`;
}

async function setLiveProvider(enabled,assistant) {
  if(state.busy)return;
  if(enabled&&!chatgptAccount(assistant).signedIn) {
    toast('Sign in to ChatGPT before enabling GPT-Live',true);
    try { await integrationsPage(); } catch(error) { toast(error.message,true); }
    return;
  }
  setBusy(true);
  try {
    if(enabled && assistant && assistant.enabled) {
      await api('/assistant',{method:'PUT',body:JSON.stringify({provider:assistant.provider,enabled:false})});
    }
    await api('/live',{method:'PUT',body:JSON.stringify({enabled})});
    toast(enabled?'GPT-Live enabled':'GPT-Live disabled');
  } catch(error) {
    toast(error.message,true);
  } finally {
    setBusy(false); try{ await integrationsPage(); }catch(error){ toast(error.message,true); }
  }
}

function bindLiveToggle(id,assistant) {
  const input=$(id),row=input?.closest('.switch-row');
  if(!input)return;
  if(row)row.onclick=event=>event.stopPropagation();
  input.onchange=()=>setLiveProvider(input.checked,assistant);
}

function clockFormatField(value,id) {
  const twelve = value !== '24';
  return `<label class="field"><span>Spoken time format</span><select id="${id}">`+
    `<option value="12" ${twelve?'selected':''}>12-hour (2:30 PM)</option>`+
    `<option value="24" ${twelve?'':'selected'}>24-hour (14:30)</option>`+
    `</select></label>`;
}

async function setAssistantProvider(provider,enabled,pipeline,assistant) {
  if(state.busy)return;
  if(enabled&&provider==='openai-codex'&&!chatgptAccount(assistant).signedIn) {
    toast('Sign in to ChatGPT before enabling this assistant',true);
    try { await integrationsPage(); } catch(error) { toast(error.message,true); }
    return;
  }
  setBusy(true);
  try {
    if(enabled) {
      await api('/live',{method:'PUT',body:JSON.stringify({enabled:false})}).catch(()=>null);
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

function bindProviderToggle(id,provider,pipeline,assistant) {
  const input=$(id),row=input?.closest('.switch-row');
  if(!input)return;
  if(row)row.onclick=event=>event.stopPropagation();
  input.onchange=()=>setAssistantProvider(provider,input.checked,pipeline,assistant);
}

/*
 * app.js owns the existing Home location & weather card and its provider
 * helpers. integrations-ui.js replaces app.js's Integrations renderer, so it
 * must render and bind that existing card itself rather than silently hiding
 * the released configuration surface.
 */
function bindHomeLocation(a) {
  if(a.unsupported||!$('#wx-provider'))return;
  const original={
    location:String(a.home_location||'').trim(),
    latitude:String(a.latitude||'').trim(),
    longitude:String(a.longitude||'').trim()
  };
  $('#wx-location').value=original.location;
  $('#wx-lat').value=original.latitude;
  $('#wx-lon').value=original.longitude;
  bindDirty(['#wx-provider','#wx-location','#wx-lat','#wx-lon'],'#save-wx');
  /*
   * The lookup button and the stale-coordinate advisory come from app.js.
   * This renderer draws the same card, so it has to bind the same controls:
   * binding only the save button here is what left "Look up coordinates" on
   * screen with no handler at all.
   */
  bindWeatherLookup(a);
  $('#save-wx').onclick=()=>{
    const provider=wxId($('#wx-provider').value);
    const location=$('#wx-location').value.trim();
    const latitude=$('#wx-lat').value.trim();
    const longitude=$('#wx-lon').value.trim();
    const haveLatitude=latitude.length>0;
    const haveLongitude=longitude.length>0;
    const lat=haveLatitude?Number(latitude):null;
    const lon=haveLongitude?Number(longitude):null;
    const originalLat=original.latitude?Number(original.latitude):null;
    const originalLon=original.longitude?Number(original.longitude):null;
    let problem='';

    /* The coordinates on screen may belong to the place looked up before this
       edit, and typing re-enables Save, so the button's state is not enough. */
    if(lookupPending()) {
      problem='Finish the lookup first — the coordinates on screen are not for that place yet.';
    } else if(haveLatitude!==haveLongitude) {
      problem='Enter both latitude and longitude, or leave both unchanged.';
    } else if(haveLatitude&&(!Number.isFinite(lat)||!Number.isFinite(lon)||
              lat<-90||lat>90||lon<-180||lon>180)) {
      problem='Latitude must be -90 to 90 and longitude must be -180 to 180.';
    } else if(location&&!haveLatitude&&provider!=='off') {
      problem='This place needs coordinates before weather can be enabled.';
    } else if(original.location&&location!==original.location&&haveLatitude&&
              lat===originalLat&&lon===originalLon) {
      problem='The place changed but the coordinates did not. Update both coordinates before saving.';
    } else if(!location&&!haveLatitude&&
              (original.location||original.latitude||original.longitude)) {
      problem='This image cannot clear old coordinates safely. Select Off or replace them with the new location.';
    }
    if(problem){toast(problem,true);return;}
    mutate('/assistant',{
      weather_provider:provider,
      home_location:location,
      latitude,
      longitude
    },'Home location saved');
  };
}

function homeAssistantVoiceStatus(enabled,pipeline) {
  const restart=pipeline?.restart?.state,ha=pipeline?.home_assistant;
  let status='Not configured';
  if(restart==='failed')status='Failed';
  else if(restart==='pending')status=enabled?'Enabling':'Disabling';
  else if(enabled) {
    status='Configured — status unavailable';
    if(pipeline?.mode==='home-assistant'&&restart==='ready'&&
        typeof ha?.ready==='boolean'&&typeof ha?.connected==='boolean') {
      status=!ha.ready?'Unavailable':ha.connected?'Connected':'Ready — awaiting Home Assistant';
    }
  }
  return {status,okay:status==='Connected'};
}

/*
 * Home Assistant onboarding, mirroring the stock ESPHome flow: HA discovers the
 * satellite over mDNS (or the owner adds it by host and port 6053), then asks
 * for the device's API encryption key. The key is the one libreecho-esphomed
 * generated and stores; the link opens HA's "add ESPHome integration" flow.
 */
const HOME_ASSISTANT_DEFAULT_URL='http://homeassistant.local:8123';
function homeAssistantSetupLink(url) {
  const base=String(url||HOME_ASSISTANT_DEFAULT_URL).replace(/\/+$/,'');
  return /^https?:\/\/[^\s"'<>`]+$/.test(base)?base+'/config/integrations/dashboard/add?domain=esphome':'';
}
function homeAssistantSetupPanel(ha,connected) {
  ha=ha||{};const url=ha.url||HOME_ASSISTANT_DEFAULT_URL,key=ha.encryption_key||'';
  const link=homeAssistantSetupLink(url),host=location.hostname||'this device';
  const keyRow=key
    ?`<div class="field"><span>Encryption key</span><div class="ha-key-row"><code id="ha-encryption-key" class="ha-key" data-revealed="false">${'•'.repeat(16)}</code>
        <button type="button" class="secondary-btn" id="ha-key-reveal" aria-controls="ha-encryption-key">Show</button>
        ${provenanceCopy('encryption key',key)}</div></div>`
    :`<p class="muted">The encryption key is generated when the Home Assistant satellite starts. Refresh this page in a moment.</p>`;
  return `<div class="ha-setup" id="ha-setup">
    <h4>${connected?'Home Assistant setup details':'Set up in Home Assistant'}</h4>
    <ol class="ha-setup-steps">
      <li>Open Home Assistant. LibreEcho is usually discovered automatically under <strong>Settings → Devices &amp; services</strong>; otherwise add the <strong>ESPHome</strong> integration with host <code>${esc(host)}</code> and port <code>6053</code>.</li>
      <li>When Home Assistant asks for the encryption key, paste the key below.</li>
    </ol>
    ${keyRow}
    ${field('Home Assistant address',url,'ha-url','url','placeholder="'+HOME_ASSISTANT_DEFAULT_URL+'" spellcheck="false" autocomplete="off"')}
    <div class="button-row">
      ${link?`<a class="primary-btn" id="ha-open-link" href="${esc(link)}" target="_blank" rel="noopener noreferrer">Set up in Home Assistant</a>`:''}
      <button class="save-btn save-changes" id="save-ha-url" disabled>Save address</button>
    </div>
  </div>`;
}
function bindHomeAssistantSetup() {
  const reveal=$('#ha-key-reveal'),code=$('#ha-encryption-key');
  if(reveal&&code) {
    const copy=code.parentElement?.querySelector('.copy-provenance');
    reveal.onclick=()=>{
      const show=code.dataset.revealed!=='true';
      code.textContent=show?(copy?.dataset.copyValue||''):'•'.repeat(16);
      code.dataset.revealed=show?'true':'false';
      reveal.textContent=show?'Hide':'Show';
    };
  }
  const input=$('#ha-url'),save=$('#save-ha-url'),open=$('#ha-open-link');
  if(!input||!save)return;
  bindDirty(['#ha-url'],'#save-ha-url');
  input.addEventListener('input',()=>{
    const link=homeAssistantSetupLink(input.value.trim()||HOME_ASSISTANT_DEFAULT_URL);
    if(open){if(link)open.href=link;open.classList.toggle('disabled',!link);}
  });
  save.onclick=()=>mutate('/home-assistant',{url:input.value.trim()||HOME_ASSISTANT_DEFAULT_URL},'Home Assistant address saved');
}

async function integrationsPage() {
  const generation=state.renderGeneration,page=state.page;
  const [d,a,pipeline,live,homeAssistant]=await Promise.all([
    api('/integrations'),
    api('/assistant').catch(error=>({unsupported:error.message})),
    api('/voice-pipeline').catch(()=>({mode:'local',stt:{},tts:{}})),
    api('/live').catch(error=>({unsupported:error.message})),
    api('/home-assistant').catch(()=>({}))
  ]);
  if(generation!==state.renderGeneration||page!==state.page)return;
  clearTimeout(state.timer);
  /*
   * integrationBlurb/integrationStatus come from app.js, which loads first.
   * An integration the image was built without reports installed:false; it is
   * listed so the absence is visible, but renders no toggle or save button --
   * enabling it could only ever fail. The handler loop below skips those rows.
   */
  const homeAssistantEnabled=d.items.some(x=>x.id==='home-assistant'&&x.enabled);
  const homeAssistantState=homeAssistantVoiceStatus(homeAssistantEnabled,pipeline);
  const transitionError=pipeline?.restart?.error
    ?`<p class="notice" role="alert">Voice pipeline error: ${esc(pipeline.restart.error)}</p>`:'';
  const integrations=d.items.map(x=>collapsiblePanel(x.name,
    `<p class="muted">${integrationBlurb(x)}</p>
    ${x.installed===false?'':toggle('Enabled',x.enabled,'int-'+x.id,x.forced)}
    <div class="status-line"><span class="status-dot ${x.id==='home-assistant'?(homeAssistantState.okay?'ok':''):(x.enabled?'ok':'')}"></span><span>${esc(x.id==='home-assistant'&&x.installed!==false?homeAssistantState.status:integrationStatus(x))}</span></div>
    ${x.id==='home-assistant'?transitionError:''}
    ${x.installed===false?'':saveButton('save-int-'+x.id)}`
  )).join('');

  if(homeAssistantEnabled) {
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${collapsiblePanel(homeAssistantState.okay?'Managed by Home Assistant':'Home Assistant voice',`<div class="assistant-heading"><div><span class="source-pill">Integration</span><h4>Home Assistant</h4><p class="muted">${homeAssistantState.okay?'Voice is handled by Home Assistant over the encrypted ESPHome connection (TCP 6053).':'Home Assistant is selected for voice using the encrypted ESPHome connection (TCP 6053).'} Your Local and Custom settings are kept for when you disable Home Assistant. The on-device assistant (Local LLM and ChatGPT) controls stay hidden while the Home Assistant integration is enabled. Disable it on this page to use the on-device assistant.</p></div><div class="assistant-state"><span class="status-dot ${homeAssistantState.okay?'ok':''}"></span>${esc(homeAssistantState.status)}</div></div>${homeAssistantSetupPanel(homeAssistant,homeAssistantState.okay)}`,'assistant-mode',!homeAssistantState.okay)}</section>
      ${integrations}
    </div>`;
  } else if(a.unsupported) {
    const liveEnabled=!live.unsupported&&Boolean(live.enabled);
    const livePanel=assistantProviderPanel({
      title:'GPT-Live',
      description:'Full-duplex speech with your ChatGPT subscription',
      status:live.unsupported?'Unavailable':'Sign-in status unavailable',
      statusOkay:false,
      toggleLabel:'Use GPT-Live',
      toggleId:'use-live-provider',
      enabled:liveEnabled,
      body:liveAssistantBody(live),
      open:false,
      disabled:Boolean(live.unsupported)||!liveEnabled
    });
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${unsupported(a.unsupported)}${livePanel}</section>
      ${integrations}
    </div>`;
    bindLiveToggle('#use-live-provider',null);
  } else {
    const localSelected=a.provider==='openai-compatible';
    const deviceSelected=a.provider==='openai-codex';
    const localEnabled=localSelected&&Boolean(a.enabled);
    const deviceEnabled=deviceSelected&&Boolean(a.enabled);
    const localConfigured=Boolean(a.base_url);
    const localStatus=localEnabled?'Enabled':localSelected?'Disabled':localConfigured?'Configured':'Not configured';
    const account=chatgptAccount(a);
    const deviceStatus=!account.signedIn?account.status:deviceEnabled?'Enabled':'Disabled';
    const localPanel=assistantProviderPanel({
      title:'Local LLM',
      description:'OpenAI-compatible server on your network',
      status:localStatus,
      statusOkay:localEnabled&&localConfigured,
      toggleLabel:'Use Local LLM',
      toggleId:'use-local-provider',
      enabled:localEnabled,
      body:localAssistantBody(a,localSelected,pipeline),
      /*
       * Collapsed even when this is the selected provider. Opening on
       * selection meant the page arrived expanded on every load for anyone
       * actually using it -- the same "stop expanding panels" complaint that
       * closed Home location, the voice assistant and Internet radio.
       */
      open:false
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
      open:!account.signedIn,
      disabled:!account.signedIn&&!deviceEnabled
    });
    const liveEnabled=!live.unsupported&&Boolean(live.enabled);
    const livePanel=assistantProviderPanel({
      title:'GPT-Live',
      description:'Full-duplex speech with your ChatGPT subscription',
      status:live.unsupported?'Unavailable':!account.signedIn?account.status:liveEnabled?'Enabled':'Disabled',
      statusOkay:liveEnabled&&account.signedIn,
      toggleLabel:'Use GPT-Live',
      toggleId:'use-live-provider',
      enabled:liveEnabled,
      body:liveAssistantBody(live,a),
      open:!live.unsupported&&!account.signedIn,
      disabled:Boolean(live.unsupported)||(!account.signedIn&&!liveEnabled)
    });
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${localPanel}${devicePanel}${livePanel}</section>
      ${weatherCard(a)}
      ${integrations}
    </div>`;

    bindProviderToggle('#use-local-provider','openai-compatible',pipeline);
    bindProviderToggle('#use-device-provider','openai-codex',pipeline,a);
    bindLiveToggle('#use-live-provider',a);
    bindChatgptSignIn('assistant',a);
    if(!live.unsupported)bindChatgptSignIn('live',a);
    scheduleChatgptAuthPoll(a);
    bindHomeLocation(a);

    bindDirty(['#local-base-url','#local-model','#local-clock-format','#local-prompt','#local-api-key','#stt-wyoming-uri','#stt-model','#tts-wyoming-uri','#tts-voice'],'#save-local-assistant');
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
          clock_format:$('#local-clock-format').value,
          prompt:$('#local-prompt').value.trim()
        };
        const key=$('#local-api-key').value;
        if(key)body.api_key=key;
        await api('/assistant',{method:'PUT',body:JSON.stringify(body)});
        toast('Local AI stack settings saved');
      } catch(error) {
        toast(error.message,true);
      } finally {
        setBusy(false); try{ await integrationsPage(); }catch(error){ toast(error.message,true); }
      }
    };
    if($('#local-test'))$('#local-test').onclick=()=>post('/assistant/respond',{text:$('#local-test-text').value},'Test response queued');

    if(deviceSelected) {
      bindDirty(['#assistant-model','#assistant-clock-format','#assistant-prompt'],'#save-assistant');
      $('#save-assistant').onclick=()=>mutate('/assistant',{
        provider:'openai-codex',
        enabled:deviceEnabled&&account.signedIn,
        model:$('#assistant-model').value.trim(),
        clock_format:$('#assistant-clock-format').value,
        prompt:$('#assistant-prompt').value.trim()
      },'On Device Voice Assistant settings saved');
      if($('#assistant-test'))$('#assistant-test').onclick=()=>post('/assistant/respond',{text:$('#assistant-test-text').value},'Test response queued');
    }
  }

  if(homeAssistantEnabled){bindHomeAssistantSetup();bindProvenanceCopy();}
  d.items.forEach(x=>{
    const save=$('#save-int-'+x.id);
    if(!save)return;                       /* not installed: nothing rendered to bind */
    bindDirty(['#int-'+x.id],'#save-int-'+x.id);
    save.onclick=async()=>{
      if(x.id==='home-assistant'&&$('#int-'+x.id).checked) await api('/live',{method:'PUT',body:JSON.stringify({enabled:false})}).catch(()=>null);
      mutate('/integrations/'+x.id,{enabled:$('#int-'+x.id).checked},x.name+' changes saved');
    };
  });
}
