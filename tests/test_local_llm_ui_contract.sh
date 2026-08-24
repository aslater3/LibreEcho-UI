#!/bin/sh
set -eu

# Contract for the Local LLM integration panel.
#
# agentd has always accepted provider/base_url/api_key/model through
# PUT /api/v1/assistant, and llm_openai.c registers the "openai-compatible"
# provider. The web UI was the only thing missing: the Local LLM panel was a
# fixed placeholder and the save handler hard-coded provider 'openai-codex',
# so the local provider was unreachable from the browser.
#
# This is host-verifiable source/UI evidence. Talking to a real endpoint
# remains a separate validation gate.
python3 - <<'PY'
from pathlib import Path

ui = Path('web/js/app.js').read_text()

# The placeholder is gone.
assert 'Provider setup will appear here' not in ui, 'placeholder copy still present'
assert 'when a reviewed model is installed' not in ui

# Real controls exist.
for control in ('local-base-url', 'local-model', 'local-api-key',
                'local-enabled', 'save-local', 'local-test'):
    assert f"id=\"{control}\"" in ui or f"'#{control}'" in ui, f'missing control {control}'

# Both providers are reachable; neither is the only option.
assert "'openai-compatible'" in ui, 'local provider id never sent'
assert "'openai-codex'" in ui, 'ChatGPT provider id lost'

# The local save path sends the endpoint fields.
assert 'base_url:' in ui, 'base_url never submitted'
assert 'api_key' in ui, 'api_key never handled'

# The API key is write-only. agentd reports api_key_configured and never
# returns the key, so the UI must not try to render one back into the DOM.
assert 'a.api_key_configured' in ui, 'configured flag not consulted'
assert 'value:a.api_key' not in ui
assert 'esc(a.api_key)' not in ui
assert "field('API key" not in ui or "'password'" in ui, 'API key field must be a password input'

# Clearing a stored key is possible without retyping it.
assert 'local-clear-key' in ui, 'no way to clear a stored API key'

# Endpoint validation mirrors agentd endpoint_valid loosely, client side.
assert 'http://' in ui and 'https://' in ui, 'no scheme validation'

print('local LLM UI contract: ok')
PY

node --check web/js/app.js
echo 'local llm ui contract: ok'
