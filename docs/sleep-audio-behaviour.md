# Sleep / nursery audio behaviour

Scope: the `audiod` sleep source (issue #100) and how its generated programme
behaves on the shared Platform media bus.  This document states the behaviour
the code implements and the tests that pin it; it is not a substitute for the
Platform engine source.

## Generation

`src/adapter/sleep_generator.h` synthesises, with no bundled sample:

- white / pink / brown noise, and a low-frequency heartbeat with a soft double
  pulse under a `sin^2` window (exactly zero at both pulse edges);
- an optional low-level pink/brown bed under the heartbeat only;
- interleaved S16 stereo at 48000 Hz (the media-bus rate, so nothing is
  resampled).

The output is hard-clamped to `+-LE_SLEEP_AMPLITUDE_CAP` (8000 of 32767), the
level is clamped 1-100, tempo 40-100, and `fade_seconds` 0-3600 (also bounded to
the timer).  Determinism (seeded xorshift32 + closed-form heartbeat phase) is
what lets the host tests assert envelope behaviour without a device.

## Fail-safe amplitude

Every sample is bounded to the cap inside this unit, independent of any
caller-supplied value, so a bad level can never drive the shared engine into
its limiter.  An out-of-range request is rejected by the handler before a
writer is started.

## Stop is bounded, smooth and idempotent

A stop is requested with SIGTERM and rendered as a fade, never a cut.

- **Bounded:** the stop ramp is `LE_SLEEP_FADE_OUT_FRAMES` (14400 frames =
  0.3 s) from the request, regardless of source or level.
- **Starts from the current gain and only descends:** the stop ramp is scaled
  by the gain captured at the request (`stop_start_gain`), sampled *before*
  the stop is armed.  The gain at an absolute frame is then the *minimum* of
  the programme envelope (fade-in, and the pre-timer fade-out when one is
  running), an earlier timer expiry, and this stop ramp.  Because the ramp
  starts exactly at the gain in force and decreases only, the applied envelope
  is monotonically non-increasing from the request: a stop that arrives while
  the programme is still inside its 0.5 s fade-in, or inside a timer fade
  already in progress, continues from where the programme is and can never
  rise.  (Two reviewed defects are closed here.  The first: the old code
  returned the stop ramp alone, so the first frame after a mid-fade-in stop
  jumped from the fade-in gain to ~1.0.  The second: taking the minimum against
  a ramp that still started at 1.0 removed that snap but let the *rising*
  fade-in term win until it crossed the descending ramp, so the envelope rose
  after the stop -- measured 0.120000 at the stop, peaking at 0.450000 before
  falling.  Scaling the ramp by `stop_start_gain` removes both.)
- **Idempotent:** a repeated stop request does not re-arm the ramp, re-extend
  it or re-snapshot the gain; the same bounded ramp runs to completion once.
- **Ceases at zero:** the final rendered frame is exactly silence.

## Interruption, duck and resume on the media bus

Sleep audio is written to the shared media FIFO, the same path as `radiod`.
The Platform PCM engine (`tools/mt8163-arm32/airplay/audio_engine.c`) owns the
mix and defines interruption as **media duck**, not a stop:

- while any higher-priority bus is live (system, announcement, alarm), media
  and AirPlay are scaled by `MEDIA_DUCK_Q15` (8231, about -12 dB) and summed
  with the priority source into the one mono programme;
- an alarm mutes media outright (gain 0) rather than ducking it;
- when the interrupting producer finishes and its bus empties, the very next
  period is mixed at unducked gain again -- the sleep stream resumes; it is
  never torn down, so there is no restart click.

Explicit spoken stop semantics are unchanged: a spoken stop still relies on the
existing bounded fade above, not on the duck path.

## AEC reference

The engine publishes, for every rendered period, the exact mono programme it
hands to the PCM device, via `le_aec_reference_publish` (magic-tagged,
sequence-numbered, with the bus activity mask).  The tap is delivered to a
datagram socket and is deliberately lossy: a missing or slow AEC consumer never
delays the sole owner of the speaker PCM.  Because the published samples are
the rendered mix, they contain the ducked media and the announcement in the
same period they were mixed.

## Verification

- `tests/test_sleep_generator.c` -- cap, determinism, duration, fades,
  heartbeat envelope, validation, the bounded stop ramp, the regression that a
  stop inside the fade-in or an in-progress timer fade starts from the current
  gain (no upward step), that the applied envelope is monotonically
  non-increasing after a stop (never above the stop-time gain, never rising,
  walked one frame at a time), and that a repeated stop -- including at the
  midpoint of a timer fade -- is idempotent and does not re-snapshot the gain.
- `tests/test_sleep_audio_lifecycle.c` -- the real daemon: argument rejection,
  legacy `colour`, status, the SIGTERM stop ramp and child reaping against a
  media-bus file (no ALSA card).
- Platform `tools/mt8163-arm32/airplay/test_sleep_media_arbitration.c` /
  `.sh` -- compiles the production engine under host tinyalsa stubs and drives
  its own `read_sources` / `mix_sources_frame` / `render_period` /
  `write_period`: a generated sleep signal reaches the media mix, an
  announcement ducks media by `MEDIA_DUCK_Q15` and is summed in, the bus
  empties and media resumes unducked, and the AEC reference equals the rendered
  mixed programme with the expected activity mask.  No live ALSA or sysfs path
  is used.
