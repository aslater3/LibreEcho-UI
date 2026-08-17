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
assert 'startup controller-info retry' in source
assert 'startup controller-info unavailable after retries' in source

# The retry must happen before the daemon exits on its initial refresh failure.
retry = source.index('wait_for_controller_info')
main = source.index('int main(')
assert retry < main
call = source.index('wait_for_controller_info(&context)')
assert call > main
assert call < source.index('close(listener)', call)

# The init wrapper must not claim a failed child is healthy via a stale pidfile.
assert 'start-stop-daemon -S -b -m -p "$PIDFILE"' in init
assert 'is_running' in init
assert 'kill -0' in init

# The startup gate must allow time for WMT/CONSYS activation while btd retries.
assert 'STARTUP_READY_TIMEOUT_TICKS' in web
assert 'STARTUP_READY_TIMEOUT_TICKS:-600' in web
PY

printf '%s\n' 'Bluetooth startup readiness retry contract: ok'
