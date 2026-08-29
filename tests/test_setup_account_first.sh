#!/bin/sh
set -eu

HTML=web/setup.html
JS=web/js/setup.js

# The account step must be the first wizard step and the network step must be later.
python3 - "$HTML" "$JS" <<'PY'
from pathlib import Path
import sys
html=Path(sys.argv[1]).read_text()
js=Path(sys.argv[2]).read_text()
assert html.index('id="setup-username"') < html.index('id="setup-ssid"')
assert 'data-step="0"' in html and 'data-step="2"' in html
assert 'Create your local account.' in html
assert "api('/auth/bootstrap'" in js
assert "sessionStorage.setItem('libreecho-token'" in js
assert "if(config.bootstrap_required){render();return}" in js
assert "const current=await api('/setup')" in js
assert js.index("api('/auth/bootstrap'") < js.index("api('/setup')")
assert 'Authorization:`Bearer ${setup.token}`' in js
print('setup account-first browser contract: ok')
PY
