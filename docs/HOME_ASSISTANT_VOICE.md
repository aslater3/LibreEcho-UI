# Home Assistant voice satellite mode

LibreEcho's Home Assistant mode is a satellite mode. It keeps the device-side
audio front end and wake word detector, while Home Assistant runs the voice
pipeline after wake:

```text
Echo microphones
  -> calibration / AEC / beam selection
  -> local openWakeWord (Alexa development model)
  -> Wyoming satellite connection
  -> Home Assistant Assist pipeline
       -> remote or Home Assistant STT (Whisper, Speech-to-Phrase, etc.)
       -> Home Assistant conversation agent and intent handling
       -> remote or Home Assistant TTS (Piper, etc.)
  -> Wyoming audio output
  -> local audiod speaker bus
```

The local STT daemon, local ChatGPT provider, and local TTS daemon are not
needed for this mode. They remain available as an explicit fallback mode; the
Home Assistant mode must not start them or send microphone audio to both
pipelines.

## Protocol choice

Home Assistant's Wyoming integration accepts external voice services and
supports Whisper, Speech-to-Phrase, Piper, and openWakeWord. Wyoming is a
small TCP protocol consisting of newline-delimited JSON event headers followed
by optional JSON data and binary payloads. The satellite exposes a TCP service
that Home Assistant discovers/configures as a Wyoming Protocol device.

The newer Open Home Foundation Linux Voice Assistant uses the ESPHome native
API and has useful newer satellite features, but its published requirements
are Linux x64/ARM64, Python 3.11+, and at least 512 MB RAM. That is not a
good runtime target for this 32-bit ARM board with approximately 491 MB total
RAM. Wyoming has the smaller C-native implementation boundary needed here.

## LibreEcho Wyoming bridge contract

The bridge will listen on a configurable TCP port, default `10700`, and
provide the satellite portion of Wyoming:

- `describe` -> `info` with the local satellite, mic, and speaker formats;
- `satellite-connected` lifecycle event;
- local wake detections -> `detection`; Home Assistant then sends
  `run-pipeline` beginning at `asr`;
- indexed post-AEC mono PCM -> `audio-start`, `audio-chunk`, `audio-stop`;
- incoming `audio-start`/`audio-chunk`/`audio-stop` -> the local 48 kHz stereo
  speaker bus, with bounded format conversion;
- `run-satellite`, `pause-satellite`, and `run-pipeline` control events;
- local-socket reconnect and one-connection-at-a-time behavior with bounded
  buffers.

The bridge must not expose raw microphone lanes. Only final post-AEC mono
16-bit PCM at 16 kHz is sent upstream. The wake daemon remains the owner of
capture, calibration, AEC, VAD, beam selection, and wakeword inference.

## Configuration and modes

The existing `home-assistant` integration toggle is persisted as integration
bit 1. When enabled, the device listens for Home Assistant on TCP port 10700;
Home Assistant's Wyoming integration connects to the device IP and port. The
current bridge advertises the fixed local wake word `Alexa` and the device
name/area `LibreEcho`; the port can be overridden in the init environment.

Selecting Home Assistant mode immediately stops local STT, local assistant
dispatch, and local TTS, and starts the Wyoming bridge. On reboot the same
choice is applied by init. It does not disable `waked`, `micd`, `audiod`, or
AEC. The existing local ChatGPT mode remains separate and mutually exclusive.

## Compatibility references

- Home Assistant Wyoming integration: https://www.home-assistant.io/integrations/wyoming
- Wyoming protocol: https://github.com/OHF-Voice/wyoming
- Wyoming satellite reference: https://github.com/rhasspy/wyoming-satellite
- Open Home Foundation Linux Voice Assistant: https://github.com/OHF-Voice/linux-voice-assistant
