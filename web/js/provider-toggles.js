/* Provider selection is explicit and mutually exclusive. */
(function () {
  function inject() {
    document.querySelectorAll('details').forEach(details => {
      const heading = details.querySelector(':scope > summary h3');
      const body = details.querySelector(':scope > .integration-section-body');
      if (!heading || !body || body.querySelector('.provider-toggle')) return;
      const local = heading.textContent === 'Local LLM';
      const device = heading.textContent === 'On Device Voice Assistant' || heading.textContent === 'ChatGPT';
      if (!local && !device) return;
      const id = local ? 'provider-local' : 'provider-device';
      const checked = local ? window.libreEchoAssistantProvider === 'openai-compatible' : window.libreEchoAssistantProvider === 'openai-codex';
      const row = document.createElement('label');
      row.className = 'switch-row provider-toggle';
      row.innerHTML = '<span>' + (local ? 'Use Local LLM' : 'Use On Device Voice Assistant') + '</span><input class="toggle-input" id="' + id + '" type="checkbox" ' + (checked ? 'checked' : '') + '><span class="switch"></span>';
      body.prepend(row);
      const input = row.querySelector('input');
      input.onchange = async event => {
        if (!event.target.checked) { event.target.checked = true; return; }
        const provider = local ? 'openai-compatible' : 'openai-codex';
        try {
          await api('/assistant', {method:'PUT', body:JSON.stringify({provider, enabled:true})});
          window.libreEchoAssistantProvider = provider;
          await integrationsPage();
        } catch (error) { event.target.checked = false; toast(error.message, true); }
      };
      row.onclick = event => {
        if (event.target !== input) {
          event.preventDefault();
          input.checked = !input.checked;
          input.dispatchEvent(new Event('change', {bubbles:true}));
        }
      };
    });
  }
  window.libreEchoAssistantProvider = 'openai-codex';
  api('/assistant').then(a => { window.libreEchoAssistantProvider = a.provider || 'openai-codex'; inject(); }).catch(() => {});
  new MutationObserver(inject).observe(document.body, {childList:true, subtree:true});
  inject();
})();
