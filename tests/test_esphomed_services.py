"""User-defined ESPHome services (#397): announce, play_sound and set_ring. Real daemon, fake
audiod/ledd sockets, raw frames. Each call reaches the existing daemon command (audiod speak,
audiod sample, ledd pattern); malformed arguments close that client before any change."""
import struct,time,unittest
from test_esphomed import Daemon,frame,num,receive,text
from test_esphomed_led_light import all_fields,fixed

ANNOUNCE,PLAY_SOUND,SET_RING=60,61,62
OWNER='esphome-action'

def zigzag(n):return n*2 if n>=0 else -n*2-1
def call(key,*args):
 # ExecuteServiceRequest: fixed32 key, then one ExecuteServiceArgument (field 2) per value.
 body=fixed(1,key)
 for a in args:body+=text(2,a)
 return frame(42,body)
def string_arg(value):return text(4,value)
def int_arg(value):return num(5,zigzag(value))

class Services(Daemon):
 def subscribe(self):
  # As Home Assistant does: list entities (3 services before ListEntitiesDone), then subscribe.
  self.hello();self.s.sendall(frame(11));self.listed=[receive(self.s) for _ in range(18)];self.s.sendall(frame(20))
 def calls(self,kind,cmd):return [r for r in self.adapters[kind].calls if r['cmd']==cmd]
 def ring_calls(self,name=None):
  # Overlay writes owned by the service (the startup stop is one of them).
  return [r['args'] for r in self.calls('led','pattern') if r['args'].get('owner')==OWNER and (name is None or r['args'].get('name')==name)]
 def last_args(self,kind,cmd):
  got=self.calls(kind,cmd);return got[-1]['args'] if got else None
 def wait_for(self,check,seconds=3):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   value=check()
   if value:return value
   time.sleep(.02)
  self.fail('expected call not seen')
 def assert_closed(self):
  with self.assertRaises(EOFError,msg='malformed call must close the client'):
   for _ in range(50):receive(self.s)
 def fresh_client(self):
  # Each rejected call closes its own client; the daemon keeps serving, so the next case reconnects.
  self.s=self.connect();self.subscribe()

class ServiceList(Services):
 def test_services_are_listed_before_list_entities_done(self):
  self.subscribe()
  self.assertEqual([t for t,_ in self.listed][-4:],[41,41,41,19])
  svc=[all_fields(b) for t,b in self.listed[-4:-1]]
  self.assertEqual([dict(f)[1] for f in svc],[b'announce',b'play_sound',b'set_ring'])
  self.assertEqual([struct.unpack('<I',dict(f)[2])[0] for f in svc],[ANNOUNCE,PLAY_SOUND,SET_RING])
  announce=[all_fields(v) for k,v in svc[0] if k==3]
  self.assertEqual([dict(a)[1] for a in announce],[b'text'])
  self.assertEqual([dict(a)[2] for a in announce],[3],'text is a STRING argument')
  ring=[all_fields(v) for k,v in svc[2] if k==3]
  self.assertEqual([dict(a)[1] for a in ring],[b'colour',b'effect',b'duration'])
  self.assertEqual([dict(a)[2] for a in ring],[3,3,1],'duration is an INT argument')

class Announce(Services):
 def test_announce_speaks_text_through_audiod(self):
  self.subscribe();self.s.sendall(call(ANNOUNCE,string_arg('Dinner is ready "now"')))
  self.assertEqual(self.wait_for(lambda:self.last_args('audio','speak')),{'text':'Dinner is ready "now"'})
 def test_malformed_text_closes_client_without_speaking(self):
  for bad in ('','a'*201,'hi\nthere'):
   self.fresh_client();self.s.sendall(call(ANNOUNCE,string_arg(bad)));self.assert_closed()
  time.sleep(.1);self.assertEqual(self.calls('audio','speak'),[])

class PlaySound(Services):
 def test_play_sound_plays_the_bundled_sample(self):
  self.subscribe();self.s.sendall(call(PLAY_SOUND,string_arg('action-2')))
  self.assertEqual(self.wait_for(lambda:self.last_args('audio','sample')),{'name':'action-2'})
 def test_unsafe_sound_name_closes_client_without_playing(self):
  for bad in ('../action-1','Action-1','action-1.raw','','a'*49):
   self.fresh_client();self.s.sendall(call(PLAY_SOUND,string_arg(bad)));self.assert_closed()
  time.sleep(.1);self.assertEqual(self.calls('audio','sample'),[])

class SetRing(Services):
 def test_set_ring_sets_the_overlay_then_stops_its_own_owner(self):
  self.subscribe();before=len(self.ring_calls('stop'))
  self.s.sendall(call(SET_RING,string_arg('#ff8000'),string_arg('Pulse'),int_arg(1)))
  self.assertEqual(self.wait_for(lambda:(self.ring_calls('pulse') or [None])[-1]),
                   {'name':'pulse','r':255,'g':128,'b':0,'brightness':100,'repeats':0,'owner':OWNER})
  # The stop must be a new one sent after the duration, not the startup stop.
  self.assertEqual(self.wait_for(lambda:len(self.ring_calls('stop'))>before,seconds=4),True,'the ring must end after its duration')
  self.assertEqual(self.ring_calls('stop')[-1],{'name':'stop','owner':OWNER})
 def test_set_ring_clears_any_leftover_overlay_on_startup(self):
  self.subscribe()
  self.assertIn({'name':'stop','owner':OWNER},self.ring_calls('stop'),'a leftover overlay from before a restart must be stopped')
 def test_invalid_ring_arguments_close_client_without_any_led_write(self):
  cases=[
   (string_arg('red'),string_arg('Solid'),int_arg(1)),
   (string_arg('#ff800'),string_arg('Solid'),int_arg(1)),
   (string_arg('#gg0000'),string_arg('Solid'),int_arg(1)),
   (string_arg('#ff8000'),string_arg('Wobble'),int_arg(1)),
   (string_arg('#ff8000'),string_arg('Solid'),int_arg(0)),
   (string_arg('#ff8000'),string_arg('Solid'),int_arg(301)),
   (string_arg('#ff8000'),string_arg('Solid')),
  ]
  for args in cases:
   self.fresh_client();self.s.sendall(call(SET_RING,*args));self.assert_closed()
  time.sleep(.1);self.assertEqual([a for a in self.ring_calls() if a['name']!='stop'],[],'a malformed ring call must not reach ledd')
 def test_unknown_service_key_closes_client(self):
  self.subscribe();self.s.sendall(call(99,string_arg('x')));self.assert_closed()

if __name__=='__main__':unittest.main()
