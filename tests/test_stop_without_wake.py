"""Stop without the wake word (#405), through the real agent daemon.

Real libreecho-agentd and the real mock sttd; fixture timer/audio/radio/tts
adapters and a scripted wake socket that streams short utterances and never
announces a wake word. No model, device or network I/O.
"""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
FRAME = 160


class Adapter:
    """One-request-per-connection JSON adapter, like the real daemons."""

    def __init__(self, path, state, engine=None):
        self.path, self.state, self.calls = path, state, []
        self.engine = engine
        self.done = threading.Event()
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(str(path)); self.sock.listen(8); self.sock.settimeout(.1)
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        while not self.done.is_set():
            try: conn, _ = self.sock.accept()
            except socket.timeout: continue
            with conn:
                conn.settimeout(2)
                try:
                    message = json.loads(conn.makefile('rb').readline(4096))
                    cmd = message['cmd']; self.calls.append(cmd)
                    if cmd == 'dismiss': self.state['ringing'] = 0
                    data = dict(self.state)
                    if self.engine and cmd == 'status': data['engine'] = self.engine
                    ok = not (self.engine and cmd == 'recognize_stream')
                    reply = {'v': 1, 'id': message['id'], 'ok': ok}
                    if ok: reply['data'] = data
                    else: reply['error'] = 'fixture refuses to recognise'
                    conn.sendall((json.dumps(reply) + '\n').encode())
                except (OSError, ValueError):
                    continue

    def close(self):
        self.done.set(); self.thread.join(3); self.sock.close()
        if self.thread.is_alive(): raise RuntimeError('fixture did not stop')


class WakeSource:
    """waked's two subscriptions: wake events (never sent) and indexed audio,
    300 ms of 'speech' then 400 ms of silence, repeated until closed."""

    def __init__(self, path):
        self.done = threading.Event()
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(str(path)); self.sock.listen(4); self.sock.settimeout(.1)
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def accept(self):
        while not self.done.is_set():
            try: conn, _ = self.sock.accept()
            except socket.timeout: continue
            conn.settimeout(2)
            message = json.loads(conn.makefile('rb').readline(4096))
            # Compact JSON: the pipeline looks for "ok":true verbatim.
            conn.sendall((json.dumps({'v': 1, 'id': message['id'], 'ok': True,
                                      'data': {'streaming': True}},
                                     separators=(',', ':')) + '\n').encode())
            return conn, message['cmd']
        return None, None

    def run(self):
        conns = {}
        while len(conns) < 2 and not self.done.is_set():
            conn, cmd = self.accept()
            if conn: conns[cmd] = conn
        audio = conns.get('stream_audio')
        sample = 0
        speech = struct.pack('<%dh' % FRAME, *([1200] * FRAME))
        silence = bytes(FRAME * 2)
        try:
            while audio and not self.done.is_set():
                for i in range(70):
                    payload = speech if i < 30 else silence
                    header = struct.pack('<IHHQII', 0x3153564c, 1, 0, sample, FRAME, 0)
                    audio.sendall(header + payload)
                    sample += FRAME
                    time.sleep(.01)
        except OSError:
            pass
        for conn in conns.values(): conn.close()

    def close(self):
        self.done.set(); self.thread.join(5); self.sock.close()


class StopWithoutWake(unittest.TestCase):
    def run_agent(self, root, stack, heard=None, engine=None, enable=True,
                  ringing=1, seconds=3.0, speaking=False, before=None):
        os.makedirs(root, exist_ok=True)
        adapters = {}
        for name, state in [('radio', {'playing': True}), ('timer', {'ringing': ringing}),
                            ('audio', {'noise_active': False}), ('tts', {'speaking': speaking})]:
            adapters[name] = Adapter(root / (name + '.sock'), state)
            stack.callback(adapters[name].close)
        wake = WakeSource(root / 'wake.sock'); stack.callback(wake.close)
        stt_calls = None
        if engine:
            stt = Adapter(root / 'stt.sock', {'ready': True}, engine=engine)
            stack.callback(stt.close); stt_calls = stt.calls
        else:
            env = dict(os.environ, LIBREECHO_MOCK_STT_TEXT=heard)
            sttd = subprocess.Popen([str(ROOT / 'build/libreecho-sttd'), '--socket',
                                     str(root / 'stt.sock'), '--model-dir', 'mock',
                                     '--threads', '1'], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            stack.callback(lambda: (sttd.terminate(), sttd.wait(5)))
            deadline = time.monotonic() + 5
            while not (root / 'stt.sock').exists() and time.monotonic() < deadline:
                time.sleep(.02)
        media = root / 'media.json'; media.write_text('{"active":"none"}')
        args = [str(ROOT / 'build/libreecho-agentd'), '--socket', str(root / 'agent.sock'),
                '--config', str(root / 'agent.json'), '--credentials', str(root / 'creds.json'),
                '--curl', str(ROOT / 'build/mock-llm-curl'), '--media-status', str(media),
                '--wake-socket', str(root / 'wake.sock'), '--stt-socket', str(root / 'stt.sock')]
        for name in adapters: args += ['--' + name + '-socket', str(root / (name + '.sock'))]
        log = (root / 'agent.log').open('w+'); stack.callback(log.close)
        env = dict(os.environ, LE_TEST_CURL_CAPTURE=str(root / 'curl.txt'))
        proc = subprocess.Popen(args, stdout=log, stderr=log, env=env)

        def stop_agent():
            proc.terminate()
            try: proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(2)
                raise AssertionError('agent did not terminate within its fixture bound')
        stack.callback(stop_agent)
        deadline = time.monotonic() + 5
        while not (root / 'agent.sock').exists() and time.monotonic() < deadline:
            self.assertIsNone(proc.poll()); time.sleep(.02)

        def request(cmd, args=None):
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(4); client.connect(str(root / 'agent.sock'))
                client.sendall((json.dumps({'v': 1, 'id': 1, 'cmd': cmd,
                                            'args': args or {}}) + '\n').encode())
                return json.loads(client.makefile('rb').readline(16384))
        if enable:
            result = request('configure', {'stop_without_wake': True})
            self.assertTrue(result['ok'], result)
            self.assertTrue(result['data']['stop_without_wake'])
        if before:
            before(request, adapters)
        time.sleep(seconds)
        status = request('status')
        self.assertTrue(status['ok'], status)
        return adapters, status['data'], request, stt_calls, proc

    def test_default_off_never_listens(self):
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            adapters, status, request, _, proc = self.run_agent(
                Path(tmp), stack, heard='stop', enable=False)
            self.assertFalse(status['stop_without_wake'])
            self.assertEqual(status['stop_windows'], 0)
            self.assertEqual(adapters['timer'].state['ringing'], 1)
            self.assertNotIn('dismiss', adapters['timer'].calls)
            self.assertIsNone(proc.poll())

    def test_bare_stop_silences_a_ringing_timer(self):
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            root = Path(tmp)
            adapters, status, request, _, proc = self.run_agent(root, stack, heard='stop')
            self.assertEqual(adapters['timer'].state['ringing'], 0)
            self.assertIn('dismiss', adapters['timer'].calls)
            self.assertGreaterEqual(status['stops_heard'], 1)
            # Only the ring: music keeps playing and nothing was spoken.
            self.assertNotIn('stop', adapters['radio'].calls)
            self.assertNotIn('speak', adapters['audio'].calls)
            # Nothing rings any more, so the window has closed again.
            self.assertFalse(status['stop_listening'])
            history = request('voice_history')
            self.assertTrue(history['ok'], history)
            self.assertIn('"stop"', json.dumps(history['data']))
            # The choice is persisted for the next start.
            self.assertTrue(json.loads((root / 'agent.json').read_text())['stop_without_wake'])
            self.assertIsNone(proc.poll())

    def test_bare_stop_ends_a_reply(self):
        # A reply from a (mock) local model is still being spoken when the
        # owner says "stop": the queue is cancelled and the speech stopped.
        def reply(request, adapters):
            result = request('configure', {'provider': 'openai-compatible',
                                           'enabled': True,
                                           'base_url': 'http://198.51.100.20:8001/v1'})
            self.assertTrue(result['ok'], result)
            result = request('respond', {'text': 'Say hello.'})
            self.assertTrue(result['ok'], result)
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            adapters, status, request, _, proc = self.run_agent(
                Path(tmp), stack, heard='stop', ringing=0, speaking=True,
                before=reply)
            self.assertIn('speak', adapters['audio'].calls)
            self.assertIn('stop_speech', adapters['audio'].calls)
            self.assertGreaterEqual(status['stops_heard'], 1)
            self.assertNotIn('dismiss', adapters['timer'].calls)
            self.assertNotIn('stop', adapters['radio'].calls)
            self.assertIsNone(proc.poll())

    def test_room_chatter_is_not_a_stop(self):
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            adapters, status, request, _, proc = self.run_agent(
                Path(tmp), stack, heard="don't stop me now")
            self.assertGreaterEqual(status['stop_windows'], 2)
            self.assertEqual(status['stops_heard'], 0)
            self.assertEqual(adapters['timer'].state['ringing'], 1)
            self.assertNotIn('dismiss', adapters['timer'].calls)
            history = request('voice_history')
            self.assertNotIn('stop me now', json.dumps(history['data']))
            self.assertIsNone(proc.poll())

    def test_nothing_ringing_means_no_window(self):
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            adapters, status, _, _, proc = self.run_agent(
                Path(tmp), stack, heard='stop', ringing=0)
            self.assertEqual(status['stop_windows'], 0)
            self.assertNotIn('dismiss', adapters['timer'].calls)
            self.assertIsNone(proc.poll())

    def test_remote_recogniser_is_never_used(self):
        # A Wyoming sttd would carry every window to another machine.
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            adapters, status, _, stt_calls, proc = self.run_agent(
                Path(tmp), stack, engine='wyoming')
            self.assertIn('status', stt_calls)
            self.assertNotIn('recognize_stream', stt_calls)
            self.assertEqual(status['stop_windows'], 0)
            self.assertEqual(adapters['timer'].state['ringing'], 1)
            self.assertIsNone(proc.poll())

    def test_configure_rejects_a_non_boolean(self):
        with tempfile.TemporaryDirectory(prefix='le-405-') as tmp, ExitStack() as stack:
            _, _, request, _, _ = self.run_agent(
                Path(tmp), stack, heard='stop', enable=False, seconds=0.2)
            result = request('configure', {'stop_without_wake': 'yes'})
            self.assertFalse(result['ok'])


if __name__ == '__main__':
    unittest.main()
