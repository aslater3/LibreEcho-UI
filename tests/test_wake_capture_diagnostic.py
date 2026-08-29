#!/usr/bin/env python3
# The logs-page health list had no wake-word entry, so a waked that had exited
# -- it dies with micd and does not survive a solo restart -- still read as
# "all healthy". This locks in that the diagnostic now reflects real wake-word
# capture health rather than nothing at all.
import pathlib

api = pathlib.Path("src/api.c").read_text(encoding="utf-8")
backend_h = pathlib.Path("src/backend.h").read_text(encoding="utf-8")
backend_linux = pathlib.Path("src/backend_linux.c").read_text(encoding="utf-8")
waked = pathlib.Path("src/adapter/waked.c").read_text(encoding="utf-8")

# 1) The diagnostics list carries a wake-word check.
assert '{\\"name\\":\\"wake word\\",\\"status\\":\\"%s\\"}' in api, \
    "diagnostics is missing the wake-word check"

# 2) Its status is not a stub: it talks to waked and is gated on a loaded model,
#    so an unreachable adapter or an unloaded model reports degraded.
diag = api.split("static void diagnostics_json", 1)[1].split("static ", 1)[0]
assert "le_get_wake_word_state(c->backend,&ww)==LE_OK&&ww.model_loaded" in diag, \
    "wake-word health is not derived from adapter reachability + model_loaded"
assert '"degraded"' in diag, "wake-word check has no degraded path"

# 3) The backend actually parses the capture-health signals from waked's status.
assert 'o->model_loaded = o->model_status[0] && !strcmp(o->model_status, "loaded");' in backend_linux
assert 'json_get_bool(response, "vad_active"' in backend_linux
assert 'json_get_int(response, "processed_frames"' in backend_linux

# 4) The state struct carries them.
for field in ("model_loaded", "capture_active", "processed_frames"):
    assert field in backend_h, f"le_wake_word_state is missing {field}"

# 5) waked's status emits the live processed-frame counter (the "capture is
#    flowing" signal), sourced from its metrics rather than a constant.
assert '\\"processed_frames\\":%llu' in waked, "waked status omits processed_frames"
assert "(unsigned long long)metrics->processed_frames" in waked, \
    "processed_frames is not sourced from live metrics"

# 6) The wake-word API surfaces the signals so an operator can see them.
assert '\\"model_loaded\\":%s' in api and '\\"capture_active\\":%s' in api \
    and '\\"processed_frames\\":%d' in api, "wake_json does not surface capture health"

print("wake-word capture diagnostic: ok")
