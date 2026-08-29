#!/bin/sh
set -eu

HTML=web/setup.html
JS=web/js/setup.js
API=src/api.c

# The account step must be the first wizard step and the network step must be later.
python3 - "$HTML" "$JS" "$API" <<'PY'
from pathlib import Path
import sys
html=Path(sys.argv[1]).read_text()
js=Path(sys.argv[2]).read_text()
api=Path(sys.argv[3]).read_text()
assert html.index('id="setup-username"') < html.index('id="setup-ssid"')
assert 'data-step="0"' in html and 'data-step="2"' in html
assert 'Create your local account.' in html
assert "api('/auth/bootstrap'" in js
assert "sessionStorage.setItem('libreecho-token'" in js
assert "if(config.bootstrap_required){render();return}" in js
assert "const current=await api('/setup')" in js
assert js.index("api('/auth/bootstrap'") < js.index("api('/setup')")
assert "button.disabled=false" in js
assert "if(current.completed){location.replace('/')" in js
assert "setup.step<=1" in js
assert "setup/vendor-import-force-next-boot" in js
assert "Account created. Please reload this page" in js
assert "retrying automatically" in js
assert "setup.scanAttempts++<8" in js
assert "Initial setup could not connect to Wi-Fi. Check the network name, password, and WPA2 security, then try again." in api
print('setup account-first browser contract: ok')
PY
