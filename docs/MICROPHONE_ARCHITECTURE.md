# LibreEcho microphone architecture

## Current contract

`libreecho-micd` is the sole owner of microphone capture. It starts at boot but
does not open ALSA until a client explicitly requests a stream.

The confirmed hardware endpoint is:

```text
ALSA card 0, device 24
9 channels
S24_3LE
16 kHz
```

Each capture word is left-justified: the 16 valid bits sit in the top two
bytes and the low byte is padding. The decoder reads the top 16 bits and
ignores the low byte. (Treating the word as a 24-bit integer makes every
sample 256 times too large, so the stream clips at any gain and a quiet room
measures around 2400 RMS instead of the array's real 12-17.)

Raw lanes 0 through 6 carry the seven physical microphones. Lanes 7 and 8 are
inactive or reserved transport lanes and are not offered to consumers. The
stock Radar `audio_device.xml` profile must be applied before capture: all four
TLV320AIC3101 ADCs use their DIF1 left/right inputs, 20 dB analog PGA gain, and
FPGA timestamps disabled.

The daemon exposes `/run/libreecho/mic.sock` using the normal LibreEcho JSON
line protocol:

- `status` returns capture availability, activity, raw format, per-device
  `miccal.0` through `miccal.6` metadata, the measured geometry and the
  fixed-beam pipeline settings. It never starts capture.
- `stream_raw` returns one JSON response line and then switches that connection
  to the interleaved raw PCM byte stream.
- `stream_mono` does the same for the calibrated, beamformed, high-passed mono
  stream (`pcm_s16_le`, 16 kHz). `libreecho-waked` keeps one open. Its OK
  header is sent only once the capture has delivered audio; if the codec
  cannot be opened, the client gets `microphone capture unavailable` instead
  of an OK header followed by an empty stream.
- `beams` returns a snapshot of the beamformer while a `stream_mono` is
  running (see below), or `running:false` when none is.
- `stream_logical` fails closed: no consumer needs per-microphone logical
  streams yet.

Only one capture stream is admitted at a time.

The baby-monitor HTTP endpoint is a consumer of `micd`; it does not launch or
own `tinycap`. When `libreecho-waked` is running, the endpoint subscribes to its
shared, already-captured mono stream instead of opening a second ALSA capture.
The raw `pcm_s24_3le` lane path remains available only when the wake service is
absent.

## Geometry

Measured on a Radar by scratching each port in turn (each port lit exactly one
lane, 23-30 dB above the next loudest). Angles are clockwise viewed from above,
with 0 degrees at the front (the volume-down button):

```text
lane 0 rear-left    150      lane 3 front-right  330
lane 1 rear-right   210      lane 4 front-left    30
lane 2 action       270      lane 5 mute          90
lane 6 centre
```

Lanes 0-5 run round the ring in order (lane n at 150 + 60n degrees); opposite
pairs are 0/3, 1/4 and 2/5. A chirp from the device's own speaker reached all
six ring lanes within +/-0.4 samples, so the capture path adds no lane-to-lane
offset: any inter-lane delay in a recording is acoustic. The source of truth is
`src/adapter/mic_array_geometry.h`.

## Calibration

The seven IDME values (`/proc/idme/miccal.N`) are Q14 gains with `16384` as
unity, one per ALSA lane: the stock `libasp.so` applies `miccal.N` to lane N.
`micd` applies them the same way (direct Q14 multiply) before beamforming.

## Processing

```text
9 raw ALSA lanes
  -> 7 microphone lanes (top 16 bits of each word)
  -> per-lane miccal gain
  -> fixed-beam beamformer (lanes 0, 1, 3, 4 by default)
  -> 80 Hz high-pass
  -> mono S16_LE at 16 kHz
  -> consumers: wake word, baby monitor, voice assistant
```

The beamformer (`src/adapter/mic_beamformer.c`) follows the shape of the stock
Radar speech front end: a 256-point STFT with a 128-sample (8 ms) hop and 12
fixed beams spaced 30 degrees apart. The beams are superdirective (minimum
variance against diffuse noise) and designed at start-up from the geometry,
with per-frequency diagonal loading that keeps white-noise gain at or above
-3 dB. Each beam passes a talker on its axis at unity gain. Output is delayed
by 16 ms.

Beam selection scores each beam by its speech-band (312 Hz-4 kHz) power over
its own running noise floor. That floor falls at once and rises 3 dB per
second, so steady sources such as fans or music raise their beam's floor along
with its power and do not take the beam. A rival must be 3 dB above its floor
and beat the selected beam by 1.5 dB for 96 ms to take over. Between
utterances the selection stays on the last talker.

`LE_MIC_BEAM_MICS` (for example `0,1,2,3,4,5`) selects a different microphone
subset for experiments; the default is the stock speech front end's four.

The earlier fixed 4-sample delay-and-sum on lanes 0 and 3 was removed in
0.14.4: the lanes arrive in step, so a fixed delay suited one source direction
only and comb-filtered speech from every other direction.

## Mix mode

The owner picks how the speech microphones become the mono stream with
`microphone_mix` on `PUT /api/v1/audio` (see `docs/API.md`):

- `beamformer` (default): the fixed-beam output described above.
- `average`: the mean of the lanes in the speech set, rounded to nearest.
- `single`: one array microphone. `microphone_lane` picks lane 0-6 (array
  microphone 1-7); `-1`, the default, uses the lowest-numbered lane in the
  speech set (lane 0 by default). The lane is set live with `set_lane` (`{"lane":N}`),
  reported as `pipeline.microphone_lane`, and saved in the web configuration.
  It is only used in `single` mode.

The beamformer runs in every mode, so its selection and the beam view stay
current, and switching modes does not restart the stream. The mode is held in
the shared slot that the stream worker reads each frame. `set_mix` changes it
live, and `status` reports it as `pipeline.microphone_mix`. At start-up micd
reads the saved value from the web configuration named by `LE_MIC_CONFIG`.
Average and single output are not delayed by the 16 ms beamformer latency, so
a switch causes a one-time step of about 16 ms in the stream.

### External microphones

Only the on-board array is selectable. A USB audio input is not possible on
the radar image: the running MT8163 kernel is built without
`CONFIG_SND_USB_AUDIO` (checked in `/proc/config.gz` on radar, 2026-11-01).
Bluetooth audio input is not part of this contract. Adding an external source
needs the USB audio driver in the kernel, and any new source must stay behind
micd, the sole capture owner.

The dot has no seven-microphone array: its capture is a single TLV320AIC3101
codec, and the array stream returns no samples there, so `single` with a lane
choice is radar-only.

## Beam view and direction of arrival

`GET /api/v1/microphones/beams` (see `docs/API.md`) exposes the `beams`
snapshot: per-beam power and power over the noise floor, a rough direction of
arrival, how directional the sound field is, and the selected beam. The stream
worker publishes it every hop through a seqlocked shared mapping; no audio
leaves `micd` through this path. The web UI draws it as the *Direction of
arrival* preview on **Audio > Microphones**.

The direction is the loudest beam, refined by a parabola through its
neighbours. It shows where the most sound comes from, including steady noise,
while the selected beam follows talkers. The two can therefore differ.

## Listening ring direction cue

While the device is listening, `waked`, the local voice pipeline and the
Home Assistant/ESPHome turn indicator put `ledd` into the `direction`
pattern. The ring uses the configured Listening theme colour, and a brighter
segment points at `micd`'s selected talker beam. `waked` and the local
pipeline sample the beam once when listening starts; the Home Assistant
indicator re-reads the snapshot every 300 ms while the turn is capturing, so
the segment follows a talker who moves. No path starts capture or adds an
audio thread for this.

The cue only points when the `beams` snapshot reports `running` and its
`contrast_db` (the peak beam above the average beam) is at least 2 dB. With
a diffuse field, or when `micd` is unavailable, the ring stays solid in the
Listening colour rather than pointing at noise. Red remains reserved for
mute and privacy.

The 12 pixels are mapped as `(45 + 30*i) % 360` degrees clockwise from
front: pixel 11 is 15 degrees clockwise of front, pixel 10 is 15 degrees
anticlockwise, and the two are the measured front-centre pair used by the
idle indicator. If a hardware check shows the LED chain runs anticlockwise,
the 30-degree step is negated in `ledd.c`.

## Safety and privacy properties

- No capture starts merely because the device boots.
- Only one capture session is admitted at a time, avoiding ALSA ownership
  races.
- Status and beam inspection never start capture, and neither carries audio.
- Raw capture is not retained by `micd`.
- The UI/API authentication and privacy policy remain in front of browser
  streaming and the beam view.

## Clap detection (experimental, off by default)

`libreecho-waked` can screen its cleaned mono frames for claps. The detector
(`src/adapter/clap_detector.c`) is off at start-up and is switched on only by
the adapter control socket:

```text
{"v":1,"id":1,"cmd":"set_clap","args":{"enabled":true}}
```

`status` reports `clap_enabled` and `clap_count`. While enabled, a clap is a
10 ms frame whose energy jumps well above a slowly tracked noise floor, whose
peak is impulsive, whose spectrum is broadband (spectral flatness over eight
Goertzel bands) and whose energy falls back by about 6 dB within 40 ms. Frames
that fail the energy test never reach the spectral check, so silence and steady
sound cost almost nothing.

Results go to the wake subscriber stream as `clap_detected` with
`{"detection_sample":N,"claps":1}` for a single clap or `"claps":2` for two
claps 150-800 ms apart. A single is reported once the double window has passed
without a second clap. Existing subscribers select on `wake_detected`, so a
clap does not start a voice turn. The setting is not persisted, not exposed in
the HTTP API and not forwarded to Home Assistant yet.

## Smoke alarm detection (experimental, off by default)

`libreecho-waked` can also screen its cleaned mono frames for the T3 temporal
pattern of a smoke alarm: three pulses of a steady tone near 3 kHz, each about
0.5 s long, separated by gaps of about 0.5 s. The detector
(`src/adapter/smoke_alarm_detector.c`) is off at start-up and is switched on
only by the adapter control socket:

```text
{"v":1,"id":1,"cmd":"set_smoke_alarm","args":{"enabled":true}}
```

`status` reports `smoke_alarm_enabled` and `smoke_alarm_count`. While enabled,
each 10 ms frame must pass an energy gate. Frames that pass it are checked for
tonality: one of three Goertzel filters (2.9, 3.1, 3.3 kHz) must hold most of
the frame energy. A run of tonal frames of 0.3-0.9 s, followed by a gap of
0.3-0.9 s and then the next run, counts as a pulse. Three pulses in sequence
report one event, and a further 10 s cooldown applies. Noise, speech, music at
other frequencies and pulses with the wrong length or gap break the sequence.
The detector costs three 160-sample Goertzel passes per frame that passes the
energy gate.

Results go to the wake subscriber stream as `smoke_alarm_detected` with
`{"detection_sample":N,"pulses":3}`. Like claps, it never starts a voice turn.
The timing windows, the tonal threshold and the energy gate are tuned
constants, not a measured standard: they must be checked against the alarm
standard and a real alarm before any conformance claim. Only the T3 pattern is
covered; the CO (T4) pattern has short pulses that do not fit these windows.
The setting is not persisted, not exposed in the HTTP API and not forwarded to
Home Assistant yet.
