#!/usr/bin/env python3
"""Regression contract for the acoustic-event-detection placeholder flag.

The flag reserves the setting; nothing implements the detection. The danger in
a placeholder like this is not that it fails to work -- it is that someone
wires a UI to the flag alone and the device then tells a person their smoke
alarm is being listened for when no code is listening for anything. So the API
reports `acoustic_events_available: false` next to it, hardcoded, and a client
is expected to gate on that.

This test exists to keep the honest half from being deleted once the flag
starts looking like a real feature.
"""

from pathlib import Path
import json

api_c = Path("src/api.c").read_text(encoding="utf-8")
api_h = Path("src/api.h").read_text(encoding="utf-8")
docs = Path("docs/API.md").read_text(encoding="utf-8")
openapi = json.loads(Path("web/openapi.json").read_text(encoding="utf-8"))

assert "feature_acoustic_events" in api_h, "the flag is not declared on the api context"

# Stored like the other feature flags, so the preference survives a restart.
assert 'json_get_bool(saved,"feature_acoustic_events"' in api_c, (
    "the flag is not read back from the persisted configuration"
)
assert '\\"feature_acoustic_events\\": %s' in api_c, (
    "the flag is not written to the persisted configuration"
)

# Reported, and reported as unavailable.
assert '\\"acoustic_events\\":%s' in api_c, "features does not report the flag"
assert '\\"acoustic_events_available\\":false' in api_c, (
    "features must report acoustic_events_available:false while nothing "
    "implements detection -- a client gating on the flag alone would claim "
    "the device is listening for smoke alarms when it is not"
)
assert '\\"acoustic_events_available\\":true' not in api_c, (
    "acoustic event detection has been marked available; if it is genuinely "
    "implemented, replace this contract rather than deleting the guard"
)

# Settable, so a UI can store the preference ahead of the implementation.
assert 'json_get_bool(q->body,"acoustic_events"' in api_c, (
    "the features endpoint does not accept the flag"
)
assert "not yet implemented" in api_c, (
    "the log line no longer says the preference is not implemented"
)
assert "want_https<0||want_sim<0||want_aed<0" in api_c, (
    "mixed feature updates must reject every malformed supplied boolean"
)

# The public docs and OpenAPI contract describe both response fields and permit
# an acoustic-only request without inventing a required simulation field.
assert '"acoustic_events": false' in docs
assert '"acoustic_events_available": false' in docs
feature_path = openapi["paths"]["/system/features"]
request_schema = feature_path["put"]["requestBody"]["content"]["application/json"]["schema"]
assert "simulation" not in request_schema.get("required", [])
assert request_schema["properties"]["acoustic_events"]["type"] == "boolean"
response_ref = feature_path["get"]["responses"]["200"]["$ref"]
response_name = response_ref.rsplit("/", 1)[-1]
response_schema = openapi["components"]["responses"][response_name]["content"]["application/json"]["schema"]
data_ref = response_schema["properties"]["data"]["$ref"]
data_name = data_ref.rsplit("/", 1)[-1]
data_schema = openapi["components"]["schemas"][data_name]
for field in ("acoustic_events", "acoustic_events_available"):
    assert field in data_schema["required"]
    assert data_schema["properties"][field]["type"] == "boolean"
assert "always false" in data_schema["properties"]["acoustic_events_available"]["description"].lower()

print("acoustic events placeholder contract: ok")
