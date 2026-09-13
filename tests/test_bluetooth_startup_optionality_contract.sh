#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SOURCE=init/libreecho-web.init
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-bt-ready.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

python3 - "$SOURCE" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
assert "bluetooth_integration_state()" in source
assert "bluetooth_ready()" in source
assert "integrations & 8" in source
assert 'BT_READY_PATH=${BT_READY_PATH:-/run/libreecho/bluetooth-ready}' in source
assert 'case "$(bluetooth_integration_state)"' in source
assert "unknown" in source
assert '[ -f "$BT_READY_PATH" ]' in source
assert '[ -f /run/libreecho/bluetooth-ready ] || return 1' not in source
PY

HELPERS=$(sed -n '/^bluetooth_integration_state()/,/^}/p; /^bluetooth_ready()/,/^}/p' "$SOURCE")
CONFIG_FILE=$WORK/web-config.json
BT_READY_PATH=$WORK/bluetooth-ready
export CONFIG_FILE BT_READY_PATH

echo '{"integrations":4}' >"$CONFIG_FILE"
eval "$HELPERS"
bluetooth_ready

echo '{"integrations":12}' >"$CONFIG_FILE"
if bluetooth_ready; then
    echo 'FAIL: enabled Bluetooth without readiness marker was accepted' >&2
    exit 1
fi
: >"$BT_READY_PATH"
bluetooth_ready

echo '{"integrations":"broken"}' >"$CONFIG_FILE"
if bluetooth_ready 2>/dev/null; then
    echo 'FAIL: malformed Bluetooth integration state was accepted' >&2
    exit 1
fi

echo 'Bluetooth startup readiness optionality contract: ok'
