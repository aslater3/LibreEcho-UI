# Real Home Assistant ESPHome acceptance gate

## Acceptance checkpoint: PASS

Executed 2026-09-30 14:53 UTC against real Home Assistant **2026.9.4**, Python
**3.14.7**, aioesphomeapi **46.2.0**, host mbedTLS **3.6.4**. The fixture compiled
the current C source privately and exited **0**. Private receipts bind the
source/binary hashes to registration, encrypted reconnect, real HA pipeline and
HTTP audio handling, exact transported PCM, two continued turns and final idle.
Generated receipts remain outside the source tree. This is host integration
acceptance, not image, mDNS, acoustic, ARM or hardware acceptance.

### Observed passing phases

* Real HA ESPHome discovery-confirm/encryption-key config flow creates a loaded
  config entry with native API **1.14**. The supplied typed discovery envelope is
  loopback-only; this is **not mDNS discovery evidence**.
* HA registers available `media_player.fixture_satellite_media_player` (idle),
  `switch.fixture_satellite_microphone_mute` (off), and
  `assist_satellite.fixture_satellite_assist_satellite` (idle), with a MAC-connected
  device registry record.
* The real HA services control actual private daemon adapter transactions:
  microphone mute on/off with HA acknowledged state, volume 42, and radio pause.
* Zero-key encrypted provisioning persists the new fixture key. Once configured,
  plaintext and the wrong Noise key are refused. The zero/test keys are fixture
  material, never production credentials.
* Restarting only the private C daemon causes HA to observe disconnected
  availability and automatically reconnect using the persisted key, preserving
  entity IDs. HA config-entry reload is **not** claimed: that alternative exposed
  HA's double-unload of assist_satellite and was replaced with genuine daemon
  restart/reconnect, not mocked entity lifecycle.
* The actual registered satellite announcement implementation waits for native
  completion and delivers 9,600 bytes to the private PCM FIFO.
* Actual satellite start-conversation invokes HA's actual PipelineRun. Scripted
  STT receives **1,280 bytes** of real transported native microphone PCM and
  returns `fixture transcript`; the actual conversation dispatcher invokes the
  scripted agent, which returns `fixture response` and requests continuation;
  real HA TTS pipeline and HTTP handlers produce a private TTS URL. Observed
  events: run-start, stt-start/end, intent-start/end, tts-start/end, run-end.

### Historical compatibility failures, now resolved

The initial 2026-09-30 13:38 UTC checkpoint exited 1 for these actual defects:

1. `src/adapter/esphomed.c`, `states_send`, emitted state **4/ANNOUNCING** while
   playing. HA 2026.9.4's real ESPHome media player mapper has no ANNOUNCING entry;
   it raises `KeyError: <MediaPlayerState.ANNOUNCING: 4>`. Use a compatible mapped
   state for API 1.14 and cover it with the core worker's protocol test.
2. `src/adapter/esphome_playback.c`, `prepare`, required exact RIFF/data lengths.
   Actual HA ffmpeg-backed TTS output is a finite **4,878-byte**, HTTP 200 chunked
   WAV with **0xffffffff** RIFF and data sizes (nonseekable ffmpeg output).
   The daemon rejects it as `playback_error`. Normalize/bound sentinel lengths
   against the fully cached finite body without weakening malformed/truncated
   WAV rejection. The acceptance fixture deliberately uses HA's real conversion
   and HTTP path; it does not repair the payload or inject a fabricated response.

Both defects are resolved in the combined source: actual playback emits mapped
PLAYING=2, and completed bounded ffmpeg WAV bodies accept only the exact size
sentinels while malformed/truncated/oversized inputs remain rejected.
Native regressions also gate continuation on both the previous RUN_END and real
playback drain, acknowledging drain exactly once.

The passing HA run observed two conversation turns with the same conversation
ID, exact input PCM of **1,280 bytes per turn**, **38,400 FIFO TTS PCM bytes**,
a **9,600-byte announcement**, automatic continuation and final idle. No
provider output or HA-served WAV header was rewritten to make acceptance pass.

## Run / normal CI wiring

Prepare an isolated Python 3.14 environment with:

```sh
/path/to/ha-venv/bin/python -m pip install -r tests/test_ha_esphome_requirements.txt
```

All dependency pins in that file were compared with installed distributions;
that comparison exited 0. Requirements are acceptance-test-only, not dependencies
for the C daemon or ARM deployment. The fixture itself never installs packages.
Provide Linux unprivileged user/PID/network namespaces, `bwrap`, a C compiler,
mbedTLS static libraries/headers, and `ffmpeg` (HA performs real audio conversion).
No root, host network fallback, container, physical audio/Bluetooth/USB access or
running HA installation is required or allowed. Missing prerequisites fail.

Generic runner invocation:

```sh
ESPHOME_HA_PYTHON=/path/to/ha-venv/bin/python \
ESPHOME_TLS_PREFIX=/path/to/mbedtls \
python3 tests/test_ha_esphome_integration.py
```

`ESPHOMED_BIN=/path/to/binary` is supported; unset it to compile current source.
Capture exact commands, source hashes and observed stdout in private verification
evidence, not checked-in run manifests. The distinct `make test-ha-esphome`
target executes this fixture; normal source CI provisions the pinned Python
3.14.7 HA environment and runs that target in addition to native-client tests.
The aggregate runner checks gate wiring without claiming that the contract
check itself executes HA. Missing dependencies/isolation and a nonzero fixture
exit fail the real-HA gate.

## Exact isolation and scripted boundaries

Bubblewrap creates private user/PID/network namespaces, a read-only host tree,
private `/dev` and `/proc`, empty `/sys` and `/run`, and a single writable scratch
fixture directory under TMPDIR (fallback ~/.hermes/cache/scratch). HA's config,
private Unix adapters, listening sockets, native daemon and PCM FIFO all live
there. Numeric 127.0.0.1 endpoints exist only inside that network namespace.
Bluetooth setup/removal and USB discovery are disabled; zeroconf is constrained
to private unicast loopback. No LAN discovery result is claimed.

Compute providers are explicitly scripted STT, conversation, TTS. Audio/wake,
radio, timer and LED adapters are deterministic private sockets reused from
`test_esphomed.Fixture`. Acoustic VAD/model quality, a production provider,
Wi-Fi/mDNS behavior, ARM CPU/RAM/audio, hardware privacy and live HA deployment
are not tested. HA config flow/entry/entity/service/pipeline/HTTP handlers,
ESPHome callbacks, encrypted API client and the C daemon are not faked.

The observed final run printed CLEANUP after private namespace exit and verified
its fixture directory was removed. No remote/hardware actions or services were
left behind. Generated evidence is a historical checkpoint, not freshness proof
for later source changes.
