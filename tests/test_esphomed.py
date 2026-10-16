"""Isolated real-daemon sockets/FIFO fixtures; no host device access."""
import contextlib,json,os,pathlib,socket,struct,subprocess,tempfile,threading,time,unittest,wave,io
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
BIN=os.environ.get('ESPHOMED_BIN',str(pathlib.Path(__file__).parents[1]/'build/libreecho-esphomed'))
def var(v):
 b=bytearray()
 while v>127:b.append((v&127)|128);v>>=7
 b.append(v);return bytes(b)
def num(f,v):return var(f<<3)+var(v)
def text(f,v):
 if isinstance(v,str):v=v.encode()
 return var((f<<3)|2)+var(len(v))+v
def frame(t,b=b''):return b'\0'+var(len(b))+var(t)+b
def recvvar(s):
 v=0
 for i in range(5):
  b=s.recv(1)
  if not b:raise EOFError()
  v|=(b[0]&127)<<(i*7)
  if b[0]<128:return v
 raise ValueError('varint')
def receive(s):
 if s.recv(1)!=b'\0':raise EOFError()
 n=recvvar(s);t=recvvar(s);b=bytearray()
 while len(b)<n:
  z=s.recv(n-len(b))
  if not z:raise EOFError()
  b+=z
 return t,bytes(b)
class Adapter:
 def __init__(self,path,kind):
  self.kind=kind;self.calls=[];self.streams={};self.stop=False;self.clients=[];self.muted=False;self.fail=False
  self.sock=socket.socket(socket.AF_UNIX);self.sock.bind(str(path));self.sock.listen();self.sock.settimeout(.1)
  self.thread=threading.Thread(target=self.run);self.thread.start()
 def run(self):
  while not self.stop:
   try:c,_=self.sock.accept()
   except TimeoutError:continue
   self.clients.append(c);threading.Thread(target=self.handle,args=(c,),daemon=True).start()
 def handle(self,c):
  c.settimeout(2);b=bytearray()
  try:
   while b'\n' not in b:
    z=c.recv(4096)
    if not z:return
    b+=z
   req=json.loads(b.split(b'\n')[0]);self.calls.append(req);cmd=req['cmd']
   if self.kind=='wake' and cmd=='set_word':time.sleep(getattr(self,'set_word_delay',0))
   if self.kind=='wake' and cmd in ('subscribe','stream_audio'):
    self.streams[cmd]=c;data={'subscribed':True} if cmd=='subscribe' else {'streaming':True,'format':'pcm_s16_le','sample_rate':16000,'channels':1,'frame_header_bytes':24,'sample_indexed':True}
    c.sendall((json.dumps({'v':1,'id':req['id'],'ok':True,'data':data})+'\n').encode());return
   if cmd=='set_mute' and not self.fail:self.muted=req['args']['muted']
   data={'muted':self.muted,'volume':66,'output_available':True} if self.kind=='audio' else {'playing':False,'paused':False,'url':'','title':'','station':''}
   c.sendall((json.dumps({'v':1,'id':req['id'],'ok':not self.fail,'data':data})+'\n').encode())
  except (OSError,ValueError):pass
  finally:
   if c not in self.streams.values():c.close()
 def wait_streams(self):
  end=time.monotonic()+3
  while len(self.streams)<2 and time.monotonic()<end:time.sleep(.01)
  assert len(self.streams)==2,self.calls
 def samples(self,first,count=320):
  self.streams['stream_audio'].sendall(struct.pack('<IHHQII',0x3153564c,1,0,first,count,0)+struct.pack('<'+'h'*count,*[(first+i)%30000 for i in range(count)]))
 def wake(self,sample):self.streams['subscribe'].sendall((json.dumps({'v':1,'event':'wake_detected','data':{'detection_sample':sample,'score':1,'model':'Alexa'}})+'\n').encode())
 def close(self):
  self.stop=True;self.thread.join(2);self.sock.close()
  for c in self.clients:
   with contextlib.suppress(OSError):c.close()
class Daemon(unittest.TestCase):
 def setUp(self):
  self.assertTrue(os.access(BIN,os.X_OK), 'daemon executable is missing')
  self.tmp=tempfile.TemporaryDirectory(prefix='esphomed-',dir=os.environ.get('TMPDIR'));self.p=pathlib.Path(self.tmp.name)
  self.config=self.p/'config.json';self.config.write_text(json.dumps(getattr(self,'config_fields',{'hostname':'fixture-satellite','wifi_mac':'02:00:00:00:00:01','ha_protocol':'esphome','voice_assistant_mode':1,'esphome_noise_key':''})))
  self.privacy=self.p/'privacy';self.privacy.write_text('0\n');self.bus=self.p/'system.pcm';os.mkfifo(self.bus);self.busfd=os.open(self.bus,os.O_RDONLY|os.O_NONBLOCK);self.busbytes=bytearray()
  self.adapters={k:Adapter(self.p/(k+'.sock'),k) for k in ('audio','wake','radio','timer','led')}
  s=socket.socket();s.bind(('127.0.0.1',0));self.port=s.getsockname()[1];s.close();self.clients=[]
  cmd=[BIN,'--port',str(self.port),'--bind','127.0.0.1']+(['--plaintext'] if getattr(self,'plaintext',True) else [])+['--config',str(self.config),'--status-file',str(self.p/'status.json'),'--privacy-state',str(self.privacy),'--audio-bus',str(self.bus),'--mdns-socket',str(self.p/'missing-mdns')]
  for k in self.adapters:cmd+=['--'+k+'-socket',str(self.p/(k+'.sock'))]
  cmd+=getattr(self,'extra_args',[]);self.proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.PIPE);end=time.monotonic()+3
  while time.monotonic()<end:
   try:self.s=self.connect();break
   except ConnectionRefusedError:time.sleep(.01)
  else:self.fail(self.proc.communicate(timeout=1))
 def connect(self):
  s=socket.create_connection(('127.0.0.1',self.port),1);s.settimeout(3);self.clients.append(s);return s
 def tearDown(self):
  for s in self.clients:s.close()
  self.proc.terminate();out,err=self.proc.communicate(timeout=3)
  for a in self.adapters.values():a.close()
  os.close(self.busfd);self.tmp.cleanup();self.assertIn(self.proc.returncode,(0,-15),err.decode())
 def hello(self):
  for b in frame(1,num(2,1)+num(3,14)):self.s.sendall(bytes([b]))
  self.assertEqual(receive(self.s)[0],2)
class Fixture(Daemon):
 def test_hello_auth_device_list_ping_disconnect(self):
  self.hello();self.s.sendall(frame(3)+frame(9));t,b=receive(self.s);self.assertEqual(t,10);self.assertIn(b'fixture-satellite',b);self.assertIn(num(17,61),b)
  self.s.sendall(frame(11));self.assertEqual([receive(self.s)[0] for _ in range(3)],[63,17,19]);self.s.sendall(frame(7));self.assertEqual(receive(self.s),(8,b''));self.s.sendall(frame(5));self.assertEqual(receive(self.s),(6,b''))
 def test_oversize_malformed_and_client_limit(self):
  self.hello();s2=self.connect();s3=self.connect();self.assertEqual(s3.recv(1),b'');s2.sendall(b'\0'+var(65537)+var(1));self.assertEqual(s2.recv(1),b'')
  time.sleep(.05);s4=self.connect();s4.sendall(frame(9,b'\x08'+b'\xff'*10));self.assertEqual(s4.recv(1),b'')
 def test_voice_preroll_vad_wav_bus_completion(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams()
  for i in range(0,6400,320):a.samples(i)
  time.sleep(.1);a.wake(4000);t,b=receive(self.s);self.assertEqual(t,90);self.assertIn(num(1,1),b);self.s.settimeout(.1)
  with self.assertRaises(TimeoutError):receive(self.s)
  self.s.settimeout(3);self.s.sendall(frame(91));t,b=receive(self.s);self.assertEqual(t,106);self.assertIn(struct.pack('<h',800),b[:10]);self.s.sendall(frame(92,num(1,12)))
  while True:
   t,b=receive(self.s)
   if t==106 and num(2,1) in b:break
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1200)*4800)
  data=wav.getvalue()
  class HTTP(BaseHTTPRequestHandler):
   def do_GET(self):self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
   def log_message(self,*a):pass
  server=ThreadingHTTPServer(('127.0.0.1',0),HTTP);thread=threading.Thread(target=server.serve_forever);thread.start()
  try:
   url=f'http://127.0.0.1:{server.server_port}/tts.wav';event=num(1,8)+text(2,text(1,'url')+text(2,url));self.s.sendall(frame(92,event)+frame(92,num(1,2)));end=time.monotonic()+4
   while time.monotonic()<end:
    with contextlib.suppress(BlockingIOError):self.busbytes+=os.read(self.busfd,65536)
    self.s.settimeout(.05)
    try:t,b=receive(self.s)
    except TimeoutError:continue
    if t==120:break
   else:self.fail('no drained completion')
   self.assertEqual(b,num(1,1));self.assertGreater(len(self.busbytes),16000);self.assertEqual(self.busbytes[:40],struct.pack('<h',1200)*20)
   status=json.loads((self.p/'status.json').read_text());self.assertFalse(status['in_progress']);self.assertEqual(status['last_result'],'success')
  finally:server.shutdown();thread.join(2);server.server_close()
 def wait_call(self,kind,cmd,after=0):
  end=time.monotonic()+2
  while time.monotonic()<end:
   matching=[req for req in self.adapters[kind].calls if req['cmd']==cmd]
   if len(matching)>after:return matching[after]
   time.sleep(.01)
  self.fail('missing actual adapter transaction: '+kind+'/'+cmd)
 def test_media_mute_timer_config_transactions_and_latch(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));time.sleep(.1)
  self.s.sendall(frame(65,b'\x0d'+struct.pack('<I',1)+num(6,1)+text(7,'http://example.invalid/radio.mp3')))
  self.assertEqual(self.wait_call('radio','play')['args'],{'url':'http://example.invalid/radio.mp3'})
  for value,command in [(1,'pause'),(0,'resume'),(2,'stop')]:
   self.s.sendall(frame(65,b'\x0d'+struct.pack('<I',1)+num(2,1)+num(3,value)));self.assertEqual(self.wait_call('radio',command)['args'],{})
  self.s.sendall(frame(65,b'\x0d'+struct.pack('<I',1)+num(4,1)+b'\x2d'+struct.pack('<f',.42)))
  self.assertEqual(self.wait_call('audio','set_volume')['args'],{'volume':42})
  self.s.sendall(frame(33,b'\x0d'+struct.pack('<I',2)+num(2,1)));self.assertEqual(self.wait_call('audio','set_mute')['args'],{'muted':True})
  before=len([r for r in self.adapters['audio'].calls if r['cmd']=='set_mute']);self.privacy.write_text('1\n');time.sleep(.3)
  self.s.sendall(frame(33,b'\x0d'+struct.pack('<I',2)));time.sleep(.2)
  self.assertEqual(len([r for r in self.adapters['audio'].calls if r['cmd']=='set_mute']),before)
  self.s.sendall(frame(115,num(1,0)+text(2,'fixture-timer')+text(3,'tea')+num(4,60)+num(5,45)+num(6,1)))
  self.assertEqual(self.wait_call('timer','remote_event')['args'],{'event_type':0,'timer_id':'fixture-timer','name':'tea','total_seconds':60,'seconds_left':45,'is_active':True})
  self.s.sendall(frame(121));self.assertEqual(receive(self.s)[0],122);self.s.sendall(frame(123,text(1,'alexa_v0.1')));self.assertEqual(receive(self.s)[0],122)
  self.assertEqual(self.wait_call('wake','set_word')['args'],{'word':'Alexa'});self.assertEqual(json.loads(self.config.read_text())['esphome_active_wake_word'],'alexa_v0.1')
 def test_owner_overlap_error_cleanup_and_negative_announcement(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));time.sleep(.05)
  other=self.connect();other.sendall(frame(1));self.assertEqual(receive(other)[0],2);other.sendall(frame(89,num(1,1)+num(2,4)));self.assertEqual(other.recv(1),b'')
  self.s.sendall(frame(119,text(1,'file:///etc/passwd')));self.assertEqual(receive(self.s),(120,num(1,0)));self.assertEqual(json.loads((self.p/'status.json').read_text())['last_result'],'playback_error')
  clears=len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_clear'])
  a=self.adapters['wake'];a.wait_streams();a.samples(0);time.sleep(.1);a.wake(100);self.assertEqual(receive(self.s)[0],90);a.wake(200);self.s.settimeout(.1)
  with self.assertRaises(TimeoutError):receive(self.s)
  self.s.settimeout(3);self.s.sendall(frame(92,num(1,0)));self.assertEqual(receive(self.s),(90,num(1,0)));time.sleep(.1);self.assertEqual(len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_clear']),clears,'ordinary pipeline errors must preserve HA timers')
  self.s.sendall(frame(89));self.wait_call('timer','remote_clear',after=clears)
 def http_audio(self,data):
  class HTTP(BaseHTTPRequestHandler):
   def do_GET(self):self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
   def log_message(self,format,*args):pass
  server=ThreadingHTTPServer(('127.0.0.1',0),HTTP);thread=threading.Thread(target=server.serve_forever);thread.start()
  self.addCleanup(server.server_close);self.addCleanup(thread.join,2);self.addCleanup(server.shutdown)
  return f'http://127.0.0.1:{server.server_port}/audio'
 def wait_completion(self,expected=True):
  end=time.monotonic()+5
  while time.monotonic()<end:
   with contextlib.suppress(BlockingIOError):self.busbytes+=os.read(self.busfd,65536)
   self.s.settimeout(.05)
   try:t,b=receive(self.s)
   except TimeoutError:continue
   if t==120:self.assertEqual(b,num(1,int(expected)));self.s.settimeout(3);return
  self.fail('missing bounded playback completion')
 def test_mp3_finite_decode_bus_and_continuation(self):
  import shutil
  ffmpeg=shutil.which('ffmpeg')
  self.assertIsNotNone(ffmpeg,'existing ffmpeg needed to make actual MP3 fixture')
  mp3=self.p/'fixture.mp3';subprocess.run([ffmpeg,'-v','error','-f','lavfi','-i','sine=frequency=440:sample_rate=24000:duration=0.15','-ac','1','-codec:a','libmp3lame','-b:a','48k',str(mp3)],check=True,timeout=10)
  url=self.http_audio(mp3.read_bytes());self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));time.sleep(.1)
  self.s.sendall(frame(119,text(1,url)+num(4,1)));self.wait_completion();self.assertGreater(len(self.busbytes),24000);self.assertTrue(any(self.busbytes));self.assertEqual(len(self.busbytes)%4,0)
  t,b=receive(self.s);self.assertEqual(t,90);self.assertIn(num(1,1),b)
 def test_wav_waits_until_fifo_consumed_not_run_end(self):
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',2200)*1000)
  url=self.http_audio(wav.getvalue());self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));time.sleep(.1);self.s.sendall(frame(119,text(1,url)))
  time.sleep(.3);self.s.sendall(frame(7));self.assertEqual(receive(self.s),(8,b''));self.assertTrue(json.loads((self.p/'status.json').read_text())['in_progress'])
  self.wait_completion();self.assertEqual(self.busbytes,struct.pack('<h',2200)*2000)
 def test_intent_metadata_retained_through_early_tts_drain(self):
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1400)*1000)
  url=self.http_audio(wav.getvalue());self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams();a.samples(0);time.sleep(.1);a.wake(100);self.assertEqual(receive(self.s)[0],90);self.s.sendall(frame(91));self.s.sendall(frame(92,num(1,12)))
  while receive(self.s)[0]!=106:pass
  self.s.sendall(frame(92,num(1,1)+text(2,text(1,'url')+text(2,url))))
  self.s.sendall(frame(92,num(1,6)+text(2,text(1,'conversation_id')+text(2,'fixture-conversation'))+text(2,text(1,'continue_conversation')+text(2,'1'))))
  self.s.sendall(frame(92,num(1,100)+text(2,text(1,'tts_start_streaming')+text(2,'1')))+frame(92,num(1,2)))
  time.sleep(.25);self.assertTrue(json.loads((self.p/'status.json').read_text())['in_progress']);self.wait_completion();t,b=receive(self.s);self.assertEqual(t,90);self.assertIn(text(2,'fixture-conversation'),b)
 def test_preannounce_precedes_main_and_finishes_once(self):
  urls=[]
  for sample,count in [(900,1000),(1800,1200)]:
   wav=io.BytesIO()
   with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',sample)*count)
   urls.append(self.http_audio(wav.getvalue()))
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));self.s.sendall(frame(119,text(1,urls[1])+text(3,urls[0])));self.wait_completion();self.assertEqual(self.busbytes,struct.pack('<h',900)*2000+struct.pack('<h',1800)*2400)
 def test_adapter_failure_does_not_claim_unmute_or_volume(self):
  self.hello();self.adapters['audio'].fail=True;self.adapters['audio'].muted=True
  self.s.sendall(frame(33,b'\x0d'+struct.pack('<I',2)+num(2,1)));self.wait_call('audio','set_mute')
  self.s.sendall(frame(33,b'\x0d'+struct.pack('<I',2)));time.sleep(.1);self.s.sendall(frame(20));seen=[]
  for _ in range(2):seen.append(receive(self.s))
  switch=[b for t,b in seen if t==26];self.assertTrue(switch);self.assertIn(num(2,1),switch[-1])
  self.s.sendall(frame(65,b'\x0d'+struct.pack('<I',1)+num(4,1)+b'\x2d'+struct.pack('<f',float('nan'))))
  for _ in range(16):
   try:t,b=receive(self.s)
   except EOFError:break
   self.assertIn(t,(26,64)) # A state broadcast already queued before rejection may drain.
  else:self.fail('malformed volume did not close the client')
 def test_announcement_error_reports_failure_and_resets_playback(self):
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1300)*1000)
  url=self.http_audio(wav.getvalue());self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4))+frame(119,text(1,url)));time.sleep(.2);clears=len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_clear']);self.s.sendall(frame(92,num(1,0)))
  messages=[];self.s.settimeout(.5)
  try:
   for _ in range(4):
    messages.append(receive(self.s))
    if messages[-1][0]==120:break
  except TimeoutError:pass
  self.assertIn((120,num(1,0)),messages,'cancelled announcement must return a negative completion');self.assertFalse(json.loads((self.p/'status.json').read_text())['in_progress']);self.assertEqual(len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_clear']),clears)
  self.s.settimeout(3);self.s.sendall(frame(119,text(1,'file:///invalid')));self.assertEqual(receive(self.s),(120,num(1,0)))
 def test_wake_start_requests_stt_not_ha_wake_word(self):
  # The wake word is detected on the device; asking HA to run its own wake
  # engine (USE_WAKE_WORD, bit 1) makes HA fail the run with wake-engine-missing.
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams();a.samples(0);time.sleep(.1);a.wake(100)
  t,b=receive(self.s);self.assertEqual(t,90);self.assertIn(num(3,1),b);self.assertNotIn(num(3,3),b);self.assertIn(text(5,'Alexa'),b)
 def test_no_tts_run_end_continues_and_preserves_timers(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams();a.samples(0);time.sleep(.1);a.wake(100);self.assertEqual(receive(self.s)[0],90);self.s.sendall(frame(91))
  clears=len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_clear'])
  self.s.sendall(frame(92,num(1,6)+text(2,text(1,'conversation_id')+text(2,'no-tts-conversation'))+text(2,text(1,'continue_conversation')+text(2,'1')))+frame(92,num(1,2)))
  messages=[]
  for _ in range(8):
   messages.append(receive(self.s))
   if messages[-1][0]==90:break
  ended=next(i for i,(t,b) in enumerate(messages) if t==106 and b==num(2,1));done=next(i for i,(t,b) in enumerate(messages) if t==120);self.assertLess(ended,done);self.assertEqual(messages[-1][0],90);self.assertIn(text(2,'no-tts-conversation'),messages[-1][1]);self.assertEqual(len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_clear']),clears)
 def test_empty_wake_selection_survives_reload_without_local_config_loss(self):
  import signal
  saved=json.loads(self.config.read_text());saved['wake_word']='Local saved word';self.config.write_text(json.dumps(saved));self.hello();self.s.sendall(frame(123));self.assertEqual(receive(self.s)[0],122)
  saved=json.loads(self.config.read_text());self.assertEqual(saved.get('esphome_active_wake_word'),'');self.assertEqual(saved['wake_word'],'Local saved word');self.proc.send_signal(signal.SIGHUP);time.sleep(.1)
  self.s=self.connect();self.hello();self.s.sendall(frame(121));t,b=receive(self.s);self.assertEqual(t,122);self.assertNotIn(text(2,'alexa_v0.1'),b)
  a=self.adapters['wake'];a.wait_streams();a.samples(0);a.wake(100);self.s.sendall(frame(7));self.assertEqual(receive(self.s),(8,b''))
  self.s.sendall(frame(123,text(1,'alexa_v0.1')));self.assertEqual(receive(self.s)[0],122);self.assertEqual(self.wait_call('wake','set_word')['args'],{'word':'Alexa'});self.assertEqual(json.loads(self.config.read_text())['esphome_active_wake_word'],'alexa_v0.1');self.assertEqual(json.loads(self.config.read_text())['wake_word'],'Local saved word')
 def test_finite_chunked_wav_decodes_before_completion(self):
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1700)*1000)
  data=wav.getvalue()
  class HTTP(BaseHTTPRequestHandler):
   protocol_version='HTTP/1.1'
   def do_GET(self):
    self.send_response(200);self.send_header('Transfer-Encoding','chunked');self.send_header('Connection','close');self.end_headers()
    for i in range(0,len(data),113):
     chunk=data[i:i+113];self.wfile.write(f'{len(chunk):x}\r\n'.encode()+chunk+b'\r\n');self.wfile.flush()
    self.wfile.write(b'0\r\n\r\n')
   def log_message(self,*args):pass
  server=ThreadingHTTPServer(('127.0.0.1',0),HTTP);thread=threading.Thread(target=server.serve_forever);thread.start();self.addCleanup(server.server_close);self.addCleanup(thread.join,2);self.addCleanup(server.shutdown)
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4))+frame(119,text(1,f'http://127.0.0.1:{server.server_port}/chunked.wav')));self.wait_completion();self.assertEqual(self.busbytes,struct.pack('<h',1700)*2000)
# The web API persists HA ownership as integrations bit 1 plus
# voice_pipeline_mode; it never writes voice_assistant_mode. These fixtures use
# exactly that saved shape so the daemon is tested against the real contract.
API_HA_CONFIG={'hostname':'fixture-satellite','wifi_mac':'02:00:00:00:00:01','ha_protocol':'esphome','esphome_noise_key':'','integrations':21,'voice_pipeline_mode':'home-assistant','voice_pipeline_previous_mode':'local'}
class ApiSavedHomeAssistant(Daemon):
 config_fields=API_HA_CONFIG
 def test_api_saved_ha_selection_starts_wake_turn(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams()
  for i in range(0,6400,320):a.samples(i)
  time.sleep(.1);a.wake(4000);t,b=receive(self.s);self.assertEqual(t,90,'API-saved HA mode must let a wake word start a satellite run');self.assertIn(num(1,1),b)
class ApiSavedLocalMode(Daemon):
 config_fields=dict(API_HA_CONFIG,integrations=20,voice_pipeline_mode='local')
 def test_api_saved_local_mode_never_starts_turn(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams()
  for i in range(0,6400,320):a.samples(i)
  time.sleep(.1);a.wake(4000);self.s.settimeout(.3)
  with self.assertRaises(TimeoutError):receive(self.s)
if __name__=='__main__':unittest.main()
