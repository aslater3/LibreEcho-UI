"""Hardware buttons as HA event entities (#394). Real daemon; a datagram on
--button-socket stands in for buttond, which sends one "<button> <gesture>"
datagram per gesture. Raw ESPHome frames, no pinned client needed."""
import os,shutil,socket,tempfile,time,unittest
from test_esphomed import Daemon,frame,receive

def varint(b,i):
 v=0;s=0
 while True:
  x=b[i];i+=1;v|=(x&127)<<s;s+=7
  if x<128:return v,i

def parse(b):
 # Repeated fields keep every value, unlike the last-wins helper in test_esphomed_light.
 out={};i=0
 while i<len(b):
  key,i=varint(b,i);f,w=key>>3,key&7
  if w==0:v,i=varint(b,i)
  elif w==5:v=b[i:i+4];i+=4
  elif w==2:n,i=varint(b,i);v=b[i:i+n];i+=n
  else:raise ValueError(w)
  out.setdefault(f,[]).append(v)
 return out

def key_of(raw):
 return int.from_bytes(raw,'little')

class ButtonEvents(Daemon):
 def setUp(self):
  self.button_dir=tempfile.mkdtemp(prefix='esphomed-buttons-')
  self.button_path=os.path.join(self.button_dir,'buttons.sock')
  self.extra_args=['--button-socket',self.button_path];super().setUp()
  # The daemon binds the socket after its TCP listener, so wait for the path.
  end=time.monotonic()+3
  while not os.path.exists(self.button_path) and time.monotonic()<end:time.sleep(.01)
  self.assertTrue(os.path.exists(self.button_path),'button socket was not bound')
  self.sender=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
 def tearDown(self):
  try:
   self.sender.close();super().tearDown()
   self.assertFalse(os.path.exists(self.button_path),'button socket is removed at exit')
  finally:
   shutil.rmtree(self.button_dir,ignore_errors=True)
 def drain(self,seconds=0.4):
  end=time.monotonic()+seconds;got=[]
  while time.monotonic()<end:
   self.s.settimeout(max(.01,end-time.monotonic()))
   try:got.append(receive(self.s))
   except (TimeoutError,socket.timeout):break
  self.s.settimeout(3);return got
 def subscribe(self):
  # As Home Assistant does: list entities, then subscribe to states.
  self.hello();self.s.sendall(frame(11))
  listed=[receive(self.s) for _ in range(7)]
  self.s.sendall(frame(20));self.drain()
  return listed
 def events_after(self,datagram):
  self.sender.sendto(datagram.encode(),self.button_path)
  return [(key_of(f[1][0]),f[2][0]) for t,b in self.drain() if t==108 for f in [parse(b)]]

class ButtonEntities(ButtonEvents):
 def test_one_event_entity_per_button_lists_all_gestures(self):
  listed=self.subscribe()
  self.assertEqual([t for t,_ in listed],[63,17,107,107,107,107,19])
  buttons=[parse(b) for t,b in listed if t==107]
  self.assertEqual([(p[1][0],key_of(p[2][0]),p[3][0]) for p in buttons],[
   (b'volume_up',10,b'Volume up button'),(b'volume_down',11,b'Volume down button'),
   (b'microphone_mute',12,b'Microphone mute button'),(b'action',13,b'Action button')])
  for p in buttons:self.assertEqual(p[9],[b'press',b'long_press',b'double_press'])

class ButtonGestures(ButtonEvents):
 def test_press_reaches_subscribed_client_with_its_key(self):
  self.subscribe()
  self.assertEqual(self.events_after('volume_up press'),[(10,b'press')])

 def test_each_button_and_gesture_maps_to_its_entity(self):
  self.subscribe()
  for datagram,key,gesture in [('volume_down long_press',11,b'long_press'),
                               ('microphone_mute double_press',12,b'double_press'),
                               ('action press',13,b'press'),
                               ('action long_press',13,b'long_press')]:
   self.assertEqual(self.events_after(datagram),[(key,gesture)],datagram)

 def test_malformed_datagrams_are_dropped_and_daemon_keeps_running(self):
  self.subscribe()
  bad=['','volume_up','volume_up  press','volume_up press extra','volume_up pressed',
       'nope press','volume_up nope',' press','volume_up '+'x'*60]
  for datagram in bad:
   self.assertEqual(self.events_after(datagram),[],repr(datagram[:40]))
  self.assertEqual(self.events_after('volume_up press'),[(10,b'press')])

 def test_no_event_until_subscribed(self):
  self.hello();self.s.sendall(frame(11))
  for _ in range(7):receive(self.s)
  self.assertEqual(self.events_after('volume_up press'),[])

if __name__=='__main__':unittest.main()
