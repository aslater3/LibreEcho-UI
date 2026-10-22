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
  stream (`pcm_s16_le`, 16 kHz). `libreecho-waked` keeps one open.
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

## Safety and privacy properties

- No capture starts merely because the device boots.
- Only one capture session is admitted at a time, avoiding ALSA ownership
  races.
- Status and beam inspection never start capture, and neither carries audio.
- Raw capture is not retained by `micd`.
- The UI/API authentication and privacy policy remain in front of browser
  streaming and the beam view.
