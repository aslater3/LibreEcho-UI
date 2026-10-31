"""Ambient light (#392): the TSL2540 lux as an HA illuminance sensor.
Real daemon, private fixture files only; --light-sensor points at a temp file
standing in for the sysfs als_lux node."""
import os,pathlib,socket,struct,tempfile,time,unittest
from test_esphomed import Daemon,frame,num,receive
def fields(b):
 out={};i=0
 def var():
  nonlocal i;v=0;s=0
  while True:
   x=b[i];i+=1;v|=(x&127)<<s;s+=7
   if x<128:return v
 while i<len(b):
  key=var();f,w=key>>3,key&7
  if w==0:out[f]=var()
  elif w==5:out[f]=b[i:i+4];i+=4
  elif w==2:n=var();out[f]=b[i:i+n];i+=n
  else:raise ValueError(w)
 return out
def lux_of(b):
 f=fields(b);return None if f.get(3) else struct.unpack('<f',f.get(2,b'\0\0\0\0'))[0]
class LightSensor(Daemon):
 def setUp(self):
  self.light_dir=tempfile.TemporaryDirectory(prefix='esphomed-light-');self.lux=pathlib.Path(self.light_dir.name)/'als_lux';self.set_lux(120)
  self.extra_args=['--light-sensor',str(self.lux)];super().setUp()
 def tearDown(self):
  super().tearDown();self.light_dir.cleanup()
 def set_lux(self,v):
  # Atomic, like a sysfs read: never let the daemon see a half-written file.
  tmp=self.lux.with_suffix('.tmp');tmp.write_text('%d\n'%v);os.replace(tmp,self.lux)
 def subscribe(self):
  self.hello();self.s.sendall(frame(20))
  return self.light_states(1.5,stop_after=1)
 def light_states(self,seconds,stop_after=None):
  got=[];end=time.monotonic()+seconds
  while time.monotonic()<end and (stop_after is None or len(got)<stop_after):
   self.s.settimeout(max(.01,end-time.monotonic()))
   try:t,b=receive(self.s)
   except (TimeoutError,socket.timeout):break
   if t==25:
    f=fields(b);self.assertEqual(struct.unpack('<I',f[1])[0],3);got.append(lux_of(b))
  self.s.settimeout(3);return got
 def test_list_entities_has_illuminance_sensor(self):
  self.hello();self.s.sendall(frame(11));msgs=[receive(self.s) for _ in range(4)]
  self.assertEqual([t for t,_ in msgs],[63,17,16,19])
  f=fields(msgs[2][1])
  self.assertEqual(f[1],b'illuminance');self.assertEqual(struct.unpack('<I',f[2])[0],3);self.assertEqual(f[3],b'Illuminance')
  self.assertEqual(f[6],b'lx');self.assertEqual(f[9],b'illuminance');self.assertEqual(f[10],1,'STATE_CLASS_MEASUREMENT')
 def test_state_on_subscribe_and_on_change(self):
  self.assertEqual(self.subscribe(),[120.0])
  self.set_lux(400);self.assertEqual(self.light_states(2.5,stop_after=1),[400.0])
 def test_updates_throttled_to_one_per_second(self):
  self.subscribe();got=[];end=time.monotonic()+3;v=500
  while time.monotonic()<end:
   v=1000 if v==500 else 500;self.set_lux(v);got+=self.light_states(.05)
  got+=self.light_states(.3)
  self.assertGreaterEqual(len(got),1);self.assertLessEqual(len(got),4,'more than ~1 update/s: %r'%got)
 def test_small_drift_is_not_published(self):
  self.subscribe();self.set_lux(122)
  self.assertEqual(self.light_states(2.2),[],'a 2% change is noise, not a new reading')
 def test_unreadable_sensor_reports_missing_state(self):
  self.subscribe();self.lux.unlink();self.assertEqual(self.light_states(2.5,stop_after=1),[None])
  self.set_lux(80);self.assertEqual(self.light_states(2.5,stop_after=1),[80.0])
class NoLightSensor(Daemon):
 def test_no_sensor_no_entity_and_no_state(self):
  # The private fixture has no /sys/bus/i2c TSL2540 node, as on a board without one.
  self.hello();self.s.sendall(frame(11));self.assertEqual([receive(self.s)[0] for _ in range(3)],[63,17,19])
  self.s.sendall(frame(20));end=time.monotonic()+1.5;types=[]
  while time.monotonic()<end:
   self.s.settimeout(max(.01,end-time.monotonic()))
   try:types.append(receive(self.s)[0])
   except (TimeoutError,socket.timeout):break
  self.assertNotIn(25,types)
if __name__=='__main__':unittest.main()
