"""Real Home Assistant / ESPHome host acceptance, never a live-HA/hardware test.

Run with an already installed Home Assistant==2026.9.4 environment:
  ESPHOME_HA_PYTHON=/path/to/ha-venv/bin/python \
  ESPHOME_TLS_PREFIX=/path/to/mbedtls python tests/test_ha_esphome_integration.py
Optional ESPHOMED_BIN uses an existing compiled daemon; otherwise compile current
sources privately at fixture time. Missing dependencies/isolation are failures,
not skips. No dependency installer, remote address, mDNS or physical device.

The outer process enforces a private network/PID namespace with bubblewrap,
read-only host files, private /dev,/proc,/sys,/run and a writable scratch tree.
Only STT, conversation, TTS compute and audio/wake/radio/timer/LED adapters are
scripted. HA's config flow, config entry manager, entity platforms, services,
Assist pipeline, ESPHome callbacks, Noise client and C daemon are real. Ambient
Bluetooth/USB discovery and zeroconf are disabled explicitly (numeric loopback).
This establishes host protocol/control/audio transport, not acoustic quality,
production HA discovery, real provider performance or ARM/hardware acceptance.
"""
from __future__ import annotations

import asyncio
import base64
import contextlib
import importlib.metadata
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave
from unittest.mock import AsyncMock, patch

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {"homeassistant": "2026.9.4", "aioesphomeapi": "46.2.0"}


def isolated_main() -> None:
    python = os.environ.get("ESPHOME_HA_PYTHON", sys.executable)
    if Path(python).absolute() != Path(sys.executable).absolute():
        os.execve(python, [python, str(Path(__file__).resolve()), *sys.argv[1:]],
                  dict(os.environ, ESPHOME_HA_PYTHON=python))
    if os.environ.get("ESPHOME_HA_ISOLATED") != "1":
        bwrap = shutil.which("bwrap")
        if not bwrap:
            raise SystemExit("FAIL: bubblewrap is required; no host-network fallback")
        scratch = Path(os.environ.get("TMPDIR", str(Path.home() / ".hermes/cache/scratch"))).resolve()
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ha-gate-", dir=scratch) as directory:
            env = dict(os.environ, ESPHOME_HA_ISOLATED="1", ESPHOME_HA_ROOT=directory,
                       ESPHOME_HA_PARENT_NET=os.readlink("/proc/self/ns/net"),
                       TMPDIR=directory, PYTHONDONTWRITEBYTECODE="1")
            argv = [bwrap, "--unshare-user", "--unshare-pid", "--unshare-net",
                    "--die-with-parent", "--ro-bind", "/", "/", "--bind", directory,
                    directory, "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/sys",
                    "--tmpfs", "/run", "--", sys.executable, str(Path(__file__).resolve())]
            result = subprocess.run(argv, env=env, timeout=180)
        assert not Path(directory).exists(), "private fixture files not cleaned"
        print("CLEANUP: private namespace exited; fixture directory removed; no services retained", flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
        print("PASS: real Home Assistant ESPHome integration acceptance", flush=True)
        return
    assert os.readlink("/proc/self/ns/net") != os.environ["ESPHOME_HA_PARENT_NET"]
    assert not Path("/dev/snd").exists() and not Path("/run/dbus").exists()
    assert not list(Path("/sys").iterdir())
    for package, expected in EXPECTED.items():
        observed = importlib.metadata.version(package)
        assert observed == expected, f"{package}: expected {expected}, observed {observed}"
    print("VERSIONS " + json.dumps(dict(EXPECTED, python=sys.version.split()[0])), flush=True)
    binary = os.environ.get("ESPHOMED_BIN")
    if not binary:
        tls = Path(os.environ.get("ESPHOME_TLS_PREFIX", "/usr"))
        binary = str(Path(os.environ["ESPHOME_HA_ROOT"]) / "libreecho-esphomed")
        sources = ["src/adapter/esphomed.c", "src/adapter/esphome_proto.c",
                   "src/adapter/esphome_frame.c", "src/adapter/esphome_noise.c",
                   "src/adapter/esphome_playback.c", "src/adapter/radio_resample.c",
                   "src/adapter/mdns_client.c", "src/adapter/mdns_lease.c",
                   "src/config_store.c", "src/json.c"]
        command = [os.environ.get("CC", "cc"), "-std=c99", "-D_POSIX_C_SOURCE=200809L",
                   "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                   "-I" + str(tls / "include"), *sources,
                   *[str(tls / "lib" / ("lib" + name + ".a"))
                     for name in ("mbedtls", "mbedx509", "mbedcrypto")], "-lm", "-o", binary]
        subprocess.run(command, cwd=ROOT, check=True, timeout=60)
    print("BUILD " + json.dumps({"binary_sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
          "compiled_current_sources": not bool(os.environ.get("ESPHOMED_BIN")),
          "daemon_source_sha256": hashlib.sha256((ROOT / "src/adapter/esphomed.c").read_bytes()).hexdigest(),
          "playback_source_sha256": hashlib.sha256((ROOT / "src/adapter/esphome_playback.c").read_bytes()).hexdigest()}), flush=True)
    os.environ["ESPHOMED_BIN"] = binary
    asyncio.run(asyncio.wait_for(acceptance(), timeout=100))


async def wait_for(predicate, description, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        await asyncio.sleep(0.02)
    raise AssertionError("timed out: " + description)


async def acceptance() -> None:
    import logging
    logging.basicConfig(level=logging.WARNING)
    from aioesphomeapi import APIClient
    from aioesphomeapi.core import APIConnectionError
    from homeassistant import bootstrap, loader
    from homeassistant.core import HomeAssistant
    from homeassistant.components import conversation, stt, tts
    from homeassistant.components.assist_pipeline.pipeline import async_setup_pipeline_store
    from homeassistant.components.assist_satellite import AssistSatelliteAnnouncement
    from homeassistant.helpers import entity_registry as er, device_registry as dr
    from homeassistant.helpers.intent import IntentResponse
    from homeassistant.setup import async_setup_component
    from test_esphomed import Fixture

    fixture = Fixture(methodName="runTest")
    await asyncio.to_thread(fixture.setUp)
    fixture.s.close()
    await asyncio.sleep(.05)
    config_dir = fixture.p / "ha-config"
    config_dir.mkdir()
    hass = HomeAssistant(str(config_dir))
    loader.async_setup(hass)
    # Do not auto-install requirements or inspect host radios/USB/network discovery.
    hass.config.skip_pip = True
    key = base64.b64encode(bytes(range(32))).decode()
    observations: dict = {"boundary": "real HA host integration; scripted compute and local adapters",
        "discovery_boundary": "typed loopback discovery envelope supplied; no mDNS discovery test",
        "providers_boundary": "scripted STT/conversation/TTS; no acoustic VAD or provider quality test"}
    ha_errors = []
    class HAErrorCollector(logging.Handler):
        def emit(self, record):
            if record.levelno >= logging.ERROR and record.name.startswith("homeassistant"):
                ha_errors.append(record.getMessage())
    error_handler = HAErrorCollector()
    logging.getLogger().addHandler(error_handler)
    patches = contextlib.ExitStack()
    patches.enter_context(patch("homeassistant.components.bluetooth.async_setup", AsyncMock(return_value=True)))
    patches.enter_context(patch("homeassistant.components.bluetooth.async_remove_scanner", return_value=None))
    patches.enter_context(patch("homeassistant.components.usb.USBDiscovery.async_setup", AsyncMock(return_value=None)))
    patches.enter_context(patch("homeassistant.components.zeroconf._async_get_zc_args",
                                return_value={"interfaces": ["127.0.0.1"], "unicast": True}))
    try:
        provision = APIClient("127.0.0.1", fixture.port,
                              noise_psk=base64.b64encode(bytes(32)).decode(), provide_time=False)
        try:
            await provision.connect(login=True)
            assert (await provision.device_info()).api_encryption_provisionable
            assert await provision.noise_encryption_set_key(bytes(range(32)))
        finally:
            await provision.disconnect(force=True)
        await asyncio.sleep(.1)
        assert json.loads(fixture.config.read_text())["esphome_noise_key"] == key
        for invalid in (None, base64.b64encode(bytes(32)).decode()):
            client = APIClient("127.0.0.1", fixture.port, noise_psk=invalid, provide_time=False)
            try:
                try:
                    await client.connect(login=True, log_errors=False)
                except APIConnectionError:
                    pass
                else:
                    raise AssertionError("configured Noise key accepted plaintext/wrong key")
            finally:
                await client.disconnect(force=True)
            await asyncio.sleep(.05)
        observations["security"] = {"provisioned_and_persisted": True, "plaintext_refused": True,
                                     "wrong_key_refused": True}
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        http_port = probe.getsockname()[1]
        probe.close()
        config = {"homeassistant": {"name": "Private acceptance", "latitude": 0,
                                   "longitude": 0, "elevation": 0, "unit_system": "metric",
                                   "time_zone": "UTC", "internal_url": f"http://127.0.0.1:{http_port}"},
                  "http": {"server_host": "127.0.0.1", "server_port": http_port},
                  "media_source": {}, "assist_satellite": {}}
        from homeassistant.config_entries import ConfigEntries
        from homeassistant.core_config import async_process_ha_core_config
        hass.config_entries = ConfigEntries(hass, config)
        assert await bootstrap.async_load_base_functionality(hass)
        assert await async_setup_component(hass, "homeassistant", config)
        assert await async_setup_component(hass, "persistent_notification", config)
        await async_process_ha_core_config(hass, config["homeassistant"])
        for domain in ("http", "esphome", "media_source", "assist_satellite"):
            assert await async_setup_component(hass, domain, config), domain
        await hass.async_start()

        class ScriptedSTT(stt.SpeechToTextEntity):
            _attr_name = "Scripted fixture STT"
            _attr_unique_id = "fixture-stt"
            supported_languages = ["en"]
            supported_formats = [stt.AudioFormats.WAV]
            supported_codecs = [stt.AudioCodecs.PCM]
            supported_bit_rates = [stt.AudioBitRates.BITRATE_16]
            supported_sample_rates = [stt.AudioSampleRates.SAMPLERATE_16000]
            supported_channels = [stt.AudioChannels.CHANNEL_MONO]

            @property
            def audio_processing(self):
                # Scripted provider terminates after a bounded PCM window;
                # acoustic VAD is deliberately not under test.
                return stt.SpeechAudioProcessing(False, False, False)

            def __init__(self):
                self.received = []

            async def async_process_audio_stream(self, metadata, stream):
                data = bytearray()
                async for chunk in stream:
                    data.extend(chunk)
                    if len(data) >= 1280:
                        break
                assert data, "STT received no actual daemon audio"
                self.received.append(bytes(data))
                return stt.SpeechResult("fixture transcript", stt.SpeechResultState.SUCCESS)

        class ScriptedTTS(tts.TextToSpeechEntity):
            _attr_name = "Scripted fixture TTS"
            _attr_unique_id = "fixture-tts"
            _attr_default_language = "en"
            _attr_supported_languages = ["en"]
            _attr_supported_options = [tts.ATTR_PREFERRED_FORMAT, tts.ATTR_PREFERRED_SAMPLE_RATE,
                                       tts.ATTR_PREFERRED_SAMPLE_CHANNELS, tts.ATTR_PREFERRED_SAMPLE_BYTES]

            def __init__(self):
                self.messages = []

            async def async_get_tts_audio(self, message, language, options):
                self.messages.append(message)
                assert language == "en"
                wav = io.BytesIO()
                with wave.open(wav, "wb") as output:
                    output.setnchannels(options.get(tts.ATTR_PREFERRED_SAMPLE_CHANNELS, 2))
                    output.setsampwidth(2)
                    output.setframerate(options.get(tts.ATTR_PREFERRED_SAMPLE_RATE, 48000))
                    output.writeframes(struct.pack("<h", 1200) * 2400 * output.getnchannels())
                return "wav", wav.getvalue()

        class ScriptedAgent(conversation.ConversationEntity):
            _attr_name = "Scripted fixture conversation"
            _attr_unique_id = "fixture-conversation"
            supported_languages = ["en"]

            def __init__(self):
                self.inputs = []

            async def async_process(self, user_input):
                self.inputs.append(user_input)
                assert user_input.text == "fixture transcript"
                response = IntentResponse(language=user_input.language)
                response.async_set_speech("fixture response")
                return conversation.ConversationResult(response, user_input.conversation_id,
                                                        continue_conversation=len(self.inputs) == 1)

        speech = ScriptedSTT()
        voice = ScriptedTTS()
        agent = ScriptedAgent()
        await hass.data[stt.DATA_COMPONENT].async_add_entities([speech])
        await hass.data[tts.DATA_COMPONENT].async_add_entities([voice])
        from homeassistant.components.conversation.const import DATA_COMPONENT as CONVERSATION_COMPONENT
        await hass.data[CONVERSATION_COMPONENT].async_add_entities([agent])
        pipeline_data = await async_setup_pipeline_store(hass)
        pipeline = await pipeline_data.pipeline_store.async_create_item({
            "name": "Fixture scripted compute", "language": "en",
            "conversation_engine": agent.entity_id, "conversation_language": "en",
            "stt_engine": speech.entity_id, "stt_language": "en", "tts_engine": voice.entity_id,
            "tts_language": "en", "tts_voice": None, "wake_word_entity": None, "wake_word_id": None})
        pipeline_data.pipeline_store.async_set_preferred_item(pipeline.id)

        # Supply a typed private discovery envelope; no mDNS scan/LAN traffic.
        # The real HA discovery-confirm/encryption-key flow validates the daemon.
        from ipaddress import IPv4Address
        from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
        discovery = ZeroconfServiceInfo(
            ip_address=IPv4Address("127.0.0.1"), ip_addresses=[IPv4Address("127.0.0.1")],
            port=fixture.port, hostname="fixture-satellite.local.", type="_esphomelib._tcp.local.",
            name="fixture-satellite._esphomelib._tcp.local.",
            properties={"mac": "020000000001", "api_encryption": "Noise_NNpsk0_25519_ChaChaPoly_SHA256"})
        result = await hass.config_entries.flow.async_init("esphome", context={"source": "zeroconf"},
                                                         data=discovery)
        assert result["type"] == "form" and result["step_id"] == "discovery_confirm", result
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
        assert result["type"] == "form" and result["step_id"] == "encryption_key", result
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {"noise_psk": key})
        assert result["type"] == "create_entry", result
        entry = result["result"]
        await wait_for(lambda: hasattr(entry, "runtime_data") and entry.runtime_data.available,
                       "ESPHome manager connected")
        await wait_for(lambda: len([s for s in hass.states.async_all()
                                   if s.entity_id.startswith(("media_player.", "switch.", "assist_satellite."))]) >= 3,
                       "HA entity platforms registered")
        registry = er.async_get(hass)
        entries = er.async_entries_for_config_entry(registry, entry.entry_id)
        entities = {}
        for domain in ("media_player", "switch", "assist_satellite"):
            matched = [e.entity_id for e in entries if e.domain == domain]
            assert len(matched) == 1, (domain, matched)
            entities[domain] = matched[0]
            await wait_for(lambda d=domain: hass.states.get(entities[d]).state not in ("unavailable", "unknown"),
                           "available " + domain)
        assert any(d for d in dr.async_entries_for_config_entry(dr.async_get(hass), entry.entry_id)
                   if (dr.CONNECTION_NETWORK_MAC, "02:00:00:00:00:01") in d.connections)
        assert entry.data["noise_psk"] == key
        assert entry.state.value == "loaded"
        assert entry.runtime_data.api_version.major == 1 and entry.runtime_data.api_version.minor == 14
        observations["registration"] = {"entry_state": entry.state.value, "entities": entities,
                                        "states": {d: hass.states.get(e).state for d, e in entities.items()},
                                        "native_api_version": "1.14"}
        print("REGISTRATION " + json.dumps(observations["registration"]), flush=True)

        async def service(domain, name, **data):
            await hass.services.async_call(domain, name, dict(entity_id=entities[domain], **data), blocking=True)

        await service("switch", "turn_on")
        await wait_for(lambda: fixture.adapters["audio"].muted, "HA mute command")
        await wait_for(lambda: hass.states.get(entities["switch"]).state == "on", "HA acknowledged mute state")
        await service("switch", "turn_off")
        await wait_for(lambda: not fixture.adapters["audio"].muted, "HA unmute command")
        await wait_for(lambda: hass.states.get(entities["switch"]).state == "off", "HA acknowledged unmute state")
        await service("media_player", "volume_set", volume_level=.42)
        await wait_for(lambda: any(r["cmd"] == "set_volume" and r["args"] == {"volume": 42}
                                  for r in fixture.adapters["audio"].calls), "HA volume command")
        await service("media_player", "media_pause")
        await wait_for(lambda: any(r["cmd"] == "pause" for r in fixture.adapters["radio"].calls), "HA pause")
        observations["controls"] = ["mute_on_state", "mute_off_state", "volume_42", "media_pause"]

        # Restart only the private fixture daemon using its saved argv/config;
        # HA's unmodified reconnect manager remains in control.
        before = sorted(e.entity_id for e in entries)
        await wait_for(lambda: "assist_satellite" in entry.runtime_data.loaded_platforms,
                       "satellite platform fully loaded before reconnect")
        await hass.async_block_till_done()
        daemon_argv = fixture.proc.args
        fixture.proc.terminate()
        _, daemon_stderr = await asyncio.to_thread(fixture.proc.communicate, timeout=3)
        assert fixture.proc.returncode in (0, -15), daemon_stderr.decode()
        await wait_for(lambda: not entry.runtime_data.available, "HA observed disconnect")
        fixture.proc = subprocess.Popen(daemon_argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        await wait_for(lambda: entry.runtime_data.available, "HA automatic reconnect")
        await wait_for(lambda: "assist_satellite" in entry.runtime_data.loaded_platforms,
                       "satellite platform fully loaded after reconnect")
        await hass.async_block_till_done()
        await wait_for(lambda: all(hass.states.get(e) and hass.states.get(e).state != "unavailable"
                                  for e in entities.values()), "entities available after reconnect")
        after = sorted(e.entity_id for e in er.async_entries_for_config_entry(registry, entry.entry_id))
        assert before == after
        assert entry.data["noise_psk"] == key
        observations["reconnect"] = {"private_daemon_restart": True, "ha_automatic_reconnect": True,
                                     "same_entity_ids": True, "configured_key_reused": True}

        # Read the actual entity created by HA's platform; no fabricated satellite.
        from homeassistant.components.assist_satellite import DATA_COMPONENT as SATELLITE_COMPONENT
        satellite = hass.data[SATELLITE_COMPONENT].get_entity(entities["assist_satellite"])
        assert satellite is not None and satellite.cli is entry.runtime_data.client
        pipeline_events = []
        original = satellite.on_pipeline_event
        def observe(event):
            pipeline_events.append(event)
            print("PIPELINE_EVENT " + json.dumps({"type": event.type.value, "data": event.data}, default=str), flush=True)
            original(event)
        satellite.on_pipeline_event = observe
        audio = fixture.adapters["wake"]
        await asyncio.to_thread(audio.wait_streams)
        pcm = bytearray()
        drain_stop = asyncio.Event()
        async def drain_bus():
            while not drain_stop.is_set():
                with contextlib.suppress(BlockingIOError):
                    pcm.extend(os.read(fixture.busfd, 65536))
                await asyncio.sleep(.005)
        drain = asyncio.create_task(drain_bus())
        try:
            # Satellite's real announcement implementation waits for native
            # completion; the scripted provider's URL remains private loopback.
            url = fixture.http_audio((await voice.async_get_tts_audio("fixture announcement", "en", {}))[1])
            offset = len(pcm)
            await asyncio.wait_for(satellite.async_announce(AssistSatelliteAnnouncement(
                media_id=url, original_media_id=url, tts_token=None,
                media_id_source="tts", message="fixture announcement")), 8)
            await wait_for(lambda: len(pcm) > offset, "announcement FIFO audio")
            observations["announcement"] = {"native_completion_awaited": True,
                                            "fifo_pcm_bytes": len(pcm) - offset}

            # HA API-start-conversation -> daemon request -> HA satellite -> real
            # PipelineRun -> scripted STT/agent/TTS -> HA's HTTP TTS URL -> FIFO.
            for i in range(0, 6400, 320):
                audio.samples(i)
            await asyncio.sleep(.1)
            kickoff_url = fixture.http_audio((await voice.async_get_tts_audio("fixture kickoff", "en", {}))[1])
            await asyncio.wait_for(satellite.async_start_conversation(AssistSatelliteAnnouncement(
                media_id=kickoff_url, original_media_id=kickoff_url, tts_token=None,
                media_id_source="tts", message="fixture kickoff")), 8)
            await wait_for(lambda: satellite._pipeline_task is not None, "HA announcement started HA pipeline")
            for i in range(6400, 32000, 320):
                audio.samples(i)
                await asyncio.sleep(.003)
                if len(agent.inputs) >= 2:
                    break
            end = time.monotonic() + 20
            while len(agent.inputs) < 2 and time.monotonic() < end:
                status = json.loads((fixture.p / "status.json").read_text())
                if status.get("last_result") in ("playback_error", "ha_error", "capture_overrun", "stream_gap"):
                    observations["voice_failure"] = {"daemon_status": status,
                        "events": [{"type": e.type.value, "data": e.data} for e in pipeline_events],
                        "stt_bytes": [len(b) for b in speech.received], "agent_turns": len(agent.inputs),
                        "tts_messages": voice.messages, "fifo_pcm_bytes": len(pcm), "ha_errors": ha_errors}
                    from homeassistant.helpers.aiohttp_client import async_get_clientsession
                    tts_urls = [e.data["tts_output"]["url"] for e in pipeline_events
                                if e.type.value == "tts-end"]
                    if tts_urls:
                        async with async_get_clientsession(hass).get(
                                f"http://127.0.0.1:{http_port}" + tts_urls[-1]) as response:
                            body = await response.read()
                            observations["voice_failure"]["ha_tts_http"] = {
                                "status": response.status, "headers": dict(response.headers),
                                "body_bytes": len(body), "wav_prefix_hex": body[:90].hex()}
                    print("EVIDENCE " + json.dumps(observations, default=str, sort_keys=True), flush=True)
                    raise AssertionError("real HA voice transport failed: " + str(status.get("last_result")))
                await asyncio.sleep(.02)
            assert len(agent.inputs) == 2, "automatic conversation continuation missing"
            await wait_for(lambda: hass.states.get(entities["assist_satellite"]).state == "idle"
                           and json.loads((fixture.p / "status.json").read_text()).get("last_result") == "success",
                           "HA playback completion returned idle", timeout=20)
            assert len(speech.received) == 2 and all(any(b) for b in speech.received)
            for received in speech.received:
                samples = struct.unpack("<" + "h" * (len(received) // 2), received)
                assert samples == tuple((samples[0] + i) % 30000 for i in range(len(samples))), (
                    "native audio transport changed the scripted PCM sequence")
            assert agent.inputs[0].conversation_id == agent.inputs[1].conversation_id
            assert all(i.satellite_id == satellite.entity_id for i in agent.inputs)
            assert voice.messages[2:] and all(m == "fixture response" for m in voice.messages[2:])
            required_events = {"run-start", "stt-start", "stt-end", "intent-start", "intent-end",
                               "tts-start", "tts-end", "run-end"}
            event_names = [e.type.value for e in pipeline_events]
            assert required_events <= set(event_names), event_names
            assert not any(e.type.value == "error" for e in pipeline_events)
            assert pcm and pcm == struct.pack("<h", 1200) * (len(pcm) // 2)
            observations["voice"] = {"pipeline_events": event_names, "stt_bytes": [len(b) for b in speech.received],
                                     "conversation_turns": len(agent.inputs), "same_conversation_id": True, "input_pcm_exact": True,
                                     "tts_messages": voice.messages, "fifo_pcm_bytes": len(pcm), "state": "idle"}

        finally:
            drain_stop.set()
            await drain

        print("EVIDENCE " + json.dumps(observations, sort_keys=True), flush=True)
        assert not ha_errors, "HA logged integration errors: " + repr(ha_errors)
    finally:
        try:
            await asyncio.wait_for(hass.async_stop(force=True), 15)
        finally:
            patches.close()
            logging.getLogger().removeHandler(error_handler)
            # HTTP server cleanups registered by Fixture.http_audio precede
            # daemon/adapters/FIFO deletion, even if an assertion failed.
            await asyncio.to_thread(fixture.doCleanups)
            await asyncio.to_thread(fixture.tearDown)


if __name__ == "__main__":
    isolated_main()
