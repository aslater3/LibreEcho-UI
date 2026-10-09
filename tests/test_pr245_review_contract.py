#!/usr/bin/env python3
"""PR #245 contracts that need source/contract checks rather than a live server.

Findings:
  * the integration operation must declare the asynchronous Home Assistant
    pipeline statuses (202/409/501/503) and both the OpenAPI document and the API
    guide must describe the pending voice-pipeline body;
  * the integration description must describe restoration of the saved pipeline
    mode (local is only the fallback), matching docs/API.md and the handler;
  * the voice-pipeline status must probe the saved custom stt/tts endpoints only
    in custom mode and surface the ESPHome satellite readiness separately, using
    the same predicate the init script uses.
"""
import json
from pathlib import Path

api = Path("src/api.c").read_text(encoding="utf-8")
openapi = json.loads(Path("web/openapi.json").read_text(encoding="utf-8"))
docs = Path("docs/API.md").read_text(encoding="utf-8")
runner = Path("tests/run_tests.sh").read_text(encoding="utf-8")
init = Path("init/libreecho-web.init").read_text(encoding="utf-8")

# --- finding 4: custom-only endpoint probes + separate HA readiness ---------
start = api.index("static void voice_pipeline_json")
end = api.index("static int voice_pipeline_update", start)
body = api[start:end]
assert "network_pipeline" not in body, "custom endpoints must not be probed in HA mode"
assert body.count("custom &&") == 2
assert body.index("esphome_satellite_ready()") > body.index("home_assistant =")
assert r'\"home_assistant\":{\"protocol\":\"esphome\",\"port\":6053,\"ready\":%s,\"connected\":%s}' in body

# Readiness uses the shared bounded process/status/listener verifier. A pidfile
# alone, forged argv[0], stale JSON or an unrelated listener is not readiness.
health = Path("src/esphome_health.h").read_text(encoding="utf-8")
native_runner = Path("tests/test_esphomed_run.py").read_text(encoding="utf-8")
assert "esphome_service_ready()" in init
assert '"$ESPHOMED_DAEMON" --health-check' in init
assert "ESPHOMED_PIDFILE" in init and "ESPHOME_STATUS_FILE" in init
assert "#define LE_ESPHOMED_PIDFILE" in api
assert '#include "esphome_health.h"' in api
assert "le_esphome_health_default(esphomed_pidfile_path(),key)" in api
assert "service_ready(esphomed_pidfile_path(),NULL)" not in api
assert "S_ISREG(st.st_mode)" in health and "data[2049]" in health
for predicate in ("le_eh_executable", "le_eh_start", "le_eh_boot", "le_eh_listener"):
    assert predicate in health
assert 'esphome_status_flag("ready")' in api
assert 'esphome_status_flag("connected")' in api
assert "json_get_top_level_bool" in health and "json_duplicate_key" in health
for override in ("LIBREECHO_ESPHOMED_PIDFILE", "LIBREECHO_ESPHOME_STATUS_FILE"):
    assert override in api + health, f"missing readiness override {override}"
# Real process/listener negative cases remain part of the normal gate; these
# source assertions do not replace the behavioral health regression.
assert "tests/test_esphome_health.py" in native_runner
assert "\nmake test-esphome\n" in runner
makefile = Path("Makefile").read_text(encoding="utf-8")
gate = makefile.split("\ntest-esphome:", 1)[1].split("\n# Execute real HA", 1)[0]
assert "python3 tests/test_esphomed_run.py" in gate

# --- finding 3: declared statuses and documented pending shape --------------
put = openapi["paths"]["/integrations/{id}"]["put"]
responses = put["responses"]
for status in ("200", "202", "404", "405", "409", "501", "503"):
    assert status in responses, f"missing {status} response"
assert responses["202"]["content"]["application/json"]["schema"]["$ref"].endswith(
    "VoicePipelineEnvelope")
envelope = openapi["components"]["schemas"]["VoicePipelineEnvelope"][
    "properties"]["data"]["properties"]
assert "home_assistant" in envelope
assert "mode" in envelope
for marker in ("202", "409", "501", "503", "pending"):
    assert marker in docs, f"API guide must document {marker}"

# --- finding 5: the description must match the restore behaviour ------------
description = put["description"]
assert "previous Local/Custom selection" in description and "disabling restores it" in description, (
    "OpenAPI must describe restoring the saved mode, not always local"
)
assert "restores the previous Local" in docs, (
    "API guide must describe restoring the saved mode"
)

# --- finding 6 (DISPROVED): non-Home-Assistant toggles are already persisted -
# The finding claims the non-Home-Assistant early return skips the only
# persist_configuration() call, so those toggles revert after a restart. That is
# false: the shared HTTP server persists every successful PUT to
# /api/v1/integrations/* itself, after api_handle() returns. Writing again inside
# after_integration_change would be a second atomic write per request and could
# replace the just-saved configuration's backup, so the guard must stay.
http = Path("src/http_server.c").read_text(encoding="utf-8")
assert "api_persist_configuration(api)" in http, (
    "the shared HTTP server must persist successful integration PUTs"
)
assert '!strncmp(q.path,"/api/v1/integrations/",21)' in http, (
    "the shared persistence must cover every integration id, not only Home Assistant"
)
after = api.index("static void after_integration_change")
after_body = api[after:]
guard = after_body.index('strstr(q->path, "home-assistant")')
first_persist = after_body.index("persist_configuration(c)")
assert guard < first_persist, (
    "the non-Home-Assistant guard must return before any persistence write in "
    "after_integration_change; the shared HTTP server owns that write"
)
assert "the toggle silently reverts" not in after_body

# --- finding 7: the wake-word path must be a socket, not any file -----------
# init/libreecho-web.init uses `-S`; a stale regular file must not report ready.
assert "S_ISSOCK(st.st_mode)" in api, (
    "the readiness predicate must require a socket at the wake-word path"
)
assert "access(socket_path,F_OK)" not in api

# The behavioral regression that exercises the live mock server; make sure it
# stays wired into the aggregate runner.
assert "tests/test_voice_pipeline_ha_transitions.sh" in runner

print("PR 245 Home Assistant pipeline contracts: ok")
