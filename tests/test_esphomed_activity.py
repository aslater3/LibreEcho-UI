"""Device activity as HA event entities (#393): voice steps, playback and the
timer events Home Assistant sends. Real daemon with fake adapters; raw frames,
no pinned client needed. Payloads carry only event types, never transcript,
TTS/media URL or timer text."""
import contextlib,io,os,struct,threading,time,unittest,wave
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from test_esphomed import Daemon,frame,receive,num,text
from test_esphomed_buttons import parse,key_of

VOICE,PLAYBACK,TIMER=20,21,22

class Activity(Daemon):
 def setUp(self):
  super().setUp();self.servers=[]
 def tearDown(self):
  for server in self.servers:server.shutdown();server.server_close()
  super().tearDown()
 def subscribe(self):
  # As Home Assistant does: list entities, subscribe to states and to the voice pipeline.
  self.hello();self.s.sendall(frame(11));listed=[receive(self.s) for _ in range(10)]
  self.s.sendall(frame(20));self.s.sendall(frame(89,num(1,1)+num(2,4)));time.sleep(.1);return listed
 def events(self,count,seconds=5):
  # Event entity messages (type 108) in arrival order; other traffic is skipped but drained.
  got=[];end=time.monotonic()+seconds
  while len(got)<count and time.monotonic()<end:
   with contextlib.suppress(BlockingIOError):self.busbytes+=os.read(self.busfd,65536)
   self.s.settimeout(.05)
   try:t,b=receive(self.s)
   except TimeoutError:continue
   if t==108:p=parse(b);got.append((key_of(p[1][0]),p[2][0].decode()))
  self.s.settimeout(3);return got
 def serve(self,seconds):
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1200)*int(48000*seconds))
  data=wav.getvalue()
  class HTTP(BaseHTTPRequestHandler):
   def do_GET(self):self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
   def log_message(self,*a):pass
  server=ThreadingHTTPServer(('127.0.0.1',0),HTTP);self.servers.append(server);threading.Thread(target=server.serve_forever,daemon=True).start()
  return f'http://127.0.0.1:{server.server_port}/reply.wav'

class ActivityEntities(Activity):
 def test_activity_entities_list_their_event_types(self):
  listed=self.subscribe()
  self.assertEqual([t for t,_ in listed],[63,17,107,107,107,107,107,107,107,19])
  entities=[parse(b) for t,b in listed if t==107][4:]
  self.assertEqual([(p[1][0],key_of(p[2][0]),p[3][0]) for p in entities],[
   (b'voice_assistant',20,b'Voice assistant'),(b'playback',21,b'Playback'),(b'timer',22,b'Timer')])
  self.assertEqual([p[9] for p in entities],[
   [b'wake_word',b'listening',b'thinking',b'replying',b'finished',b'stopped'],
   [b'started',b'stopped'],
   [b'started',b'updated',b'cancelled',b'finished']])

class ActivityEvents(Activity):
 def test_voice_turn_then_reply_reports_each_step_in_order(self):
  self.subscribe();a=self.adapters['wake'];a.wait_streams()
  for i in range(0,6400,320):a.samples(i)
  time.sleep(.1);a.wake(4000)
  self.assertEqual(self.events(2),[(VOICE,'wake_word'),(VOICE,'listening')])
  self.s.sendall(frame(91));self.s.sendall(frame(92,num(1,12)))
  self.assertEqual(self.events(1),[(VOICE,'thinking')])
  url=self.serve(0.1);event=num(1,8)+text(2,text(1,'url')+text(2,url))
  self.s.sendall(frame(92,event)+frame(92,num(1,2)))
  self.assertEqual(self.events(4),[(VOICE,'replying'),(PLAYBACK,'started'),(PLAYBACK,'stopped'),(VOICE,'finished')])

 def test_announcement_reports_playback_and_finish(self):
  self.subscribe();url=self.serve(0.1)
  self.s.sendall(frame(119,text(1,url)))
  self.assertEqual(self.events(3),[(PLAYBACK,'started'),(PLAYBACK,'stopped'),(VOICE,'finished')])

 def test_home_assistant_timer_events_map_to_timer_entity(self):
  self.subscribe()
  for kind in range(4):self.s.sendall(frame(115,num(1,kind)+text(2,'t1')+text(3,'tea')+num(4,60)+num(5,45)+num(6,1)))
  self.assertEqual(self.events(4),[(TIMER,'started'),(TIMER,'updated'),(TIMER,'cancelled'),(TIMER,'finished')])

 def test_unsubscribed_client_gets_no_activity_events(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));time.sleep(.1)
  for kind in range(4):self.s.sendall(frame(115,num(1,kind)+text(2,'t1')+text(3,'tea')+num(4,60)+num(5,45)+num(6,1)))
  self.assertEqual(self.events(1,seconds=1),[])

if __name__=='__main__':unittest.main()
