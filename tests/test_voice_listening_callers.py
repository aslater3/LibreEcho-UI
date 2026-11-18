#!/usr/bin/env python3
"""Exercise silent ESPHome wake/LED transactions; local retains its wake chirp."""
from pathlib import Path
import os
import time
import unittest
from test_esphomed import Fixture, frame, num, receive

ROOT = Path(__file__).resolve().parents[1]


class SilentWake(Fixture):
    def test_esphome_wake_is_visual_only(self):
        self.hello()
        self.s.sendall(frame(89, num(1, 1) + num(2, 4)))
        wake = self.adapters['wake']
        wake.wait_streams()
        wake.samples(0)
        time.sleep(.15)
        wake.wake(100)
        self.assertEqual(receive(self.s)[0], 90)
        # Calls 0 and 1 are the daemon's startup releases of any stale HA light overlay (#395)
        # and of any stale set_ring overlay (#397); the listening indicator is the next pattern.
        # The HA turn uses the same Alexa-style direction cue as the wake/local paths: the
        # Listening theme with an optional azimuth from micd (absent here, so solid).
        listening = self.wait_call('led', 'pattern', 2)['args']
        self.assertEqual((listening['name'], listening['profile'], listening['owner'], listening['repeats']),
                         ('direction', 'listening', 'esphome', 0))
        # A turn indicator must never persist ring state via animate (#esphome-led).
        self.assertFalse(any(req['cmd'] == 'animate' for req in self.adapters['led'].calls),
                         'ESPHome turn indicators must not use persistent animate')
        # Ping is a processing fence; the LED transaction above is actual IO.
        self.s.sendall(frame(7))
        self.assertEqual(receive(self.s), (8, b''))
        self.assertFalse(any(req['cmd'] in ('cue', 'play', 'play_sample')
                             for req in self.adapters['audio'].calls),
                         'ESPHome wake must never request an audible cue')
        try:
            emitted = os.read(self.busfd, 65536)
        except BlockingIOError:
            emitted = b''
        self.assertEqual(emitted, b'', 'wake must not emit PCM on the speaker bus')
        self.s.sendall(frame(92, num(1, 0)))
        self.assertEqual(receive(self.s), (90, num(1, 0)))
        self.assertFalse(any(req['cmd'] == 'cue' for req in self.adapters['audio'].calls))
        # The ended turn's indicator must expire on its own: a finite flash or an
        # owner release, never an open-ended breathing animation. The direction
        # tracker may have re-sent cues in between, so match by content rather
        # than a fixed call index.
        deadline = time.monotonic() + 2
        ended = None
        while ended is None and time.monotonic() < deadline:
            for req in self.adapters['led'].calls:
                if req['cmd'] != 'pattern':
                    continue
                args = req.get('args') or {}
                if args.get('owner') != 'esphome':
                    continue
                if args.get('name') == 'stop' or (args.get('name') == 'flash' and 0 < args.get('repeats', 0) <= 5):
                    ended = args
                    break
            if ended is None:
                time.sleep(.02)
        self.assertIsNotNone(ended, 'the ended turn must release or expire its own indicator')
        self.assertFalse(any(req['cmd'] == 'animate' for req in self.adapters['led'].calls))


def main():
    local = (ROOT / 'src/adapter/voice_pipeline.c').read_text(encoding='utf-8')
    for call in ('le_voice_listening_feedback_set(1)', 'le_voice_listening_feedback_set(0)'):
        if call not in local:
            raise SystemExit('local capture must retain and clear audible wake feedback')
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.TestSuite([SilentWake('test_esphome_wake_is_visual_only')]))
    if not result.wasSuccessful():
        raise SystemExit(1)
    print('voice listening callers: actual ESPHome wake silent + visual, local chirp retained: ok')


if __name__ == '__main__':
    main()
