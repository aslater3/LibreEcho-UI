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

/*
 * The ChatGPT account is shared by every subscription-backed assistant, so the
 * sign-in controls belong to the account rather than to one provider panel.
 * They are rendered wherever the account is required -- including while the
 * provider is not selected or not enabled -- and bound from one place per
 * panel that draws them, so account setup is never reachable only through
 * activation.
 */
function chatgptAccount(a) {
  const authState=String(a?.auth_state||(a?.authenticated?'signed_in':'signed_out'));
  const signedIn=Boolean(a?.authenticated);
  const waiting=authState==='waiting';
  const failed=authState==='error';
  return {signedIn,waiting,failed,authState,
    status:signedIn?'Signed in':waiting?'Waiting for sign-in':failed?'Sign-in failed':'Sign in required'};
}

function chatgptSignInBlock(a,prefix) {
  const waiting=a.auth_state==='waiting';
  const signedIn=Boolean(a.authenticated);
  return `${
    waiting?`<div class="device-code"><span>Enter this code</span><strong>${esc(a.user_code)}</strong><a class="primary-btn action-link" href="${esc(a.verification_url)}" target="_blank" rel="noopener">Open ChatGPT sign-in</a></div>`:''
  }${
    a.auth_state==='error'&&a.auth_error?`<p class="error-text">ChatGPT sign-in failed: ${esc(a.auth_error)}</p>`:''
  }<div class="button-row">
    ${!signedIn&&!waiting?action('Connect ChatGPT',prefix+'-auth-start','primary-btn'):''}
    ${waiting?action('Check sign-in',prefix+'-auth-poll','primary-btn'):''}
    ${signedIn?action('Disconnect',prefix+'-logout','danger-btn'):''}
  </div>${
    signedIn?'':'<p class="muted">The switch above stays off until this account is signed in.</p>'
  }`;
}

function bindChatgptSignIn(prefix) {
  if($('#'+prefix+'-auth-start'))$('#'+prefix+'-auth-start').onclick=()=>assistantAction('/assistant/auth/start','ChatGPT device sign-in started');
  if($('#'+prefix+'-auth-poll'))$('#'+prefix+'-auth-poll').onclick=()=>assistantAction('/assistant/auth/poll','Sign-in status checked');
  if($('#'+prefix+'-logout'))$('#'+prefix+'-logout').onclick=()=>assistantAction('/assistant/logout','ChatGPT disconnected');
}

/*
 * A pending device login is polled so the code and the verification link stay
 * valid while the user finishes the sign-in in another tab. This runs for the
 * account, not for the selected provider: sign-in can be started from either
 * ChatGPT-backed panel, and the poll re-renders the whole page -- which is why
 * those panels are rendered open while the account is signed out or waiting.
 */
function scheduleChatgptAuthPoll(a) {
  if(!a||a.unsupported||a.auth_state!=='waiting')return;
  state.timer=setTimeout(async()=>{
    if(state.page!=='Integrations')return;
    try {
      await api('/assistant/auth/poll',{method:'POST',body:'{}'});
      await integrationsPage();
    } catch(error) {
      toast(error.message,true);
    }
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
  const heading=`<div class="assistant-heading">
    <div>
      <span class="source-pill">Subscription</span>
      <h4>${esc(a.provider_name||'ChatGPT')}</h4>
      <p class="muted">Uses ChatGPT device login without storing an API key or falling back to metered API billing.</p>
    </div>
  </div>`;
  if(!selected) {
    /* Sign-in is account setup, not provider activation: it is rendered and
       bound here even while another provider is selected. */
    return `${heading}
    ${chatgptSignInBlock(a,'assistant')}
    <div class="privacy-callout">Enable this assistant with the switch above to run wake-to-reply on the device. Its ChatGPT sign-in is shared with GPT-Live.</div>`;
  }
  const signedIn=Boolean(a.authenticated);
  return `${heading}
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

function liveAssistantBody(live,account,authPrefix) {
  if(live.unsupported) return unsupported(live.unsupported);
  const session=live.session||{},metrics=live.transport_metrics||{};
  const signedIn=account?account.signedIn:true;
  const disabled=signedIn?'':'disabled';
  const voices=['alloy','ash','ballad','coral','echo','sage','shimmer','verse','marin','cedar'];
  const accents=['natural British English','neutral English','natural Irish English','natural American English'];
  const voice=live.voice||'marin',accent=live.accent||'natural British English';
  const voiceOptions=voices.map(v=>`<option value="${v}" ${v===voice?'selected':''}>${v[0].toUpperCase()+v.slice(1)}</option>`).join('');
  const accentOptions=accents.map(v=>`<option value="${v}" ${v===accent?'selected':''}>${esc(v)}</option>`).join('');
  return `<div class="assistant-heading">
    <div>
      <span class="source-pill">Subscription</span>
      <h4>GPT-Live</h4>
      <p class="muted">Speech-to-speech over the ChatGPT subscription. Post-AEC audio, including the short RAM-only wake preroll, leaves the device only after a wake starts a conversation.</p>
    </div>
  </div>
  ${signedIn?'':`${chatgptSignInBlock(live,authPrefix)}
  <div class="privacy-callout">GPT-Live speaks over the same ChatGPT account as the On Device Voice Assistant, so it cannot be switched on until that sign-in completes.</div>`}
  <div class="settings-grid assistant-settings">
    <div>
      <label class="field"><span>Realtime voice</span><select id="live-voice" ${disabled}>${voiceOptions}</select></label>
      <label class="field"><span>English accent instruction</span><select id="live-accent" ${disabled}>${accentOptions}</select></label>
      <p class="muted">Voice names are provider personas, not guaranteed regional accents. The accent instruction is advisory and every session is explicitly instructed to remain in English.</p>
      ${saveButton('save-live-voice')}
      ${action('Test selected voice','test-live-voice',!signedIn||!live.enabled)}
    </div>
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
  </div>
  <label class="field"><span>Context supplied when a session opens</span><textarea id="live-context" rows="7" readonly>${esc(live.context||'Context is collected locally when the service starts and refreshed for each wake.')}</textarea></label>
  <div class="privacy-callout">Context includes the LibreEcho product identity, host name, local date/time, now-playing state and active timers. Credentials, network identifiers, transcripts and microphone audio are excluded.</div>
  ${live.last_event&&live.last_event!=='idle'?`<p class="muted">Last event: ${esc(live.last_event)}</p>`:''}`;
}

async function setLiveProvider(enabled,assistant,gate) {
  if(state.busy)return;
  /* See setAssistantProvider: the disabled switch is the visible half of this
     prerequisite, not the only place it is enforced. */
  if(enabled&&gate&&!gate.signedIn) {
    toast('Sign in to ChatGPT before enabling GPT-Live',true);
    await integrationsPage();
    return;
  }
  setBusy(true);
  try {
    if(enabled && assistant && assistant.enabled) {
      await api('/assistant',{method:'PUT',body:JSON.stringify({provider:assistant.provider,enabled:false})});
    }
    const body={enabled};
    if($('#live-voice'))body.voice=$('#live-voice').value;
    if($('#live-accent'))body.accent=$('#live-accent').value;
    await api('/live',{method:'PUT',body:JSON.stringify(body)});
    toast(enabled?'GPT-Live enabled':'GPT-Live disabled');
  } catch(error) {
    toast(error.message,true);
  } finally {
    await integrationsPage();
    setBusy(false);
  }
}

function bindLiveToggle(id,assistant,gate) {
  const input=$(id),row=input?.closest('.switch-row');
  if(!input)return;
  if(row)row.onclick=event=>event.stopPropagation();
  input.onchange=()=>setLiveProvider(input.checked,assistant,gate);
}

function bindLiveVoiceControls(live) {
  const save=$('#save-live-voice'),test=$('#test-live-voice');
  const apply=async preview=>{
    if(state.busy)return;
    if(preview&&!live.enabled){toast('Enable GPT-Live before testing a voice',true);return;}
    setBusy(true);
    try {
      await api('/live',{method:'PUT',body:JSON.stringify({
        enabled:Boolean(live.enabled),
        voice:$('#live-voice').value,
        accent:$('#live-accent').value
      })});
      if(preview)await api('/live/preview',{method:'POST',body:'{}'});
      toast(preview?'Playing the selected realtime voice':'GPT-Live voice settings saved');
    } catch(error) {
      toast(error.message,true);
    } finally {
      await integrationsPage();
      setBusy(false);
    }
  };
  if(save)save.onclick=()=>apply(false);
  if(test)test.onclick=()=>apply(true);
  bindDirty(['#live-voice','#live-accent'],'#save-live-voice');
}

function clockFormatField(value,id) {
  const twelve = value !== '24';
  return `<label class="field"><span>Spoken time format</span><select id="${id}">`+
    `<option value="12" ${twelve?'selected':''}>12-hour (2:30 PM)</option>`+
    `<option value="24" ${twelve?'':'selected'}>24-hour (14:30)</option>`+
    `</select></label>`;
}

async function setAssistantProvider(provider,enabled,pipeline,gate) {
  if(state.busy)return;
  /*
   * A subscription-backed assistant cannot be enabled before the shared
   * ChatGPT account is signed in: the daemon accepts the write and only fails
   * later, at the first voice turn. The switch is rendered disabled in that
   * state; this guard is what makes the prerequisite hold for any path that
   * still reaches a change event.
   */
  if(enabled&&gate&&!gate.signedIn) {
    toast('Sign in to ChatGPT before enabling this assistant',true);
    await integrationsPage();
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

function bindProviderToggle(id,provider,pipeline,gate) {
  const input=$(id),row=input?.closest('.switch-row');
  if(!input)return;
  if(row)row.onclick=event=>event.stopPropagation();
  input.onchange=()=>setAssistantProvider(provider,input.checked,pipeline,gate);
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

async function integrationsPage() {
  const [d,a,pipeline,live]=await Promise.all([
    api('/integrations'),
    api('/assistant').catch(error=>({unsupported:error.message})),
    api('/voice-pipeline').catch(()=>({mode:'local',stt:{},tts:{}})),
    api('/live').catch(error=>({unsupported:error.message}))
  ]);
  /*
   * integrationBlurb/integrationStatus come from app.js, which loads first.
   * An integration the image was built without reports installed:false; it is
   * listed so the absence is visible, but renders no toggle or save button --
   * enabling it could only ever fail. The handler loop below skips those rows.
   */
  const integrations=d.items.map(x=>collapsiblePanel(x.name,
    `<p class="muted">${integrationBlurb(x)}</p>
    ${x.installed===false?'':toggle('Enabled',x.enabled,'int-'+x.id,x.forced)}
    <div class="status-line"><span class="status-dot ${x.enabled?'ok':''}"></span><span>${integrationStatus(x)}</span></div>
    ${x.installed===false?'':saveButton('save-int-'+x.id)}`
  )).join('');

  const homeAssistantEnabled=d.items.some(x=>x.id==='home-assistant'&&x.enabled);
  if(homeAssistantEnabled) {
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${collapsiblePanel('Managed by Home Assistant',`<div class="assistant-heading"><div><span class="source-pill">Integration</span><h4>Home Assistant</h4><p class="muted">Voice is handled by Home Assistant over the local Wyoming connection. The on-device assistant (Local LLM and ChatGPT) stays stopped while the Home Assistant integration is enabled. Disable it on this page to use the on-device assistant.</p></div><div class="assistant-state"><span class="status-dot ok"></span>Enabled</div></div>`,'assistant-mode')}</section>
      ${integrations}
    </div>`;
  } else if(a.unsupported) {
    const liveEnabled=!live.unsupported&&Boolean(live.enabled);
    const livePanel=assistantProviderPanel({
      title:'GPT-Live',
      description:'Full-duplex speech with your ChatGPT subscription',
      status:live.unsupported?'Unavailable':liveEnabled?'Enabled':'Disabled',
      statusOkay:liveEnabled,
      toggleLabel:'Use GPT-Live',
      toggleId:'use-live-provider',
      enabled:liveEnabled,
      body:liveAssistantBody(live,null,'live'),
      open:false,
      disabled:Boolean(live.unsupported)
    });
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${unsupported(a.unsupported)}${livePanel}</section>
      ${integrations}
    </div>`;
    bindLiveToggle('#use-live-provider',null,null);
    bindLiveVoiceControls(live);
  } else {
    const localSelected=a.provider==='openai-compatible';
    const deviceSelected=a.provider==='openai-codex';
    const localEnabled=localSelected&&Boolean(a.enabled);
    const deviceEnabled=deviceSelected&&Boolean(a.enabled);
    const localConfigured=Boolean(a.base_url);
    const localStatus=localEnabled?'Enabled':localSelected?'Disabled':localConfigured?'Configured':'Not configured';
    /*
     * The ChatGPT account is a prerequisite for every subscription-backed
     * assistant, so its state is read once and drives both panels: the device
     * provider reports it as "Sign-in required" while signed out, and GPT-Live
     * reports service availability and account state separately so a missing
     * daemon never looks like an authentication problem (and vice versa).
     */
    const account=chatgptAccount(a);
    /*
     * The account state wins over the stored enable flag: a credential that
     * expired after the assistant was switched on leaves the daemon reporting
     * enabled with authenticated false, and that is a sign-in problem, not a
     * running assistant.
     */
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
       * closed Home location, the voice assistant and Internet radio. This
       * panel is also independent of the ChatGPT account: a LAN endpoint needs
       * no sign-in, so it is never forced open or disabled by one.
       */
      open:false
    });
    const devicePanel=assistantProviderPanel({
      title:'On Device Voice Assistant',
      description:'ChatGPT subscription with device login',
      status:deviceStatus,
      statusOkay:deviceEnabled&&account.signedIn,
      toggleLabel:'Use On Device Voice Assistant',
      toggleId:'use-device-provider',
      enabled:deviceEnabled,
      body:deviceAssistantBody(a,deviceSelected),
      /*
       * Open while the account needs attention. The device code and the
       * Connect control live inside this panel, and the auth poll re-renders
       * the whole page every few seconds, so a fixed open:false collapsed the
       * panel out from under the code the user was told to enter.
       */
      open:!account.signedIn,
      disabled:!account.signedIn
    });
    const liveAvailable=!live.unsupported;
    const liveEnabled=liveAvailable&&Boolean(live.enabled);
    const liveStatus=!liveAvailable?'Unavailable':!account.signedIn?account.status:liveEnabled?'Enabled':'Disabled';
    const livePanel=assistantProviderPanel({
      title:'GPT-Live',
      description:'Full-duplex speech with your ChatGPT subscription',
      status:liveStatus,
      statusOkay:liveEnabled&&account.signedIn,
      toggleLabel:'Use GPT-Live',
      toggleId:'use-live-provider',
      enabled:liveEnabled,
      body:liveAssistantBody(live,account,'live'),
      open:liveAvailable&&!account.signedIn,
      disabled:!liveAvailable||!account.signedIn
    });
    content.innerHTML=`<div class="integration-grid">
      <section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${localPanel}${devicePanel}${livePanel}</section>
      ${weatherCard(a)}
      ${integrations}
    </div>`;

    bindProviderToggle('#use-local-provider','openai-compatible',pipeline,null);
    bindProviderToggle('#use-device-provider','openai-codex',pipeline,{signedIn:account.signedIn});
    bindLiveToggle('#use-live-provider',a,{signedIn:account.signedIn,available:liveAvailable});
    if(liveAvailable)bindLiveVoiceControls(live);
    /*
     * The sign-in controls are bound for every ChatGPT-backed panel that drew
     * them, not only for the selected provider, and the pending-login poll runs
     * for the account. Local LLM takes part in none of it.
     */
    bindChatgptSignIn('assistant');
    if(liveAvailable)bindChatgptSignIn('live');
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
        await integrationsPage();
        setBusy(false);
      }
    };
    if($('#local-test'))$('#local-test').onclick=()=>post('/assistant/respond',{text:$('#local-test-text').value},'Test response queued');

    if(deviceSelected) {
      bindDirty(['#assistant-model','#assistant-clock-format','#assistant-prompt'],'#save-assistant');
      $('#save-assistant').onclick=()=>mutate('/assistant',{
        provider:'openai-codex',
        /* A save must not turn into an enable the account cannot honour. */
        enabled:deviceEnabled&&account.signedIn,
        model:$('#assistant-model').value.trim(),
        clock_format:$('#assistant-clock-format').value,
        prompt:$('#assistant-prompt').value.trim()
      },'On Device Voice Assistant settings saved');
      if($('#assistant-test'))$('#assistant-test').onclick=()=>post('/assistant/respond',{text:$('#assistant-test-text').value},'Test response queued');
    }
  }

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
