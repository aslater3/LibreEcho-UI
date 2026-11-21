#!/usr/bin/env bash
# The Sendspin native lanes that the default suite skips: the C++20 engine-sink
# client against the real C99 engine (plain, ASan+UBSan, TSan), the host SDK
# lane through the real engine loop, and two-player interop with the pinned
# server oracle. Needs SENDSPIN_PLATFORM_DIR (a Platform checkout); fetches the
# pinned archives into SENDSPIN_ARCHIVE_DIR (default $TMPDIR/sendspin-archives).
set -euo pipefail
: "${SENDSPIN_PLATFORM_DIR:?set SENDSPIN_PLATFORM_DIR to a LibreEcho-Platform checkout}"
work=${TMPDIR:-/tmp}
export SENDSPIN_ARCHIVE_DIR=${SENDSPIN_ARCHIVE_DIR:-$work/sendspin-archives}
python3 tests/ci/fetch_sendspin_archives.py "$SENDSPIN_PLATFORM_DIR" "$SENDSPIN_ARCHIVE_DIR"
if [ -z "${SENDSPIN_SERVER_PYTHON:-}" ]; then
    python3 -m venv "$work/sendspin-server-venv"
    "$work/sendspin-server-venv/bin/python" -m pip install --quiet -r tests/ci/sendspin-server-requirements.txt
    export SENDSPIN_SERVER_PYTHON=$work/sendspin-server-venv/bin/python
fi
for lane in test-sendspin-sink test-sendspin-sink-sanitize test-sendspin-sink-tsan \
            test-sendspin-host test-sendspin-interop; do
    echo "== $lane"
    make -B "$lane"
done
echo 'sendspin native lanes: ok'
