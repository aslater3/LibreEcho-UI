"""Real agent daemon, local fixture adapters; no model, device or network I/O."""
from contextlib import ExitStack
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Adapter:
    def __init__(self, path, state):
        self.path, self.state, self.calls = path, state, []
        self.failed = set()
        self.done = threading.Event()
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(str(path)); self.sock.listen(); self.sock.settimeout(.1)
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
                    ok = cmd not in self.failed
                    if ok:
                        if cmd == 'stop': self.state['playing'] = False
                        if cmd == 'dismiss': self.state['ringing'] = 0
                        if cmd == 'noise_stop': self.state['noise_active'] = False
                    result = {'v':1, 'id':message['id'], 'ok':ok}
                    if ok: result['data'] = dict(self.state)
                    else: result['error'] = 'fixture refused operation'
                    conn.sendall((json.dumps(result)+'\n').encode())
                except (OSError, ValueError):
                    continue

    def close(self):
        self.done.set(); self.thread.join(3); self.sock.close()
        if self.thread.is_alive(): raise RuntimeError('fixture did not stop')

class StopIntegration(unittest.TestCase):
    def test_real_daemon_local_stop_without_sign_in(self):
        with tempfile.TemporaryDirectory(prefix='le-stop-') as tmp, ExitStack() as stack:
            root = Path(tmp)
            adapters = {}
            for name, state in [('radio',{'playing':True}), ('timer',{'ringing':0}),
                                ('audio',{'noise_active':True}), ('tts',{'speaking':False})]:
                adapters[name] = Adapter(root/(name+'.sock'),state)
                stack.callback(adapters[name].close)
            media = root/'media.json'; media.write_text('{"active":"none"}')
            args = [str(ROOT/'build/libreecho-agentd'), '--socket',str(root/'agent.sock'),
                    '--config',str(root/'agent.json'),'--credentials',str(root/'credentials.json'),
                    '--curl','/nonexistent/no-model-transport','--media-status',str(media),
                    '--wake-socket',str(root/'absent-wake.sock'),
                    '--stt-socket',str(root/'absent-stt.sock')]
            for name in adapters: args += ['--'+name+'-socket',str(root/(name+'.sock'))]
            with (root/'agent.log').open('w+') as log:
                proc = subprocess.Popen(args,stdout=log,stderr=log)
                try:
                    deadline=time.monotonic()+5
                    while not (root/'agent.sock').exists() and time.monotonic()<deadline:
                        self.assertIsNone(proc.poll()); time.sleep(.02)
                    self.assertTrue((root/'agent.sock').exists())
                    def request(text):
                        with socket.socket(socket.AF_UNIX) as client:
                            client.settimeout(4); client.connect(str(root/'agent.sock'))
                            client.sendall((json.dumps({'v':1,'id':1,'cmd':'respond',
                                                       'args':{'text':text}})+'\n').encode())
                            return json.loads(client.makefile('rb').readline(8192))
                    result=request('Stop!')
                    self.assertTrue(result['ok'],result)
                    self.assertFalse(adapters['radio'].state['playing'])
                    self.assertFalse(adapters['audio'].state['noise_active'])
                    self.assertEqual(result['data']['text'],'')
                    self.assertNotIn('speak',adapters['audio'].calls)
                    self.assertTrue(request('stop')['ok'])  # idle stop is not conversation
                    adapters['radio'].state['playing']=True
                    adapters['timer'].state['ringing']=1
                    self.assertTrue(request('quiet')['ok'])
                    self.assertEqual(adapters['timer'].state['ringing'],0)
                    self.assertTrue(adapters['radio'].state['playing'])
                    adapters['timer'].state['ringing']=1
                    for text in ["don't stop",'tell me about the bus stop']:
                        before=len(adapters['radio'].calls)
                        self.assertFalse(request(text)['ok'])  # no configured model
                        self.assertEqual(len(adapters['radio'].calls),before)
                        self.assertEqual(adapters['timer'].state['ringing'],1)
                    adapters['timer'].state['ringing']=0
                    adapters['radio'].failed.add('stop')
                    result=request('stop the radio')
                    self.assertFalse(result['ok'],result)
                    self.assertTrue(adapters['radio'].state['playing'])
                    adapters['radio'].failed.clear()
                    self.assertTrue(request('stop')['ok'])
                    media.write_text('{"active":"airplay2"}')
                    result=request('stop')
                    self.assertTrue(result['ok'],result)
                    self.assertIn('phone',result['data']['text'])
                    self.assertIsNone(proc.poll())
                finally:
                    proc.terminate()
                    try: proc.wait(5)
                    except subprocess.TimeoutExpired:
                        proc.kill(); proc.wait(2)
                        self.fail('agent did not terminate within its fixture bound')

if __name__=='__main__': unittest.main()
