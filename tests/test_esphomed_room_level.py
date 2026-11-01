"""Room level sensor (#399): a dBFS sensor from the 16 kHz stream esphomed already reads for the ring, with no
second capture. Off unless esphome_room_level_sensor is set; muted input reads as missing; publishes are rate
limited and only move on a change of at least 2 dB. Real daemon, fake adapters, raw frames."""
import struct,time,unittest
from test_esphomed import Daemon,frame,receive
from test_esphomed_buttons import varint,key_of

LEVEL=73
SENSOR_LIST,SENSOR_STATE=16,25
BASE={'hostname':'fixture-satellite','wifi_mac':'02:00:00:00:00:01','ha_protocol':'esphome','voice_assistant_mode':1,'esphome_noise_key':''}

def fields(b):
 # Minimal protobuf reader: varint, fixed32 (raw bytes) and length-delimited (raw bytes) fields, repeats kept.
 out={};i=0
 while i<len(b):
  k,i=varint(b,i);f,w=k>>3,k&7
  if w==0:v,i=varint(b,i)
  elif w==5:v=b[i:i+4];i+=4
  elif w==2:n,i=varint(b,i);v=b[i:i+n];i+=n
  else:raise ValueError('wire type %d'%w)
  out.setdefault(f,[]).append(v)
 return out

class RoomLevel(Daemon):
 config_fields=dict(BASE,esphome_room_level_sensor=True)

 def listing(self):
  # Keys of ListEntitiesSensorResponse messages (type 16) up to ListEntitiesDone (19).
  self.hello();self.s.sendall(frame(11));keys=[]
  while True:
   t,b=receive(self.s)
   if t==SENSOR_LIST:keys.append(key_of(fields(b)[2][0]))
   if t==19:return keys

 def states(self,count,seconds=5):
  # SensorStateResponse (type 25) as (key, state or None when missing); other traffic is drained.
  got=[];end=time.monotonic()+seconds
  while len(got)<count and time.monotonic()<end:
   self.s.settimeout(.05)
   try:t,b=receive(self.s)
   except TimeoutError:continue
   if t==SENSOR_STATE:
    p=fields(b);key=key_of(p[1][0])
    missing=bool(p.get(3,[0])[0])
    got.append((key,None if missing else struct.unpack('<f',p[2][0])[0]))
  self.s.settimeout(3);return got

 def feed(self,value,seconds):
  # Real-time 1280-sample frames (80 ms each) of constant `value`, as the stream delivers them; a window
  # completes every ~1 s, so the level stays fresh while the test waits for a publish.
  a=self.adapters['wake'];a.wait_streams();stream=a.streams['stream_audio'];end=time.monotonic()+seconds
  while time.monotonic()<end:
   first=self.first;self.first+=1280
   stream.sendall(struct.pack('<IHHQII',0x3153564c,1,0,first,1280,0)+struct.pack('<1280h',*([value]*1280)))
   time.sleep(.08)

 def setUp(self):
  super().setUp();self.first=0

class RoomLevelOff(Daemon):
 def test_not_listed_by_default(self):
  self.hello();self.s.sendall(frame(11));keys=[]
  while True:
   t,b=receive(self.s)
   if t==SENSOR_LIST:keys.append(key_of(fields(b)[2][0]))
   if t==19:break
  self.assertNotIn(LEVEL,keys)

class RoomLevelOn(RoomLevel):
 def test_listed_and_missing_until_the_first_window(self):
  self.assertEqual(self.listing(),[LEVEL])
  self.s.sendall(frame(20));self.assertEqual(self.states(1),[(LEVEL,None)])

 def test_quiet_then_loud_publishes_dbfs_and_ignores_small_changes(self):
  self.listing();self.s.sendall(frame(20));self.states(1)
  self.feed(10,1.5)  # RMS 10 of full scale 32768: about -70.3 dBFS, published at once
  got=self.states(1,seconds=1)
  self.assertEqual(len(got),1);self.assertEqual(got[0][0],LEVEL);self.assertAlmostEqual(got[0][1],-70.3,delta=0.2)
  self.feed(3000,6)  # about -20.8 dBFS; the 5 s minimum interval after the first publish has passed
  got=self.states(1,seconds=1)
  self.assertEqual(len(got),1);self.assertAlmostEqual(got[0][1],-20.8,delta=0.2)
  self.feed(3050,6)  # about 0.15 dB higher: under the 2 dB step, so nothing is published
  self.assertEqual(self.states(1,seconds=1),[])

 def test_muted_input_reads_missing(self):
  self.listing();self.s.sendall(frame(20));self.states(1)
  self.feed(3000,2.5);self.assertEqual(len(self.states(1,seconds=1)),1)
  self.privacy.write_text('1\n')
  self.feed(3000,4)  # the stream keeps flowing, but muted audio is never measured
  self.assertEqual(self.states(1,seconds=1),[(LEVEL,None)])

if __name__=='__main__':unittest.main()
