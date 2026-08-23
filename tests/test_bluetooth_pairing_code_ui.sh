#!/bin/sh
set -eu

# Regression contract for issue #77: numerical Bluetooth pairing codes must be
# presented as a prominent left-aligned value instead of a small status-line
# detail. This is host-verifiable source/UI evidence; hardware pairing remains
# a separate validation gate.
python3 - <<'PY'
from pathlib import Path

ui = Path('web/js/bluetooth.js').read_text()
html = Path('web/index.html').read_text()
css = Path('web/css/app.css').read_text()

assert 'function bluetoothPairingCode(value)' in ui
assert 'class="pairing-code"' in ui
assert 'class="pairing-code-value"' in ui
assert "live.setAttribute('role','status')" not in ui
assert 'aria-hidden="true"' in ui
assert 'updateBluetoothLiveRegion' in ui
assert "const live=document.getElementById('bt-pairing-live')" in ui
assert 'document.createElement' not in ui
assert 'appendChild(live)' not in ui
assert "(p.method==='confirm'||p.method==='notify')" in ui
assert 'bluetoothPairingCode(p.value)' in ui

live = '<div id="bt-pairing-live" class="sr-only" role="status" aria-live="polite" aria-atomic="true"></div>'
assert html.count('id="bt-pairing-live"') == 1
assert live in html
assert html.index(live) < html.index('<script src="/js/bluetooth.js')

pairing_code = css[css.index('.pairing-code{'):]
assert 'text-align:left' in pairing_code
assert '.sr-only{' in css
assert 'clip:rect(0,0,0,0)' in css
assert '.pairing-code-value{' in css
value_style = pairing_code[pairing_code.index('.pairing-code-value{'):]
assert 'font-size:' in value_style
assert 'font-weight:800' in value_style
PY
printf '%s\n' 'bluetooth pairing-code UI contract: ok'
