#!/usr/bin/env python3
"""The action button's "Start listening" (#372) reaches every wake subscriber.

buttond asks waked to "listen"; waked must publish the same wake_detected
event a real detection produces, so the on-device pipeline, the Home Assistant
satellite and a live session all start a turn without knowing the difference.
The event is marked with the "button" model so the log says where it came from.

Runs the real daemon against a fake microphone that streams silence.
"""
import json
import os
import socket
import sys
import time
import unittest

sys.dont_write_bytecode = True  # importing the fixture must not leave a cache
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_waked_subscriber_slots as slots  # noqa: E402

call = slots.call


def read_event(sock, timeout=3.0):
    sock.settimeout(timeout)
    data = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        for line in data.split(b'\n'):
            if b'"event"' in line:
                return json.loads(line)
    raise AssertionError('no wake event arrived: %r' % data)


class Listen(slots.SubscriberSlots):
    # Reuse the fixture only, not the slot tests.
    test_closed_event_subscribers_release_their_slots = None
    test_closed_audio_subscribers_release_their_slots = None
    test_live_subscribers_still_hold_the_limit = None

    def test_listen_publishes_a_wake_event(self):
        subscriber, reply = call(self.sock, 'subscribe')
        self.assertTrue(reply['ok'], reply)
        try:
            # Let a few frames through so the sample position is real.
            time.sleep(.3)
            s, reply = call(self.sock, 'listen')
            s.close()
            self.assertTrue(reply['ok'], reply)
            self.assertEqual(reply['data'], {'listening': True})
            event = read_event(subscriber)
        finally:
            subscriber.close()
        self.assertEqual(event['event'], 'wake_detected')
        self.assertEqual(event['data']['model'], 'button')
        self.assertGreaterEqual(event['data']['detection_sample'], 0)

    def test_listen_counts_as_a_detection(self):
        s, before = call(self.sock, 'status')
        s.close()
        s, reply = call(self.sock, 'listen')
        s.close()
        self.assertTrue(reply['ok'], reply)
        s, after = call(self.sock, 'status')
        s.close()
        self.assertEqual(after['data']['detected_count'],
                         before['data']['detected_count'] + 1)

    def test_unknown_command_is_still_refused(self):
        s, reply = call(self.sock, 'listen_now')
        s.close()
        self.assertFalse(reply['ok'])


if __name__ == '__main__':
    unittest.main()
