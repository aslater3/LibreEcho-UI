#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
init = Path('init/libreecho-btd.init').read_text()
web = Path('init/libreecho-web.init').read_text()

# btd must tolerate the documented boot ordering: hci0 can exist before the
# controller responds to MGMT Read Controller Information.
assert '#define STARTUP_INFO_RETRY_COUNT' in source
assert '#define STARTUP_INFO_RETRY_DELAY_MS' in source
assert 'wait_for_controller_info' in source
assert 'refresh_info(context)' in source
assert 'btd: controller-info retry' in source
assert 'btd: controller-info unavailable after retries' in source
assert 'BT_READY_PATH' in source
assert 'set_controller_ready(1)' in source
assert 'set_controller_ready(0)' in source

# The retry must happen before the daemon exits on its initial refresh failure.
retry = source.index('wait_for_controller_info')
main = source.index('int main(')
assert retry < main
assert 'wait_for_controller_info(&context)' not in source
assert 'controller not ready; activation remains available' in source
assert source.index('refresh_info(&context)', main) < source.index('le_log_warn("btd: controller not ready', main)
assert 'wait_for_controller_info(context)' in source[source.index('set_powered(context, 1)'):]

# The init wrapper must not claim a failed child is healthy via a stale pidfile.
assert 'start-stop-daemon -S -b -m -p "$PIDFILE"' in init
assert 'is_running' in init
assert 'kill -0' in init

# The startup gate must allow time for WMT/CONSYS activation while btd retries,
# but a deliberately disabled Bluetooth integration must not hold the whole UI
# in its startup animation.
assert 'STARTUP_READY_TIMEOUT_TICKS' in web
assert 'STARTUP_READY_TIMEOUT_TICKS:-600' in web
assert 'bluetooth_integration_state()' in web
assert 'bluetooth_ready()' in web
assert 'BT_READY_PATH=${BT_READY_PATH:-/run/libreecho/bluetooth-ready}' in web
assert 'case "$(bluetooth_integration_state)"' in web
assert '[ -f "$BT_READY_PATH" ]' in web
assert '[ -f /run/libreecho/bluetooth-ready ] || return 1' not in web
PY

printf '%s\n' 'Bluetooth startup readiness retry contract: ok'
