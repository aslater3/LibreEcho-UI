# Private recent voice history core (#102)

This document describes the canonical, private, RAM-only recent voice-turn
history added for issue #102 and how it relates to the existing persisted
latency history.

## What exists now

- **Latency history** (`voice_history` fields inside `agentd`): twelve
  timing-only records (`stt_*_ms`, `first_text_ms`, `first_announce_ms`,
  `first_pcm_ms`, `request_id`, `follow_up`). It is persisted to
  `<agent-config>.history-generation` (mode 0600) and survives a restart. It is
  served by the existing `history` command. It carries **no transcript text**.
- **Canonical voice history** (new): the ten most recent voice turns with the
  recognised transcript and the final human-facing reply, plus status, timings
  and truncation metadata. It is **RAM only** and is served by the new
  `voice_history`, `voice_history_entry` and `voice_history_clear` commands.

The two are deliberately separate. Transcript text is never written through the
persisted latency file, so privacy-sensitive speech does not survive a reboot.

## Unit layout

| File | Responsibility |
|------|----------------|
| `src/adapter/voice_history.[ch]` | Bounded ten-record ring, status vocabulary, JSON serialization, clear/generation guard. No file I/O. |
| `src/adapter/voice_pipeline.[ch]` | Extended with the pre-transcript outcome callback, a bounded recognition deadline and failure metrics. |
| `src/adapter/agentd.c` | Wires the pipeline outcome callback, records completed/failed/cancelled turns and serves the history commands. |

## Data model

A record is assigned a stable numeric `id` and a stable ISO-8601 local
`timestamp` once, at insertion. Text is bounded: `transcript` 768 bytes,
`response` 1536 bytes; a longer canonical value is truncated and flagged. Status
is one of:

`completed`, `stt_failed`, `assistant_failed`, `tts_failed`, `cancelled`,
`timed_out`.

Only human-facing text is stored:

- the recognised transcript (never the follow-up-augmented prompt);
- the final reply that was actually produced (never a raw provider error or
  tool-call payload).

Failure records carry a fixed, human-safe reason
(`speech not recognised`, `assistant failed`, `speech playback failed`,
`cancelled`, `timed out`); provider diagnostics are never surfaced.

Truncation is UTF-8-safe. The stored `transcript`/`response` bounds, the
collection preview and the per-record detail field all stop on a codepoint
boundary, so a value that is stored or served is always valid UTF-8 and never
ends mid-sequence. Invalid input policy: a malformed byte (stray continuation,
bad lead, or a sequence cut short) is replaced with `?`, which keeps the same
bounded width. The serialized failure `error` field is always the fixed reason
above, never the stored `reason` buffer, so no caller/provider text is emitted.

## Turn lifecycle and outcome capture

```text
wake -> STT -> transcript -> assistant/tool -> response -> TTS -> terminal status
```

Each canonical turn is recorded **once**:

- `completed` – the recognised transcript and final reply.
- `stt_failed` – the recogniser failed or produced no usable transcript before
  one existed (empty final transcript, socket closed, or start failure).
- `assistant_failed` – the response provider path failed.
- `tts_failed` – the local speech path failed while speaking the reply. A
  completed record may be upgraded to `tts_failed` at most once if playback
  fails after the record was stored.
- `cancelled` – a wake that could not start a turn because a previous turn was
  still in flight.
- `timed_out` – recognition exceeded the bounded deadline
  (`LE_VOICE_STT_TIMEOUT_MS`, default 30000 ms).

The pipeline reports `stt_failed`, `cancelled` and `timed_out` through
`le_voice_pipeline_set_outcome_callback()`; the transcript callback still
reports turns that reach the assistant, so no failure vanishes.

## Bounded wire response

Ten records of full canonical text cannot fit a 4096-byte adapter message, and
worst-case JSON escaping can double the text. The design therefore uses two
bounded routes:

- **Collection** (`voice_history`): all ten records, newest first, with a short
  bounded text preview (`preview_chars`, up to 24) and explicit
  `transcript_truncated` / `response_truncated` flags plus stored lengths. If
  the escape-heavy worst case would not fit, the collection degrades to
  metadata only (`preview_chars: 0`) instead of dropping a record.
- **Per record** (`voice_history_entry {id}`): the full bounded text for one
  record (transcript up to 512 chars, response up to 1024 chars on the wire,
  with per-field truncation flags). The worst case safely fits 4096 bytes.

Recommended UI flow: `GET` the collection for the list, then fetch one entry per
row (at most ten small requests on the LAN).

## Clearing and generation

`voice_history_clear` (and the existing `history_clear`, which now also clears
the canonical ring) scrubs every record immediately and bumps a generation
counter. A turn captures the generation before it starts and passes it back
when it records; a completion that lands after a clear is dropped rather than
repopulating a scrubbed ring. A post-storage `tts_failed` upgrade is guarded by
the same generation.

## Privacy

- RAM only; no microphone audio and no transcript text on disk.
- The ring is fixed at ten; the eleventh turn evicts the oldest.
- Only human-facing text, fixed failure reasons and timings are exposed.
- Access is through agentd, reachable only via the authenticated management API
  (see the integration handoff for the exact route mapping).

## Focused tests

`tests/voice_history_harness.sh` compiles and runs, without the shared Makefile:

- `tests/test_voice_history.c` – rollover at the 11th turn, newest-first order,
  every terminal status, clear immediacy, generation guard for stale
  completions, worst-case escaping/full-ten sizing, long-text truncation flags,
  UTF-8 codepoint-boundary truncation (stored, preview and detail) and invalid
  input policy, fixed-reason privacy (no stored provider text emitted), detail
  lookup and no-disk verification.
- `tests/test_voice_pipeline_outcomes.c` – the pipeline outcome callback for
  recognition failure, superseded wake, recognition deadline and the unchanged
  transcript success path, against the production capture/dispatch workers.
