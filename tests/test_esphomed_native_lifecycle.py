"""Real daemon/native client/HTTP/FIFO lifecycle regressions, no device paths.

After landing beside test_esphomed.py, run with ESPHOMED_BIN set to the daemon.
In a private lane set ESPHOMED_FIXTURE_TESTS to the read-only repository tests.
"""
import contextlib
import fcntl
import io
import json
import os
from pathlib import Path
import select
import socket
import struct
import sys
import termios
import time
import unittest
import wave

sys.dont_write_bytecode = True
sys.path.insert(0, os.environ.get('ESPHOMED_FIXTURE_TESTS', str(Path(__file__).parent)))
import test_esphomed as fixture


class NativeLifecycle(unittest.TestCase):
    # Reuse fixture plumbing, not its unrelated/inherited test methods.
    setUp = fixture.Fixture.setUp
    tearDown = fixture.Fixture.tearDown
    connect = fixture.Fixture.connect
    hello = fixture.Fixture.hello
    http_audio = fixture.Fixture.http_audio
    wait_completion = fixture.Fixture.wait_completion

    def owner(self):
        self.hello()
        self.s.sendall(fixture.frame(89, fixture.num(1, 1) + fixture.num(2, 4)))
        self.barrier()
        time.sleep(.3)  # private audio/physical-privacy fixture status warm-up

    def barrier(self, client=None):
        client = client or self.s
        client.sendall(fixture.frame(7))
        messages = []
        end = time.monotonic() + 2
        while time.monotonic() < end:
            message = fixture.receive(client)
            messages.append(message)
            if message[0] == 8:
                return messages[:-1]
        self.fail('native ping barrier did not return')

    def messages_for(self, duration=.2):
        messages = []
        end = time.monotonic() + duration
        while time.monotonic() < end:
            ready, _, _ = select.select([self.s], [], [], max(0, end-time.monotonic()))
            if ready:
                messages.append(fixture.receive(self.s))
        return messages

    def wav_url(self, sample=1400, count=1000):
        data = io.BytesIO()
        with wave.open(data, 'wb') as writer:
            writer.setnchannels(1)
            writer.setsampwidth(2)
            writer.setframerate(48000)
            writer.writeframes(struct.pack('<h', sample) * count)
        return self.http_audio(data.getvalue()), struct.pack('<h', sample) * (count*2)

    def unread_audio(self):
        end = time.monotonic() + 2
        while time.monotonic() < end:
            size = struct.unpack('i', fcntl.ioctl(self.busfd, termios.FIONREAD, struct.pack('i', 0)))[0]
            if size > 0:
                return size
            time.sleep(.01)
        self.fail('actual private FIFO never received playback bytes')

    def drain_audio(self, expected):
        end = time.monotonic() + 3
        while len(self.busbytes) < len(expected) and time.monotonic() < end:
            with contextlib.suppress(BlockingIOError):
                self.busbytes += os.read(self.busfd, 65536)
            time.sleep(.005)
        self.assertEqual(self.busbytes, expected, 'must drain actual decoded PCM, not emulate completion')
        return self.messages_for(.2)

    def start_voice(self):
        wake = self.adapters['wake']
        wake.wait_streams()
        wake.samples(0)
        time.sleep(.05)
        wake.wake(100)
        self.assertEqual(fixture.receive(self.s)[0], 90)
        self.s.sendall(fixture.frame(91))
        self.barrier()

    def event(self, kind, **data):
        body = fixture.num(1, kind)
        for key, value in data.items():
            body += fixture.text(2, fixture.text(1, key)+fixture.text(2, value))
        return fixture.frame(92, body)

    def tts_continuation(self, run_end_first=False):
        self.owner()
        self.start_voice()
        url, pcm = self.wav_url()
        request = self.event(6, conversation_id='prior-conversation', continue_conversation='1')
        request += self.event(8, url=url)
        if run_end_first:
            request += self.event(2)
        self.s.sendall(request)
        self.unread_audio()
        return pcm

    def unrelated_announcement(self):
        other = self.connect()
        other.sendall(fixture.frame(1))
        self.assertEqual(fixture.receive(other)[0], 2)
        body = b'\x0d'+struct.pack('<I', 1)+fixture.num(6, 1)
        body += fixture.text(7, 'http://127.0.0.1:1/unrelated.wav')
        body += fixture.num(8, 1)+fixture.num(9, 1)
        other.sendall(fixture.frame(65, body))
        self.barrier(other)

    def test_bounded_wake_reload_does_not_time_out_at_legacy_half_second(self):
        self.owner()
        self.start_voice()
        self.adapters["wake"].set_word_delay = .8
        self.s.sendall(fixture.frame(123, fixture.text(1, "alexa_v0.1")))
        self.assertTrue(any(kind == 122 for kind, _ in self.barrier()))
        time.sleep(1.0)
        status = json.loads((self.p / "status.json").read_text())
        self.assertTrue(status["in_progress"], "a valid bounded reload cancelled the active turn at the obsolete 500 ms deadline")
        self.assertNotEqual(status["last_result"], "adapter_error")

    def test_unrelated_client_cannot_complete_active_announcement(self):
        self.owner()
        url, pcm = self.wav_url()
        self.s.sendall(fixture.frame(119, fixture.text(1, url)))
        self.unread_audio()
        self.unrelated_announcement()
        messages = self.barrier()+self.messages_for(.15)
        self.assertFalse(any(kind == 120 for kind, _ in messages),
                         f'unrelated client prematurely completed owner announcement while FIFO unread: {messages}')
        self.assertTrue(json.loads((self.p/'status.json').read_text())['in_progress'])
        self.wait_completion()
        self.assertEqual(self.busbytes, pcm)
        self.assertFalse(any(kind == 120 for kind, _ in self.messages_for(.1)), 'completion must occur once')

    def test_unrelated_client_does_not_replace_real_cancellation(self):
        self.owner()
        url, _ = self.wav_url()
        self.s.sendall(fixture.frame(119, fixture.text(1, url)))
        self.unread_audio()
        self.unrelated_announcement()
        before = self.barrier()+self.messages_for(.1)
        self.s.sendall(self.event(0))
        after = self.barrier()+self.messages_for(.1)
        completions = [(kind, data) for kind, data in before+after if kind == 120]
        self.assertEqual(completions, [(120, fixture.num(1, 0))],
                         'only actual cancellation, not unrelated rejection, may complete the announcement')

    def test_audio_drain_alone_does_not_start_continuation(self):
        pcm = self.tts_continuation()
        messages = self.drain_audio(pcm)
        self.assertFalse(any(kind == 90 for kind, _ in messages),
                         f'prior RUN_END is still missing, but new capture began: {messages}')
        self.assertEqual([(kind, data) for kind, data in messages if kind == 120], [(120, fixture.num(1, 1))],
                         'drain acknowledgement must not wait for RUN_END (HA may await it)')
        self.s.sendall(self.event(2))
        next_messages = self.barrier()+self.messages_for(.1)
        starts = [data for kind, data in next_messages if kind == 90]
        self.assertEqual(len(starts), 1)
        self.assertIn(fixture.text(2, 'prior-conversation'), starts[0])
        self.assertFalse(any(kind == 120 for kind, _ in next_messages), 'RUN_END must not re-ack completed audio')

    def test_delayed_prior_run_end_cannot_clear_next_capture(self):
        pcm = self.tts_continuation()
        drained_messages = self.drain_audio(pcm)
        self.s.sendall(self.event(2))
        all_messages = drained_messages+self.barrier()+self.messages_for(.1)
        self.assertEqual(sum(kind == 90 for kind, _ in all_messages), 1)
        self.s.sendall(fixture.frame(91))
        self.barrier()
        self.assertTrue(json.loads((self.p/'status.json').read_text())['in_progress'],
                        'delayed prior RUN_END cleared the automatically-started next capture')

    def test_run_end_alone_does_not_start_continuation_before_fifo_drain(self):
        pcm = self.tts_continuation(run_end_first=True)
        messages = self.barrier()+self.messages_for(.15)
        self.assertFalse(any(kind in (90, 120) for kind, _ in messages), 'RUN_END must not bypass unread audio')
        drained = self.drain_audio(pcm)
        self.assertEqual(sum(kind == 120 for kind, _ in drained), 1)
        self.assertEqual(sum(kind == 90 for kind, _ in drained), 1)

    def test_no_tts_continuation_requires_only_run_end(self):
        self.owner()
        self.start_voice()
        self.s.sendall(self.event(6, conversation_id='no-tts-conversation', continue_conversation='1')+self.event(2))
        messages = self.barrier()+self.messages_for(.1)
        self.assertEqual(sum(kind == 90 for kind, _ in messages), 1)
        self.assertIn((106, fixture.num(2, 1)), messages)
        self.assertEqual(sum(kind == 120 for kind, _ in messages), 1)

    def test_standalone_preannounce_start_conversation_needs_no_run_end(self):
        self.owner()
        pre, pre_pcm = self.wav_url(900)
        main, main_pcm = self.wav_url(1800, 1200)
        self.s.sendall(fixture.frame(119, fixture.text(1, main)+fixture.text(3, pre)+fixture.num(4, 1)))
        self.wait_completion()
        self.assertEqual(self.busbytes, pre_pcm+main_pcm)
        kind, body = fixture.receive(self.s)
        self.assertEqual(kind, 90)
        self.assertIn(fixture.num(3, 1), body)

    def test_private_hardware_mute_suppresses_continuation_after_both_gates(self):
        pcm = self.tts_continuation()
        self.privacy.write_text('1\n')  # private physical-mute status, never host sysfs
        time.sleep(.35)
        drained = self.drain_audio(pcm)
        self.s.sendall(self.event(2))
        messages = drained+self.barrier()+self.messages_for(.15)
        self.assertFalse(any(kind == 90 for kind, _ in messages), 'physical mute must suppress automatic capture')
        self.assertFalse(json.loads((self.p/'status.json').read_text())['in_progress'])

    def test_standalone_media_audio_drain_needs_no_voice_run_end(self):
        self.owner()
        url, pcm = self.wav_url()
        body = b'\x0d'+struct.pack('<I', 1)+fixture.num(6, 1)+fixture.text(7, url)+fixture.num(8, 1)+fixture.num(9, 1)
        self.s.sendall(fixture.frame(65, body))
        self.wait_completion()
        self.assertEqual(self.busbytes, pcm)
        self.assertFalse(json.loads((self.p/'status.json').read_text())['in_progress'])


if __name__ == '__main__':
    unittest.main()
