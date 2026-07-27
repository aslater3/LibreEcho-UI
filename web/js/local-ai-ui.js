/* Keep local-provider setup visible before a provider is selected. */
async function integrationsPage() {
  const pair = await Promise.all([api('/integrations'), api('/assistant').catch(e => ({unsupported:e.message}))]);
  const d = pair[0], a = pair[1];
  const cards = d.items.map(x => collapsiblePanel(x.name, '<p class="muted">Optional local integration; no cloud connection required.</p>' + toggle('Enabled', x.enabled, 'int-' + x.id) + saveButton('save-int-' + x.id))).join('');
  const legacy = a.provider === 'openai-compatible' ? '' : assistantCard(a).replace(/^<section[^>]*><h3>Voice Assistants<\/h3>/, '').replace(/<\/section>$/, '');
  content.innerHTML = '<div class="integration-grid"><section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>' + localAssistantPanel(a) + legacy + '</section>' + cards + '</div>';
  d.items.forEach(x => { bindDirty(['#int-' + x.id], '#save-int-' + x.id); $('#save-int-' + x.id).onclick = () => mutate('/integrations/' + x.id, {enabled: $('#int-' + x.id).checked}, x.name + ' changes saved'); });
  bindDirty(['#local-assistant-enabled', '#local-base-url', '#local-model', '#local-prompt', '#local-api-key'], '#save-local-assistant');
  $('#save-local-assistant').onclick = async () => { const body = {enabled: $('#local-assistant-enabled').checked, provider: 'openai-compatible', base_url: $('#local-base-url').value.trim(), model: $('#local-model').value.trim(), prompt: $('#local-prompt').value.trim()}; const key = $('#local-api-key').value; if (key) body.api_key = key; await mutate('/assistant', body, 'Local LLM settings saved'); };
}
