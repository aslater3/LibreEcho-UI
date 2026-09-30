# Voice history integration handoff (#102)

This is the handoff for the canonical private voice-turn history core. The
source, tests and docs in this change are complete and host-verified; the
remaining work is the shared integration wiring (Makefile, test runner, HTTP
layer), which this change deliberately does not touch.

## What is done

- `src/adapter/voice_history.[ch]` — bounded ten-record RAM ring, status
  vocabulary, generation guard, UTF-8-safe JSON serialization.
- `src/adapter/voice_pipeline.[ch]` — pre-transcript outcome callback, bounded
  recognition deadline, failure metrics.
- `src/adapter/agentd.c` — wires the outcome callback, records one canonical
  record per turn, clears both histories together, serves the new commands.
- `tests/test_voice_history.c`, `tests/test_voice_pipeline_outcomes.c`,
  `tests/voice_history_harness.sh` — focused, standalone harnesses.
- `docs/voice-history-core.md` — design and data model.

## Not done here (owned by the integration change)

- `Makefile`: `src/adapter/voice_history.c` is **not** in `AGENTD_SOURCES`, so
  the real target `make build/libreecho-agentd` fails to link today (see the
  evidence below). Add it to `AGENTD_SOURCES` next to `voice_pipeline.c`.
- `tests/run_tests.sh`: the two new tests are only run by the standalone
  harness, not by the aggregate runner. Wire the harness (or the two tests)
  into `run_tests.sh`.
- `src/api.c` + `src/http_server.c` + `web/openapi.json` + `docs/API.md`: the
  new adapter commands are not reachable over HTTP yet (mapping below).

## Exact commands run and results

All on the current working tree, host only, no hardware.

| Command | Result |
|---|---|
| `tests/voice_history_harness.sh` | pass (both harnesses) |
| `cc ... tests/test_voice_history.c src/adapter/voice_history.c` (strict `-Wall -Wextra -Wpedantic -Werror`) | pass |
| `cc ... tests/test_voice_pipeline.c <existing pipeline sources> -lpthread` then run | `voice pipeline: indexed wake audio to streaming STT: ok` |
| `make build/libreecho-agentd` | **fail**: `undefined reference to le_voice_history_*` (unit missing from `AGENTD_SOURCES`) |
| strict direct agentd link: compile `voice_history.c` (`-Werror`) and link it with all `build/adapter/*.o` + `build/{config_store,json,log}.o` | pass, `build/voice-history-harness/libreecho-agentd` produced |

## New / changed source list

New:

- `src/adapter/voice_history.c`
- `src/adapter/voice_history.h`
- `tests/test_voice_history.c`
- `tests/test_voice_pipeline_outcomes.c`
- `tests/voice_history_harness.sh`
- `docs/voice-history-core.md`
- `docs/history-handoff.md` (this file)

Modified:

- `src/adapter/agentd.c`
- `src/adapter/voice_pipeline.c`
- `src/adapter/voice_pipeline.h`

## Adapter commands

New commands (handled in `handle_client` before the control-mutex block, since
they only need `metrics_mutex`):

- `voice_history` — collection, newest first, at most ten records.
- `voice_history_entry` `{"id":<u64>}` — full bounded text for one record.
- `voice_history_clear` — scrub the canonical ring immediately.

The old latency command `history` (twelve timing-only records) is unchanged and
still persisted; `history_clear` now also clears the canonical ring.

## Wire schema

`voice_history` (collection):

```json
{"history_generation":<u64>,"capacity":10,"count":<u>,"preview_chars":<u>,
 "turns":[{"id":<u64>,"timestamp":"<iso8601 local with offset>",
   "status":"completed|stt_failed|assistant_failed|tts_failed|cancelled|timed_out",
   "transcript_preview":"<up to preview_chars bytes>",
   "response_preview":"<...>",
   "transcript_truncated":true|false,"response_truncated":true|false,
   "transcript_length":<stored bytes>,"response_length":<stored bytes>,
   "stt_ms":<u>,"assistant_ms":<u>,"tts_ms":<u>,"error":"<fixed reason>"|null}]}
```

`voice_history_entry` (one record):

```json
{"id":<u64>,"timestamp":"<iso8601>","status":"<status>",
 "transcript":"<up to 512 bytes, codepoint-safe>",
 "response":"<up to 1024 bytes, codepoint-safe>",
 "transcript_truncated":true|false,"response_truncated":true|false,
 "transcript_length":<stored bytes>,"response_length":<stored bytes>,
 "stt_ms":<u>,"assistant_ms":<u>,"tts_ms":<u>,
 "error":"<fixed reason>"|null,"history_generation":<u64>}
```

Notes:

- The whole collection is bounded to fit the 4096-byte adapter message; if the
  escape-heavy worst case would not fit, it degrades to `preview_chars:0`
  (metadata only) rather than dropping a record. Full text always comes from
  `voice_history_entry`.
- Both routes cut text on UTF-8 codepoint boundaries; a malformed input byte is
  replaced with `?`. Fixed reasons only — never stored provider text.

## HTTP mapping to add in the integration change

Existing routes (`src/api.c:2168-2171`, gated in `src/http_server.c:238`):

- `GET  /api/v1/assistant/history` -> adapter `history` (latency)
- `POST /api/v1/assistant/history/clear` -> adapter `history_clear`

Requested mapping for the new core:

- `GET    /api/v1/assistant/history`        -> `voice_history` (collection)
- `GET    /api/v1/assistant/history/{id}`   -> `voice_history_entry` `{"id":<id>}`
- `DELETE /api/v1/assistant/history`        -> `voice_history_clear`

The old latency history must stay reachable (it is a separate persisted
contract the current UI depends on). Keep it under a separate alias rather than
reusing the `history` route, for example a distinct path
(`/api/v1/assistant/latency-history`) or a documented query selector, so the
existing twelve-record contract is preserved. `agent_result` wraps the adapter
payload as the `data` field of `{"ok":true,"data":...}`; error bodies follow the
standard JSON error envelope. Declare the new methods explicitly and add
contract coverage in `tests/test_api.sh`.

## Status lifecycle / privacy review

- One canonical record per terminal turn. Timer, stop, assistant-failure,
  assistant-success, and pre-transcript (STT failure / superseded wake /
  recognition deadline) paths each record exactly once (or upgrade a completed
  record to `tts_failed` at most once). No path records twice.
- Clear semantics: `voice_history_clear` and `history_clear` both scrub the ring
  immediately and bump the generation; an in-flight completion captured before
  the clear is dropped, and a post-storage `tts_failed` upgrade is guarded by the
  same generation.
- Threading: every `voice_history` access takes `metrics_mutex`; the capture
  thread (outcome), dispatch thread (transcript) and playback path
  (`note_tts_failure`) are serialized on it. No unsynchronized access found.
- Privacy: the serialized `error` field is always
  `le_voice_turn_status_reason(status)`, never the stored `reason` buffer, so no
  caller/provider text reaches the wire. Only RAM; no microphone audio or
  transcript on disk (the no-disk check in `test_voice_history.c` passes). Logs
  carry status names and counts only, never transcript or reply text.

### Known boundaries (not defects)

- A pre-transcript failure outcome is recorded with the *current* generation, so
  a failure that is already in flight when the user clears will still appear as
  a new row. This is a deliberate consequence of the pipeline not tracking a
  generation; recording it is preferable to losing a real terminal event.
- A transcript that reaches `voice_transcript` while the device is unconfigured
  / signed out produces no canonical record (there is no assistant reply). If a
  record is wanted there, it needs an explicit product decision.

## Release impact

`Release impact: minor` (new additive API/UI capability), `Release area: UI`,
`Release note: The control centre now shows the ten most recent voice turns
with the recognised text and reply, stored in RAM only.`
