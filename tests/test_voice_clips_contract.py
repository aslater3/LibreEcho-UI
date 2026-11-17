"""Issue #407 contract: wake near-miss clips are off by default, bounded,
deletable from the control centre, and documented in the API reference."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


api = read("src/api.c")
store = read("src/adapter/voice_clip_store.c")
store_h = read("src/adapter/voice_clip_store.h")
waked = read("src/adapter/waked.c")
worker = read("src/adapter/wake_worker.c")
ui = read("web/js/privacy-ui.js")
openapi = read("web/openapi.json")
docs = read("docs/API.md")
suite = read("tests/run_tests.sh")
makefile = read("Makefile")

checks = {
    "list, settings, download, delete and clear routes exist": all(
        route in api
        for route in (
            '"/api/v1/voice/clips"',
            '"/api/v1/voice/clips/clear"',
            '"/api/v1/voice/clips/"',
        )
    ),
    "settings default to off when the file is absent": "settings->near_miss_enabled = 0;" in store
    and "settings->near_miss_enabled = strcmp(text" in store,
    "settings and clips are written 0600 with an atomic rename": "0600" in store
    and "rename(temporary, path)" in store
    and "rename(temp_path, final_path)" in store,
    "count and byte bounds are enforced by the writer": "LE_VOICE_CLIP_MAX_COUNT" in store
    and "LE_VOICE_CLIP_MAX_BYTES" in store,
    "ids are strict and path-safe": "parse_id" in store and "strchr(id, '-')" in store,
    "download is the only non-envelope response and is size checked": 'strcpy(r->type, "audio/wav")' in api
    and "download limit" in api,
    "near-miss band is off unless a floor is set": "near_floor > 0.0f" in worker
    and "floor > 0.0f ? callback : NULL" in worker,
    "waked gates every write on the owner setting": "near_miss_enabled" in waked
    and "le_voice_clip_settings_load" in waked,
    "store is linked into the daemons and the API tests": "src/adapter/voice_clip_store.c" in makefile
    and "test-voice-clip-store" in makefile,
    "suite runs the store test and points the daemon at a private directory": "test-voice-clip-store" in suite
    and "LIBREECHO_VOICE_CLIP_DIR" in suite,
    "control centre lists, plays, downloads, deletes and clears": all(
        token in ui
        for token in (
            "voiceClipsPanel",
            "data-clip-play",
            "data-clip-download",
            "data-clip-delete",
            "clear-voice-clips",
            "/voice/clips/clear",
        )
    ),
    "OpenAPI and API reference describe the endpoints": all(
        token in openapi for token in ('"/voice/clips"', '"/voice/clips/{id}"', '"audio/wav"')
    )
    and all(token in docs for token in ("GET /api/v1/voice/clips", "DELETE /api/v1/voice/clips/{id}")),
    "privacy-ui cache revision moved with the change": 'privacy-ui.js?rev=42' in read("web/index.html"),
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("voice clip contract: " + "; ".join(failed))
print("voice clip contract: ok")
