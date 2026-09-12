#!/usr/bin/env python3
# The logs-page health list had no wake-word entry, so a waked that had exited
# -- it dies with micd and does not survive a solo restart -- still read as
# "all healthy". This locks in that the diagnostic now reflects real wake-word
# capture health: the adapter is read directly, a loaded model alone is not
# enough (the frame counter must keep advancing across successive samples),
# VAD is not misread as capture liveness, and the 64-bit counter survives
# parsing, storage and formatting.
import pathlib

api = pathlib.Path("src/api.c").read_text(encoding="utf-8")
api_h = pathlib.Path("src/api.h").read_text(encoding="utf-8")
backend_h = pathlib.Path("src/backend.h").read_text(encoding="utf-8")
backend_linux = pathlib.Path("src/backend_linux.c").read_text(encoding="utf-8")
waked = pathlib.Path("src/adapter/waked.c").read_text(encoding="utf-8")
health = pathlib.Path("src/wake_health.h").read_text(encoding="utf-8")
json_c = pathlib.Path("src/json.c").read_text(encoding="utf-8")
json_h = pathlib.Path("src/json.h").read_text(encoding="utf-8")

# 1) The diagnostics list carries a wake-word check.
assert '{\\"name\\":\\"wake word\\",\\"status\\":\\"%s\\"}' in api, \
    "diagnostics is missing the wake-word check"

# 2) Its status is not a stub: it talks to waked, requires a loaded model, and
#    requires the frame counter to be advancing, so an unreachable adapter, an
#    unloaded model, or a stalled capture stream reports degraded.
diag = api.split("static void diagnostics_json", 1)[1].split("static ", 1)[0]
assert "le_get_wake_word_state(c->backend,&ww)==LE_OK&&ww.model_loaded&&wake_capture_alive(c,&ww)" in diag, \
    "wake-word health is not derived from adapter reachability + model_loaded + advancing counter"
assert '"degraded"' in diag, "wake-word check has no degraded path"

# 3) Successive samples of the counter are retained in the API context and
#    compared; a frozen counter is only called out once a full stale window
#    has passed, and a first sample stays optimistic.
assert "#define LE_WAKE_CAPTURE_STALE_MS 1000ULL" in health
assert "static inline void le_wake_sample_update" in health
assert "s->stalled = frames == s->frames;" in health
assert "struct le_wake_sample wake_capture;" in api_h
assert "le_wake_sample_update(&c->wake_capture,w->processed_frames,wake_now_ms())" in api, \
    "capture health does not sample the frame counter into the retained sample"
assert "static int wake_capture_alive(struct api_context*c,const struct le_wake_word_state*w)" in api
assert "if(!w->model_loaded)return 0;" in api

# 4) The backend keeps the raw adapter facts: VAD is exposed as VAD (a quiet
#    room is not a fault), and the counter is parsed 64-bit, never through
#    json_get_int.
assert 'o->model_loaded = o->model_status[0] && !strcmp(o->model_status, "loaded");' in backend_linux
assert 'json_get_bool(response, "vad_active", &v) > 0) o->vad_active = v;' in backend_linux
assert 'json_get_u64(response, "processed_frames", &frames) > 0) o->processed_frames = frames;' in backend_linux
assert 'json_get_int(response, "processed_frames"' not in backend_linux
assert "model_loaded, vad_active; unsigned long long processed_frames;" in backend_h
assert "capture_active" not in backend_h, \
    "capture_active must not be a stored adapter field"
assert "int json_get_u64(const char *s, const char *k, unsigned long long *out)" in json_c
assert "json_get_u64(const char*,const char*,unsigned long long*)" in json_h

# 5) waked's status emits the live processed-frame counter (the "capture is
#    flowing" signal), sourced from its uint64 metrics and printed as %llu.
assert '\\"processed_frames\\":%llu' in waked, "waked status omits processed_frames"
assert "(unsigned long long)metrics->processed_frames" in waked, \
    "processed_frames is not sourced from live metrics"

# 6) The wake-word API surfaces the raw signals and the derived verdict:
#    vad_active (frame content), capture_active (successive-sample verdict),
#    and the full 64-bit counter.
assert '\\"model_loaded\\":%s' in api and '\\"vad_active\\":%s' in api \
    and '\\"capture_active\\":%s' in api, "wake_json does not surface capture health"
assert '\\"processed_frames\\":%llu' in api, "wake_json does not print the counter 64-bit"
assert '\\"processed_frames\\":%d' not in api, "wake_json still prints the counter as int"
assert "int capture=wake_capture_alive(c,&w);" in api, \
    "capture_active is not the successive-sample verdict"

print("wake-word capture diagnostic: ok")
