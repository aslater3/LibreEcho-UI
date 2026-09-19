# Audio lifecycle contract

## Ownership

The Platform engine is the playback PCM owner. Every managed producer connects
to `/run/libreecho-audio/streams.sock`. One connection is one immutable output
generation. Roles select mixing policy, not cancellation identity. The engine
accepts at most eight clients and 4096 queued stereo frames per client. A Live
session has a separate focus connection and a fresh audio connection per reply.
No inference provider owns ALSA or operates the board amplifier.

The identical `pcm_stream_protocol.h` files in UI and Platform specify the wire:
32-byte little-endian headers, magic `0x3150434c`, messages OPEN/DATA/FINISH/CANCEL/
QUERY/STATE, and at most 1024 S16_LE/48 kHz/stereo frames per DATA packet. OPEN
carries the role and optional focus flag. STATE carries accepted and played frame
counters. Partial/truncated/oversized packets and DATA after FINISH fail closed.

## Completion is not cancellation

FINISH follows the final DATA packet on the same ordered connection. The engine
pads a short final period but accounts only valid frames. DRAINED means the
last valid frame passed the hardware playback cursor. Unknown hardware delay is
not evidence of completion. A temporary empty queue is not end-of-stream.

A producer retains its connection until DRAINED. Closing earlier cancels only
that connection, including queued IPC packets not yet read. Old connections can
never cancel a successor, music or cues. Audio already mixed into ALSA cannot be
selectively retracted: the two 2048-frame periods at 48 kHz bound the hardware
queue to roughly 85 ms, with additional scheduling/driver latency still requiring
measurement. No stronger instantaneous-stop claim is made.

Cold startup writes silence, releases the existing safety mute, then submits
programme audio. The codec safety controls and board's duplicated-mono stereo
routing are retained. A bounded warm-idle interval avoids reopening the output
for brief streaming gaps. A focus connection holds media ducking throughout a
voice interaction; disconnect/failure releases it.

Cues and traditional TTS explicitly permit legacy FIFO fallback for older
images. Only the managed path has stream-scoped completion/cancellation. Live
fails if the managed endpoint is absent. `LE_LIVE_ALLOW_LEGACY_TEST_SINK=1` is an
explicit host-fixture escape hatch, reported as `managed:false`; it must not be
set by production init scripts.

## Conversation and provider boundary

The existing public Realtime adapter now correlates response and item ids,
quarantines cancelled response audio/tool events, sends response.cancel when
generation is active, and truncates the item at the last confirmed played
position (conservatively adjusted to that item's start). A new response.created
re-arms output; it does not depend on the legacy turn.done event. Function-call
continuation waits for the generating response to finish before response.create.
This is the configured Realtime integration, not a new implementation of the
separate continuous GPT-Live API. Existing authentication selection is unchanged;
there is no silent provider, account or billing fallback.

The Live output queue and WebSocket queues are fixed-size and nonblocking.
EAGAIN retains exact data, zero-timeout reads really return immediately, and
partial WebSocket headers/payloads survive polls. TLS retries retain the same
pending chunk. Wake event fragments are retained and a wake arriving ahead of
its separate indexed PCM stream waits at most one second for that sample.
Initial network connection setup remains bounded synchronous work.

Half-duplex remains the production default to avoid echo-driven self-turns.
`--full-duplex` enables continuous post-AEC upload and local/server-VAD
interruption, including during drain. Local wake interruption works in either
mode. This opt-in still requires AEC reference/capture alignment, clock-drift and
room double-talk qualification on hardware. It does not certify natural barge-in
at every playback volume or distance. The traditional agent's independent wake/
turn coordinator and a common provider/tool registry remain follow-on work.

## Verification gates

Host regressions exercise 1/2047/2048/2049-frame finite clips, independent
progress/cancellation over simultaneous media, bounded queues, exact resampler
finite lengths at several rates, late response rejection, played-position
truncation, function-call continuation, TCP fragmentation and zero-timeout I/O.
Run `make test-lived` in UI and
`python tools/mt8163-arm32/airplay/test_audio_period_buffer.py` in Platform.

The paired branches must be built into one image. Hardware gates remain:
acoustic first/last-sample capture on cold/warm playback; music/wake and natural
double-talk across volumes; capture clock continuity; AEC delay/discontinuity
measurements; interruption latency under CPU/network load. Source and synthetic
PCM tests do not establish audible success or certify codec readiness.

## Paired source acceptance

With both feature checkouts available, run
`sh tests/test_pcm_cross_repo.sh ../LibreEcho-Platform`. This compiles the real
UI output client and Platform managed-stream server into one bounded host test.
It checks exact resampled tails, stream-specific drain, cancellation with music
and cues continuing, successor replies, and conversation focus lifetime. The
hardware cursor is simulated; this is not an acoustic or ALSA acceptance test.
