#!/usr/bin/env python3
"""Contracts for PR 141's bounded TLS and authentication fixes."""
from pathlib import Path
import json

http = Path("src/http_server.c").read_text(encoding="utf-8")
api = Path("src/api.c").read_text(encoding="utf-8")
runner = Path("tests/run_tests.sh").read_text(encoding="utf-8")
main = Path("src/main.c").read_text(encoding="utf-8")
openapi = json.loads(Path("web/openapi.json").read_text(encoding="utf-8"))
docs = Path("docs/API.md").read_text(encoding="utf-8")

assert "close(relay_listener);for(i=0;i<max;i++)if(clients[i].fd>=0)close(clients[i].fd);" in http
assert "relay_ls=socket(AF_INET,SOCK_STREAM,0)" in http
assert "c[i].secure_transport=1" in http
assert "q.https=c->secure_transport;" in http
assert "make build/test-auth-transport" in runner
assert "make build/test-radiod-json" in runner
assert "connect-src 'self' https://geocoding-api.open-meteo.com" in http
assert "json_escape" in Path("src/json.c").read_text(encoding="utf-8")
assert "test_unit.c" in runner

mac_start = main.index("static void apply_saved_mac_overrides")
mac_end = main.index("/* mkdir -p", mac_start)
mac_restore = main[mac_start:mac_end]
assert "address_rc = run_first_program(ip_paths, set);" in mac_restore
assert "up_rc = run_first_program(ip_paths, up);" in mac_restore
assert mac_restore.index("address_rc =") < mac_restore.index("up_rc =")
assert "Saved Wi-Fi MAC address could not be applied at boot" in mac_restore

network_schema = openapi["components"]["requestBodies"]["NetworkUpdate"]["content"][
    "application/json"]["schema"]
assert network_schema["properties"]["wifi_mac"]["pattern"].startswith("^$")
assert network_schema["properties"]["bt_mac"]["pattern"].startswith("^$")
assert "restored after restart" in openapi["paths"]["/assistant/history"]["get"][
    "description"]
assert "restored after that daemon restarts" in docs

start = api.index("static void auth_login_json")
end = api.index("static void auth_current_json", start)
login = api[start:end]
assert "if(q->https&&c->sessions_path[0])" in login
assert "if(c->https_active)" not in login

print("PR 141 transport/auth source contract: ok")
