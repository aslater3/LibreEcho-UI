#!/bin/sh
set -eu

python3 - <<'PY'
from pathlib import Path

networkd = Path('src/adapter/networkd.c').read_text()
backend_h = Path('src/backend.h').read_text()
backend_linux = Path('src/backend_linux.c').read_text()
backend_mock = Path('src/backend_mock.c').read_text()
api = Path('src/api.c').read_text()
openapi = Path('web/openapi.json').read_text()
ui = Path('web/js/app.js').read_text()
docs = Path('docs/API.md').read_text()

for field in ('connectivity', 'gateway_reachable', 'recovery_stage', 'liveness_failures'):
    assert field in networkd, field
    assert field in backend_h, field
    assert field in backend_linux, field
    assert field in backend_mock, field
    assert field in api, field
    assert field in openapi, field
    assert field in docs, field

assert 'REASSOCIATE\\n' in networkd
assert 'SIOCSIFFLAGS' in networkd
assert '/tmp/reboot.request' in networkd
assert '/dev/wmtWifi' not in networkd
assert networkd.count('reset_network_health(ctx, monotonic_ms());') == 2
main_loop = networkd[networkd.index('int main(int argc, char **argv)'):]
client_action = main_loop.index('read_client(&ctx, j);')
probe_action = main_loop.index('handle_gateway_probe_event(')
health_tick = main_loop.index('check_network_health(&ctx, now);')
assert client_action < probe_action < health_tick
assert 'read_client(ctx, i);' in networkd
assert 'le_network_health_reboot_request_failed' not in networkd
assert 'const char *connectivity = "unknown";' in backend_linux
network_fallback = backend_linux[backend_linux.index('static int network('):
                                 backend_linux.index('static int audio(')]
assert 'o->internet = 0;' in network_fallback
assert "n.connectivity==='healthy'" in ui
assert 'n.recovery_stage' in ui
print('network liveness/recovery cross-layer contract: ok')
PY
