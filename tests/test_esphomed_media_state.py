"""Pinned native-client announcement states: PLAYING until actual bus drain, IDLE after."""
import asyncio
import contextlib
import io
import os
import struct
import time
import unittest
import wave
from test_esphomed_aio import APIClient
from test_esphomed import Fixture
from aioesphomeapi.model import MediaPlayerEntityState, MediaPlayerState


class MediaStateFixture(Fixture):
    def test_native_announcement_playing_then_idle_after_pcm_drain(self):
        self.s.close()
        wav = io.BytesIO()
        pcm = struct.pack("<h", 1900) * 12000
        with wave.open(wav, "wb") as writer:
            writer.setnchannels(1)
            writer.setsampwidth(2)
            writer.setframerate(48000)
            writer.writeframes(pcm)
        url = self.http_audio(wav.getvalue())

        async def run():
            client = APIClient("127.0.0.1", self.port, provide_time=False)
            states = []
            try:
                await client.connect(login=True)
                await client.list_entities_services()
                async def started(*args): return 0
                async def stopped(*args): return None
                async def captured(*args): return None
                client.subscribe_voice_assistant(handle_start=started, handle_stop=stopped, handle_audio=captured)
                client.subscribe_states(lambda state: states.append(state) if isinstance(state, MediaPlayerEntityState) else None)
                await asyncio.sleep(.3)
                states.clear()
                announcement = asyncio.create_task(client.send_voice_assistant_announcement_await_response(url, 4))
                deadline = time.monotonic() + 2
                while not states and time.monotonic() < deadline:
                    await asyncio.sleep(.01)
                self.assertTrue(states, "no actual announcement media state")
                self.assertEqual(states[0].state, MediaPlayerState.PLAYING,
                                 "HA 2026.9.4 cannot map ANNOUNCING; actual playback must be PLAYING")
                await asyncio.sleep(.1)
                self.assertFalse(announcement.done(), "completion before blocked PCM bus was drained")
                deadline = time.monotonic() + 3
                while not announcement.done() and time.monotonic() < deadline:
                    with contextlib.suppress(BlockingIOError):
                        self.busbytes += os.read(self.busfd, 65536)
                    await asyncio.sleep(.01)
                self.assertTrue((await announcement).success)
                with contextlib.suppress(BlockingIOError):
                    self.busbytes += os.read(self.busfd, 65536)
                self.assertEqual(self.busbytes, pcm * 2)
                await asyncio.sleep(.05)
                self.assertEqual(states[-1].state, MediaPlayerState.IDLE)
                self.assertNotIn(MediaPlayerState.ANNOUNCING, [state.state for state in states])
            finally:
                await client.disconnect(force=True)

        asyncio.run(asyncio.wait_for(run(), 8))


if __name__ == "__main__":
    unittest.main(defaultTest="MediaStateFixture.test_native_announcement_playing_then_idle_after_pcm_drain")
