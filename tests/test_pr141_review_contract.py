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

assert "close_kernel_log_inherited(fd);" in http
assert http.count("close_kernel_log_inherited(fd);") >= 3
assert "start_api_worker(c->fd,api,&q)" in http
assert "start_pcm_stream(c->fd,selected_channel)" in http
assert "opendir(\"/proc/self/fd\")" in http
assert "inherited != keep_fd" in http
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
assert "q->https&&c->sessions_path[0]&&le_auth_save_issued_session" in login
assert "le_auth_save_issued_session" in api
assert "if(c->https_active)" not in login
assert "persist_auth_sessions(c)" in api
assert "The session could not be saved after logout" in api
assert "The initial session could not be saved" in api
assert "||(!strcmp(p,\"/api/v1/integrations/radio/play\")&&strcmp(q->method,\"POST\"))" in api
assert "||(!strcmp(p,\"/api/v1/integrations/radio/stop\")&&strcmp(q->method,\"POST\"))" in api
assert "||(!strcmp(p,\"/api/v1/integrations/radio\")&&strcmp(q->method,\"GET\")&&strcmp(q->method,\"PUT\"))" in api
assert "||(!strcmp(p,\"/api/v1/storage/usb/play\")&&strcmp(q->method,\"POST\"))" in api
assert "p[19]==0||p[19]=='?'" in api
assert "static int tls_directory_for_user" in main
assert "chown(path, user->pw_uid, user->pw_gid)" in main
assert "chmod(o.tls_cert,0600)||chmod(o.tls_key,0600)" in main
assert "if(!tp||tls_directory_for_user(tls_dir,tp)" in main

radio_apply_start = api.index("static int radio_apply")
radio_apply_end = api.index("/* The colour arrives", radio_apply_start)
radio_apply = api[radio_apply_start:radio_apply_end]
assert radio_apply.index("radio_save(c, staged, staged_count)") < radio_apply.index("memcpy(c->radio")
assert "return LE_IO;" in radio_apply

assert "close_kernel_log_inherited(fd);" in http
assert "opendir(\"/proc/self/fd\")" in http
assert "inherited != keep_fd" in http
assert "target_end-target_start-1" in http

assert "LE_USB_ESCAPED_MAX" in api
assert "static int usb_relative_path_valid" in api
assert "!usb_relative_path_valid(rel)" in api
assert "if(!ch)return -1;" in api
assert "static int usb_disk_find(char*node,size_t node_size,char*part,size_t part_size)" in api
assert "bytes=fr*(unsigned long long)vfs.f_blocks" in api
assert "if(i+1>=size){overflow=1;continue;}" in api
agentd = Path("src/adapter/agentd.c").read_text(encoding="utf-8")
assert "static int load_history_file" in agentd
assert "load_history_file(&state, backup)" in agentd
assert "A JSON backup contains the recoverable turn ring" in agentd
app = Path("web/js/app.js").read_text(encoding="utf-8")
assert "const persisted=new Map(stations.map(st=>[st.word,st]))" in app
assert "const sameStation=(a,b)=>" in app
assert "stored=persisted.get(v.word)" in app
usb_play = openapi["paths"]["/storage/usb/play"]["post"]
usb_body = usb_play["requestBody"]["content"]["application/json"]["schema"]
assert usb_play["requestBody"]["required"] is True
assert usb_body["required"] == ["path"]
assert usb_body["properties"]["path"]["maxLength"] == 255
assert "storage/usb/play" in docs
assert "Ordinary names such as" in docs
assert "501" in usb_play["responses"]
assert openapi["paths"]["/storage/usb"]["get"]["parameters"][0]["schema"]["maxLength"] == 255
assert "consecutive dots" in openapi["paths"]["/storage/usb"]["get"]["description"]
assert "prctl(PR_SET_PDEATHSIG, SIGTERM)" in Path("src/adapter/radiod.c").read_text(encoding="utf-8")
assert "getppid() != parent" in Path("src/adapter/radiod.c").read_text(encoding="utf-8")
assert "utf8_prefix" in Path("src/adapter/btd.c").read_text(encoding="utf-8")
assert "bond_name_json" in Path("src/adapter/btd.c").read_text(encoding="utf-8")

buttons_start = api.index('if(!strcmp(p,"/api/v1/buttons")')
buttons_end = api.index('if(!strcmp(p,"/api/v1/privacy")', buttons_start)
buttons = api[buttons_start:buttons_end]
assert buttons.index("rc=persist_configuration(c)") < buttons.index("buttons_json(c,r)")
assert "Button settings could not be saved" in buttons
assert "button_action_sounds" in Path("tests/test_config.sh").read_text(encoding="utf-8")

print("PR 141 transport/auth/USB/radio source contract: ok")
