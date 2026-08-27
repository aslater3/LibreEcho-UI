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

api_c = Path("src/api.c").read_text(encoding="utf-8")
api_h = Path("src/api.h").read_text(encoding="utf-8")

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

print("acoustic events placeholder contract: ok")
