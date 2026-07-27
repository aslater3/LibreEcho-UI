/* Provider-aware assistant controls layered over the legacy ChatGPT panel. */
(function () {
  const originalIntegrationsPage = window.integrationsPage;

  window.integrationsPage = async function localIntegrationsPage() {
    const [d, a] = await Promise.all([
      api('/integrations'),
      api('/assistant').catch(e => ({unsupported: e.message}))
    ]);
    if (a.unsupported || a.provider !== 'openai-compatible') {
      return originalIntegrationsPage();
    }
    const local = collapsiblePanel('Local LLM', `<div class="assistant-heading"><div><span class="source-pill">LAN endpoint</span><h4>OpenAI-compatible local model</h4><p class="muted">Transcripts stay on the device until they are sent to this configured endpoint.</p></div><div class="assistant-state"><span class="status-dot ${a.authenticated ? 'ok' : ''}"></span>${a.authenticated ? 'Configured' : 'Not configured'}</div></div><div class="settings-grid assistant-settings"><div>${toggle('Enable wake-to-reply voice loop', a.enabled, 'local-assistant-enabled', !a.authenticated)}${field('Endpoint URL', a.base_url || '', 'local-base-url')}<label class="field"><span>API key (optional)</span><input id="local-api-key" type="password" autocomplete="off" placeholder="Leave blank to keep the current key"></label>${field('Model', a.model, 'local-model')}<label class="field"><span>Voice response prompt</span><textarea id="local-prompt" rows="8">${esc(a.prompt)}</textarea></label>${saveButton('save-local-assistant')}</div><div><dl class="facts"><dt>Wake audio</dt><dd class="${a.wake_connected ? 'connected' : ''}">${a.wake_connected ? 'Connected' : 'Unavailable'}</dd><dt>Post-AEC stream</dt><dd class="${a.audio_connected ? 'connected' : ''}">${a.audio_connected ? 'Connected' : 'Unavailable'}</dd><dt>Speech recognition</dt><dd>${a.recognizing ? 'Recognizing' : 'Ready'}</dd><dt>Completed voice turns</dt><dd>${Number(a.completed_transcripts || 0)}</dd></dl>${a.authenticated ? `<label class="field"><span>Test prompt</span><input id="local-test-text" value="Say hello in one short sentence."></label>${action('Speak test response', 'local-test')}` : ''}</div></div>`, 'assistant-provider');
    const integrations = d.items.map(x => collapsiblePanel(x.name, `<p class="muted">${x.id === 'rest' ? 'Versioned local device API.' : 'Optional local integration; no cloud connection required.'}</p>${toggle('Enabled', x.enabled, 'int-' + x.id)}<div class="status-line"><span class="status-dot ${x.enabled ? 'ok' : ''}"></span><span>${x.enabled ? 'Enabled' : 'Not configured'}</span></div>${saveButton('save-int-' + x.id)}`)).join('');
    content.innerHTML = `<div class="integration-grid"><section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${local}</section>${integrations}</div>`;
    d.items.forEach(x => {
      bindDirty(['#int-' + x.id], '#save-int-' + x.id);
      $('#save-int-' + x.id).onclick = () => mutate('/integrations/' + x.id, {enabled: $('#int-' + x.id).checked}, x.name + ' changes saved');
    });
    bindDirty(['#local-assistant-enabled', '#local-base-url', '#local-model', '#local-prompt', '#local-api-key'], '#save-local-assistant');
    $('#save-local-assistant').onclick = async () => {
      const body = {enabled: $('#local-assistant-enabled').checked, provider: 'openai-compatible', base_url: $('#local-base-url').value.trim(), model: $('#local-model').value.trim(), prompt: $('#local-prompt').value.trim()};
      const key = $('#local-api-key').value;
      if (key) body.api_key = key;
      await mutate('/assistant', body, 'Local LLM settings saved');
    };
    if ($('#local-test')) $('#local-test').onclick = () => post('/assistant/respond', {text: $('#local-test-text').value}, 'Test response queued');
  };
})();
