#!/bin/sh
set -eu

# Regression contract for issue #77: numerical Bluetooth pairing codes must be
# presented as a prominent left-aligned value instead of a small status-line
# detail. This is host-verifiable source/UI evidence; hardware pairing remains
# a separate validation gate.
python3 - <<'PY'
from pathlib import Path

ui = Path('web/js/bluetooth.js').read_text()
css = Path('web/css/app.css').read_text()

assert 'function bluetoothPairingCode(value)' in ui
assert 'class="pairing-code"' in ui
assert 'class="pairing-code-value"' in ui
assert 'role="status"' in ui
assert "(p.method==='confirm'||p.method==='notify')" in ui
assert 'bluetoothPairingCode(p.value)' in ui

pairing_code = css[css.index('.pairing-code{'):]
assert 'text-align:left' in pairing_code
assert '.pairing-code-value{' in pairing_code
value_style = pairing_code[pairing_code.index('.pairing-code-value{'):]
assert 'font-size:' in value_style
assert 'font-weight:800' in value_style
PY
printf '%s\n' 'bluetooth pairing-code UI contract: ok'
