#!/usr/bin/env python3
"""Contracts for PR 141's bounded TLS and authentication fixes."""
from pathlib import Path

http = Path("src/http_server.c").read_text(encoding="utf-8")
api = Path("src/api.c").read_text(encoding="utf-8")
runner = Path("tests/run_tests.sh").read_text(encoding="utf-8")

assert "close(relay_listener);for(i=0;i<max;i++)if(clients[i].fd>=0)close(clients[i].fd);" in http
assert "relay_ls=socket(AF_INET,SOCK_STREAM,0)" in http
assert "c[i].secure_transport=1" in http
assert "q.https=c->secure_transport;" in http
assert "make build/test-auth-transport" in runner
assert "make build/test-radiod-json" in runner
assert "connect-src 'self' https://geocoding-api.open-meteo.com" in http
assert "json_escape" in Path("src/json.c").read_text(encoding="utf-8")
assert "test_unit.c" in runner

start = api.index("static void auth_login_json")
end = api.index("static void auth_current_json", start)
login = api[start:end]
assert "if(q->https&&c->sessions_path[0])" in login
assert "if(c->https_active)" not in login

print("PR 141 transport/auth source contract: ok")
