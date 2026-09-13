#!/usr/bin/env python3
"""PR #245 contracts that need source/contract checks rather than a live server.

Findings:
  * the integration operation must declare the asynchronous Home Assistant
    pipeline statuses (202/409/501/503) and both the OpenAPI document and the API
    guide must describe the pending voice-pipeline body;
  * the voice-pipeline status must probe the saved custom stt/tts endpoints only
    in custom mode and surface the Wyoming satellite readiness separately.
"""
import json
from pathlib import Path

api = Path("src/api.c").read_text(encoding="utf-8")
openapi = json.loads(Path("web/openapi.json").read_text(encoding="utf-8"))
docs = Path("docs/API.md").read_text(encoding="utf-8")
runner = Path("tests/run_tests.sh").read_text(encoding="utf-8")

# --- finding 4: custom-only endpoint probes + separate HA readiness ---------
start = api.index("static void voice_pipeline_json")
end = api.index("static int voice_pipeline_update", start)
body = api[start:end]
assert "network_pipeline" not in body, "custom endpoints must not be probed in HA mode"
assert body.count("custom &&") == 2
assert body.index("service_ready(LE_WYOMINGD_PIDFILE, NULL)") > body.index(
    "home_assistant =")
assert r'"\"home_assistant\":{\"ready\":%s},' in body
assert "#define LE_WYOMINGD_PIDFILE" in api

# --- finding 3: declared statuses and documented pending shape --------------
responses = openapi["paths"]["/integrations/{id}"]["put"]["responses"]
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

# The behavioral regression that exercises findings 1 and 2 runs against the
# live mock server; make sure it stays wired into the aggregate runner.
assert "tests/test_voice_pipeline_ha_transitions.sh" in runner

print("PR 245 Home Assistant pipeline contracts: ok")
