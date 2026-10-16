"""SPEC1/SPEC2 real daemon regression; run only inside a private namespace.
Requires ESPHOMED_BIN and TIMERD_BIN. No physical audio or public network.
"""
import contextlib,json,os,select,socket,struct,subprocess,time,unittest
from pathlib import Path
from test_esphomed_native_lifecycle import NativeLifecycle
import test_esphomed as f

if not os.environ.get('ESPHOMED_PRIVATE_NAMESPACE') or list(Path('/sys').iterdir()):
    raise RuntimeError('Run through the supplied private bwrap verifier; empty /sys is mandatory')

class FinalSpec(unittest.TestCase):
    setUp=NativeLifecycle.setUp
    tearDown=NativeLifecycle.tearDown
    connect=NativeLifecycle.connect
    hello=NativeLifecycle.hello
    owner=NativeLifecycle.owner
    barrier=NativeLifecycle.barrier
    messages_for=NativeLifecycle.messages_for
    wav_url=NativeLifecycle.wav_url
    http_audio=NativeLifecycle.http_audio
    unread_audio=NativeLifecycle.unread_audio
    drain_audio=NativeLifecycle.drain_audio
    wait_completion=NativeLifecycle.wait_completion

    def media(self,client,url,announce=True):
        body=b'\x0d'+struct.pack('<I',1)+f.num(6,1)+f.text(7,url)
        if announce: body+=f.num(8,1)+f.num(9,1)
        client.sendall(f.frame(65,body))
        return self.barrier(client)

    def secondary(self):
        client=self.connect();client.sendall(f.frame(1))
        self.assertEqual(f.receive(client)[0],2)
        return client

    def test_idle_secondary_invalid_url_cannot_notify_voice_owner(self):
        self.owner(); other=self.secondary()
        self.media(other,'file:///invalid')
        messages=self.barrier()+self.messages_for(.2)
        self.assertFalse(any(t==120 for t,_ in messages),messages)
        self.assertFalse(json.loads((self.p/'status.json').read_text())['in_progress'])

    def test_idle_secondary_valid_wav_cannot_notify_and_owner_drains_once(self):
        self.owner(); other=self.secondary(); url,pcm=self.wav_url()
        self.media(other,url)
        # Exercise real decoder/FIFO if buggy secondary command started playback.
        end=time.monotonic()+.5
        while time.monotonic()<end:
            with contextlib.suppress(BlockingIOError): self.busbytes+=os.read(self.busfd,65536)
            time.sleep(.005)
        messages=self.barrier()+self.messages_for(.2)
        self.assertFalse(any(t==120 for t,_ in messages),messages)
        self.assertEqual(self.busbytes,b'', 'non-owner finite voice announcement must not start PCM')
        self.media(self.s,url)
        self.unread_audio()
        messages=self.drain_audio(pcm)
        self.assertEqual([(t,b) for t,b in messages if t==120],[(120,f.num(1,1))])
        self.assertFalse(any(t==120 for t,_ in self.messages_for(.15)))
        # Ordinary secondary radio controls remain independent of voice ownership.
        self.media(other,url,False)
        end=time.monotonic()+2
        while not any(r['cmd']=='play' for r in self.adapters['radio'].calls) and time.monotonic()<end:time.sleep(.01)
        self.assertTrue(any(r['cmd']=='play' and r['args']['url']==url for r in self.adapters['radio'].calls))

    def actual_timerd(self):
        self.adapters['timer'].close(); del self.adapters['timer']
        self.timerpath=self.p/'timer.sock';self.timerpath.unlink()
        self.tp=subprocess.Popen([os.environ['TIMERD_BIN'],'--foreground','--socket',str(self.timerpath),
             '--state',str(self.p/'timers.json'),'--audio-socket',str(self.p/'missing-audio.sock')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        self.addCleanup(self.stop_timerd)
        end=time.monotonic()+2
        while not self.timerpath.exists() and time.monotonic()<end:time.sleep(.01)
        self.assertTrue(self.timerpath.exists())
        self.timer('add',{'seconds':300,'label':'local-survives'})

    def stop_timerd(self):
        self.tp.terminate();_,err=self.tp.communicate(timeout=3)
        self.assertEqual(self.tp.returncode,0,err.decode())

    def timer(self,cmd,args=None,path=None):
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(2);s.connect(str(path or self.timerpath))
            s.sendall((json.dumps({'v':1,'id':88,'cmd':cmd,'args':args or {}})+'\n').encode())
            data=b''
            while b'\n' not in data:data+=s.recv(8192)
        reply=json.loads(data.split(b'\n')[0]);self.assertTrue(reply['ok'],reply)
        return reply['data']

    def ids(self,path=None):return {t['external_id'] for t in self.timer('status',path=path)['remote_timers']}

    def wait_ids(self,ids,path=None):
        end=time.monotonic()+3
        while time.monotonic()<end:
            actual=self.ids(path)
            if actual==ids:return
            time.sleep(.02)
        self.assertEqual(actual,ids)

    def timer_event(self,id):
        self.s.sendall(f.frame(115,f.num(1,0)+f.text(2,id)+f.text(3,'remote')+f.num(4,300)+f.num(5,300)+f.num(6,1)))
        self.barrier()

    def stop_esphomed(self,crash=False):
        self.args=self.proc.args;start=time.monotonic()
        self.proc.kill() if crash else self.proc.terminate()
        _,err=self.proc.communicate(timeout=3)
        self.assertEqual(self.proc.returncode,-9 if crash else 0,err.decode())
        self.assertLess(time.monotonic()-start,2.5,'shutdown must have a hard bound even if adapter absent')

    def restart(self):
        self.proc=subprocess.Popen(self.args,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        end=time.monotonic()+3
        while time.monotonic()<end:
            try:self.s=self.connect();break
            except ConnectionRefusedError:time.sleep(.01)
        else:self.fail('restart did not listen')
        self.owner()
        status=json.loads((self.p/'status.json').read_text())
        self.assertEqual(status['pid'],self.proc.pid,'status belongs to replacement process')
        self.assertTrue(status['start_time'] and status['boot_id'] and status['listener_inode'])
        sockets={os.readlink(fd) for fd in Path('/proc/'+str(self.proc.pid)+'/fd').iterdir()}
        self.assertIn('socket:['+status['listener_inode']+']',sockets)

    def assert_local(self,path=None):
        self.assertTrue(any(t['label']=='local-survives' for t in self.timer('status',path=path)['timers']))

    def test_graceful_shutdown_clears_real_remote_table_preserves_local(self):
        self.actual_timerd();self.owner();self.timer_event('old-owner');self.wait_ids({'old-owner'})
        self.stop_esphomed();self.wait_ids(set());self.assert_local()
        self.restart();self.timer_event('new-owner');self.wait_ids({'new-owner'});self.assert_local()

    def test_crash_restart_clears_real_remote_table_before_replacement(self):
        self.actual_timerd();self.owner();self.timer_event('old-owner');self.wait_ids({'old-owner'})
        self.stop_esphomed(True);self.assertEqual(self.ids(),{'old-owner'})
        self.restart();self.wait_ids(set());self.assert_local()
        self.timer_event('new-owner');self.wait_ids({'new-owner'});time.sleep(.2);self.wait_ids({'new-owner'})

    def test_startup_unavailable_retries_and_fences_real_table_events(self):
        self.actual_timerd();self.owner();self.timer_event('old-owner');self.wait_ids({'old-owner'})
        self.stop_esphomed(True)
        hidden=self.p/'hidden-timer.sock';self.timerpath.rename(hidden)
        self.restart()
        try:self.timer_event('new-owner')
        except EOFError:self.fail('replacement timer was rejected instead of fenced during adapter outage')
        time.sleep(1.2)
        self.assertEqual(self.ids(hidden),{'old-owner'},'replacement must remain unsent until startup clear ACK')
        hidden.rename(self.timerpath);self.wait_ids({'new-owner'});self.assert_local()
        time.sleep(.3);self.wait_ids({'new-owner'})

    def test_shutdown_unavailable_is_bounded_and_next_startup_recovers(self):
        self.actual_timerd();self.owner();self.timer_event('old-owner');self.wait_ids({'old-owner'})
        hidden=self.p/'hidden-timer.sock';self.timerpath.rename(hidden)
        self.stop_esphomed();self.assertEqual(self.ids(hidden),{'old-owner'})
        hidden.rename(self.timerpath);self.restart();self.wait_ids(set());self.assert_local()

del NativeLifecycle  # avoid unittest rediscovering imported baseline class
if __name__=='__main__':unittest.main()
