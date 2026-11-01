"""LED ring as a Home Assistant light (#395). Real daemon, fake ledd socket, raw
frames. HA's state is one ledd overlay (owner esphome-light); device voice
indicators (owner esphome) keep priority and HA's change is applied after them."""
import struct,time,unittest
from test_esphomed import Daemon,frame,num,receive,text,var
from test_esphomed_activity import Activity,VOICE

LIGHT_KEY=30
def fixed(f,v):return var(f<<3|5)+struct.pack('<I',v)
def flt(f,v):return var(f<<3|5)+struct.pack('<f',v)
def all_fields(b):
 out=[];i=0
 def varint():
  nonlocal i;v=0;s=0
  while True:
   x=b[i];i+=1;v|=(x&127)<<s;s+=7
   if x<128:return v
 while i<len(b):
  key=varint();f,w=key>>3,key&7
  if w==0:out.append((f,varint()))
  elif w==5:out.append((f,b[i:i+4]));i+=4
  elif w==2:n=varint();out.append((f,b[i:i+n]));i+=n
  else:raise ValueError(w)
 return out
def float_of(v):return struct.unpack('<f',v)[0]
def command(state=None,rgb=None,level=None,effect=None):
 b=fixed(1,LIGHT_KEY)
 if state is not None:b+=num(2,1)+num(3,int(state))
 if rgb is not None:b+=num(6,1)+flt(7,rgb[0])+flt(8,rgb[1])+flt(9,rgb[2])
 if level is not None:b+=num(20,1)+flt(21,level)
 if effect is not None:b+=num(18,1)+text(19,effect)
 return frame(32,b)

class LedLight(Daemon):
 def subscribe(self):
  # As Home Assistant does: list entities (LED ring light after the activity entities), then subscribe.
  self.hello();self.s.sendall(frame(11));self.listed=[receive(self.s) for _ in range(18)]
  self.s.sendall(frame(20));return self.listed
 def wait_for(self,predicate,seconds=3):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   if predicate():return True
   time.sleep(.02)
  return predicate()
 def led_calls(self):return [r for r in self.adapters['led'].calls if r['cmd']=='pattern']
 def ring_states(self,seconds=1.5):
  # LightStateResponse (24) messages, drained within the window.
  got=[];end=time.monotonic()+seconds
  while time.monotonic()<end:
   self.s.settimeout(max(.01,end-time.monotonic()))
   try:t,b=receive(self.s)
   except (TimeoutError,OSError):break
   if t==24:got.append({f:v for f,v in all_fields(b)})
  self.s.settimeout(3);return got

class LedLightEntity(LedLight):
 def test_list_entities_has_rgb_light_with_effects(self):
  listed=self.subscribe()
  self.assertEqual([t for t,_ in listed],[63,17,17,49,49,49,107,107,107,107,107,107,107,15,41,41,41,19])
  light=all_fields(listed[13][1])
  self.assertEqual(light[0],(1,b'led_ring'))
  self.assertEqual(light[1],(2,struct.pack('<I',LIGHT_KEY)))
  self.assertEqual([v for f,v in light if f==3],[b'LED ring'])
  self.assertEqual([v for f,v in light if f==12],[35],'COLOR_MODE_RGB only, brightness implied')
  self.assertEqual([v for f,v in light if f==11],[b'Solid',b'Pulse',b'Flash'])

 def test_state_on_subscribe_is_off_and_solid_white(self):
  self.subscribe()
  states=[s for s in self.ring_states(1.0)]
  self.assertEqual(len(states),1)
  st=states[0];self.assertEqual(st[1],struct.pack('<I',LIGHT_KEY));self.assertEqual(st[2],0)
  self.assertEqual(float_of(st[3]),1.0);self.assertEqual(st[11],35);self.assertEqual(st[9],b'Solid')

class LedLightCommands(LedLight):
 def test_on_colour_level_and_effect_reach_ledd_and_are_reported(self):
  self.subscribe()
  self.s.sendall(command(state=True,rgb=(1.0,0.5,0.0),level=0.5,effect='Pulse'))
  self.assertTrue(self.wait_for(lambda:any(r['args'].get('name')=='pulse' for r in self.led_calls())),'pulse overlay not sent')
  self.assertEqual(self.led_calls()[-1]['args'],{'name':'pulse','r':255,'g':128,'b':0,'brightness':50,'repeats':0,'owner':'esphome-light'})
  states=[s for s in self.ring_states(1.0) if s.get(9)==b'Pulse']
  self.assertEqual(len(states),1,'one state report for the command');self.assertEqual(states[0][2],1);self.assertEqual(float_of(states[0][3]),0.5)

 def test_off_stops_only_the_home_assistant_overlay(self):
  self.subscribe()
  self.s.sendall(command(state=True,rgb=(0,0,1.0),level=1.0,effect='Flash'))
  self.assertTrue(self.wait_for(lambda:any(r['args'].get('name')=='flash' for r in self.led_calls())))
  self.s.sendall(command(state=False))
  self.assertTrue(self.wait_for(lambda:self.led_calls()[-1]['args']=={'name':'stop','owner':'esphome-light'}),'off did not stop esphome-light')
  states=self.ring_states(1.0);self.assertEqual(states[-1][2],0)

 def test_unknown_effect_is_rejected_before_any_change(self):
  self.subscribe();self.ring_states(.3);before=len(self.led_calls())
  self.s.sendall(command(state=True,rgb=(1.0,1.0,1.0),level=1.0,effect='Chase'))
  seen=[]
  with self.assertRaises(EOFError,msg='an unknown effect closes the client'):
   for _ in range(50):seen.append(receive(self.s)[0])
  self.assertNotIn(24,seen,'a rejected command reports no state')
  time.sleep(.1);self.assertEqual(len(self.led_calls()),before)

 def test_startup_clears_a_stale_overlay_from_a_previous_process(self):
  self.assertTrue(self.wait_for(lambda:{'name':'stop','owner':'esphome-light'} in [r['args'] for r in self.led_calls()]),'no startup clear')

class LedLightPriority(Activity):
 wait_for=LedLight.wait_for;led_calls=LedLight.led_calls
 def test_device_voice_indicator_wins_and_home_assistant_state_resumes_after(self):
  self.subscribe();a=self.adapters['wake'];a.wait_streams()
  self.s.sendall(command(state=True,rgb=(1.0,0.0,0.0),level=1.0,effect='Solid'))
  self.assertTrue(self.wait_for(lambda:any(r['args'].get('owner')=='esphome-light' for r in self.adapters['led'].calls if r['cmd']=='pattern')),'HA solid overlay not sent')
  for i in range(0,6400,320):a.samples(i)
  time.sleep(.1);a.wake(4000)
  self.assertEqual(self.events(2),[(VOICE,'wake_word'),(VOICE,'listening')])
  self.assertTrue(self.wait_for(lambda:any(r['args'].get('owner')=='esphome' for r in self.led_calls())),'listening indicator not shown')
  mark=len(self.led_calls())
  self.s.sendall(command(state=False))
  time.sleep(.3)
  self.assertFalse(any(r['args']=={'name':'stop','owner':'esphome-light'} for r in self.led_calls()[mark:]),'HA changed the ring during a voice turn')
  self.s.close()
  self.assertTrue(self.wait_for(lambda:self.led_calls()[-1]['args']=={'name':'stop','owner':'esphome-light'}),'HA state not applied after the turn')
  owners=[r['args'].get('owner') for r in self.led_calls()[mark:]]
  self.assertIn('esphome',owners,'the voice indicator was not replaced or flashed by the turn end')
  self.assertEqual(owners[-1],'esphome-light')

if __name__=='__main__':unittest.main()
