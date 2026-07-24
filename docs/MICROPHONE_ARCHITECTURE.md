# LibreEcho microphone architecture

## Current contract

`libreecho-micd` is the sole owner of microphone capture. It starts at boot but
does not open ALSA until a client explicitly requests a raw stream.

The confirmed hardware endpoint is:

```text
ALSA card 0, device 24
9 channels
S24_3LE
16 kHz
```

Hardware testing established that raw lanes 0 through 6 carry the seven
physical microphones. Lanes 7 and 8 are inactive or reserved transport lanes
and are not offered to consumers. The stock Radar `audio_device.xml` profile
must be applied before capture: all four TLV320AIC3101 ADCs use their DIF1
left/right inputs, 20 dB analog PGA gain, and FPGA timestamps disabled.

The daemon exposes `/run/libreecho/mic.sock` using the normal LibreEcho JSON
line protocol:

- `status` returns capture availability, activity, raw format, and per-device
  `miccal.0` through `miccal.6` metadata.
- `stream_raw` returns one JSON response line and then switches that connection
  to the interleaved raw PCM byte stream.
- `stream_logical` currently fails closed because the raw-to-logical channel
  mapping has not been established.

The baby-monitor HTTP endpoint is a consumer of `micd`; it does not launch or
own `tinycap`.

## Calibration boundary

The seven IDME values are represented as Q14-like gains with `16384` as unity.
They belong to seven logical microphones. Testing now proves which seven ALSA
lanes contain microphone signals, but does not yet prove their physical
geometry order. Until that order is independently established, the daemon must
report but not apply these values.

```text
9 raw ALSA lanes
  -> 7 active microphone lanes + 2 inactive/reserved lanes
  -> unresolved logical geometry ordering
  -> 7 logical microphones
  -> per-device miccal gain
  -> consumer-specific processing
```

This prevents a guessed lane map from corrupting capture, beamforming, or
future wake-word input.

## Planned processing stages

The stable service boundary is intended to grow behind the same socket:

```text
capture owner
  -> raw lane interpreter
  -> calibrated logical microphones
  -> optional AEC using the central playback reference
  -> optional beamforming / beam selection
  -> mono S16_LE at 16 kHz
  -> consumers
       - baby monitor
       - diagnostics / calibration tools
       - recording
       - future wake-word service
```

Wake-word inference is deliberately outside `micd`. It should consume a
post-AEC, post-beamformed mono stream when those stages are proven.

## Safety and privacy properties

- No capture starts merely because the device boots.
- Only one raw capture session is admitted at a time in the initial
  implementation, avoiding ALSA ownership races.
- Status inspection never starts capture.
- Raw capture is not retained by `micd`.
- The UI/API authentication and privacy policy remain in front of browser
  streaming.
- Logical/calibrated output remains unavailable until its channel contract is
  verified.
