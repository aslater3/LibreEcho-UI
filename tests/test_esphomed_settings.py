"""Settings as Home Assistant entities (#396): wake word on/off switch and the volume,
wake sensitivity and LED brightness numbers (config category). Real daemon, fake
audio/wake/led sockets, raw frames. Commands reach the same daemon commands the web
API uses; values are read back from those daemons, and sensitivity persists to the web config."""
import json,struct,time,unittest
from test_esphomed import Daemon,frame,num,receive
from test_esphomed_led_light import all_fields,float_of,flt,fixed

VOLUME,SENSITIVITY,LED_BRIGHTNESS=40,41,42
WAKE_SWITCH=43

def number_command(key,value):return frame(51,fixed(1,key)+flt(2,value))
def switch_command(key,on):return frame(33,fixed(1,key)+num(2,int(on)))

class Settings(Daemon):
 def setUp(self):
  super().setUp()
  self.adapters['led'].track=self.adapters['wake'].track=self.adapters['audio'].track=True
  self.adapters['led'].status={'brightness':40};self.adapters['wake'].status={'sensitivity':55}
 def subscribe(self):
  self.hello();self.s.sendall(frame(11));self.listed=[receive(self.s) for _ in range(21)];self.s.sendall(frame(20))
 def calls(self,kind,cmd):return [r for r in self.adapters[kind].calls if r['cmd']==cmd]
 def wait_call(self,kind,cmd,seconds=3):
  end=time.monotonic()+seconds
  while time.monotonic()<end:
   if self.calls(kind,cmd):return self.calls(kind,cmd)[-1]['args']
   time.sleep(.02)
  self.fail('%s %s not sent'%(kind,cmd))
 def read_until(self,predicate,seconds=5):
  # Collects every daemon message until predicate(type, fields) is true or the window ends.
  got=[];end=time.monotonic()+seconds
  while time.monotonic()<end:
   self.s.settimeout(max(.05,end-time.monotonic()))
   try:t,b=receive(self.s)
   except (TimeoutError,OSError):break
   f={k:v for k,v in all_fields(b)} if t in (26,50) else {}
   got.append((t,f))
   if predicate(t,f):break
  self.s.settimeout(3);return got
 def number_states(self,seconds=5,want=None):
  # Latest NumberStateResponse (50) value per key, read until every wanted key has the wanted value.
  latest={}
  def done(t,f):
   if t==50:latest[struct.unpack('<I',f[1])[0]]=float_of(f[2]) if 2 in f else None
   return want is not None and all(latest.get(k)==v for k,v in want.items())
  self.read_until(done,seconds);return latest

class SettingsEntities(Settings):
 def test_entities_are_listed_in_config_category(self):
  self.subscribe()
  self.assertEqual([t for t,_ in self.listed][4:8],[17,49,49,49])
  sw=all_fields(self.listed[4][1])
  self.assertIn((1,b'wake_word'),sw);self.assertIn((2,struct.pack('<I',WAKE_SWITCH)),sw);self.assertIn((7,1),sw)
  for idx,(object_id,key) in zip((5,6,7),((b'volume',VOLUME),(b'wake_word_sensitivity',SENSITIVITY),(b'led_brightness',LED_BRIGHTNESS))):
   f=all_fields(self.listed[idx][1])
   self.assertIn((1,object_id),f);self.assertIn((2,struct.pack('<I',key)),f);self.assertIn((10,1),f)  # entity_category CONFIG
   # ListEntitiesNumberResponse: min_value=6, max_value=7, step=8 (not 8/9/10).
   self.assertEqual(float_of([v for k,v in f if k==6][0]),0.0);self.assertEqual(float_of([v for k,v in f if k==7][0]),100.0)
   self.assertEqual(float_of([v for k,v in f if k==8][0]),1.0);self.assertIn((12,2),f)  # mode SLIDER

 def test_states_are_read_back_from_the_daemons(self):
  self.adapters['audio'].volume=30
  self.subscribe()
  latest=self.number_states(5,want={VOLUME:30.0,SENSITIVITY:55.0,LED_BRIGHTNESS:40.0})
  # Key 50 is the separate reply volume number (#409): it follows the master volume and is covered by test_esphomed_aio.
  self.assertEqual({k:v for k,v in latest.items() if k!=50},{VOLUME:30.0,SENSITIVITY:55.0,LED_BRIGHTNESS:40.0})

 def test_unchanged_settings_are_not_repeated_on_status_polls(self):
  self.subscribe();self.number_states(1)
  # Status polls run every 2 s with the same values: no settings entity may be re-sent.
  got=self.read_until(lambda t,f:False,seconds=4.5)
  self.assertEqual([t for t,_ in got if t==50],[],'unchanged number states were repeated')
 def test_web_change_reaches_home_assistant_on_the_next_poll(self):
  self.subscribe();self.number_states(5,want={LED_BRIGHTNESS:40.0})
  self.adapters['led'].status['brightness']=20
  self.assertEqual(self.number_states(5,want={LED_BRIGHTNESS:20.0}).get(LED_BRIGHTNESS),20.0,'web change not reported')

class SettingsCommands(Settings):
 def test_number_commands_reach_the_web_api_setters_and_read_back(self):
  self.subscribe();self.number_states(5)
  self.s.sendall(number_command(LED_BRIGHTNESS,75));self.assertEqual(self.wait_call('led','set_brightness'),{'brightness':75})
  self.s.sendall(number_command(SENSITIVITY,60));self.assertEqual(self.wait_call('wake','set_sensitivity'),{'sensitivity':60})
  self.s.sendall(number_command(VOLUME,30));self.assertEqual(self.wait_call('audio','set_volume'),{'volume':30})
  self.assertEqual(self.wait_call('led','activity'),{'source':'volume'},'web volume activity not sent')
  latest=self.number_states(5,want={LED_BRIGHTNESS:75.0,SENSITIVITY:60.0,VOLUME:30.0})
  self.assertEqual({k:v for k,v in latest.items() if k!=50},{LED_BRIGHTNESS:75.0,SENSITIVITY:60.0,VOLUME:30.0})

 def test_wake_switch_turns_wake_word_off_and_on(self):
  self.subscribe();self.number_states(1)
  self.s.sendall(switch_command(WAKE_SWITCH,False))
  got=self.read_until(lambda t,f:t==26 and f.get(1)==struct.pack('<I',WAKE_SWITCH))
  self.assertEqual([f[2] for t,f in got if t==26 and f.get(1)==struct.pack('<I',WAKE_SWITCH)][-1],0)
  self.assertEqual(json.loads(self.config.read_text())['esphome_active_wake_word'],'')
  self.s.sendall(switch_command(WAKE_SWITCH,True))
  self.assertEqual(self.wait_call('wake','set_word'),{'word':'Alexa'})
  got=self.read_until(lambda t,f:t==26 and f.get(1)==struct.pack('<I',WAKE_SWITCH) and f.get(2)==1)
  self.assertTrue(any(t==26 and f.get(2)==1 for t,f in got),'wake switch on not reported')
  self.assertEqual(json.loads(self.config.read_text())['esphome_active_wake_word'],'alexa_v0.1')

 def test_out_of_range_number_is_rejected_before_any_change(self):
  self.subscribe();self.number_states(1)
  self.s.sendall(number_command(LED_BRIGHTNESS,150))
  with self.assertRaises(EOFError,msg='an out-of-range value closes the client'):
   for _ in range(50):receive(self.s)
  time.sleep(.1);self.assertEqual(self.calls('led','set_brightness'),[])

 def test_fractional_number_is_rejected_before_any_change(self):
  self.subscribe();self.number_states(1)
  self.s.sendall(number_command(SENSITIVITY,33.5))
  with self.assertRaises(EOFError,msg='a fractional value closes the client'):
   for _ in range(50):receive(self.s)
  time.sleep(.1);self.assertEqual(self.calls('wake','set_sensitivity'),[])

class SensitivityPersists(Settings):
 config_fields={'hostname':'fixture-satellite','wifi_mac':'02:00:00:00:00:01','ha_protocol':'esphome','voice_assistant_mode':1,'esphome_noise_key':'','wake_sensitivity':30}
 def test_sensitivity_replaces_the_saved_value(self):
  self.subscribe();self.number_states(1)
  self.s.sendall(number_command(SENSITIVITY,60));self.wait_call('wake','set_sensitivity')
  end=time.monotonic()+3;saved=None
  while time.monotonic()<end:
   saved=json.loads(self.config.read_text()).get('wake_sensitivity')
   if saved==60:break
   time.sleep(.05)
  self.assertEqual(saved,60,'sensitivity not saved to the web config')
  self.assertEqual(json.loads(self.config.read_text())['hostname'],'fixture-satellite','unrelated fields must survive')

class SensitivityInserted(Settings):
 def test_sensitivity_is_inserted_when_absent(self):
  self.subscribe();self.number_states(1)
  self.s.sendall(number_command(SENSITIVITY,44));self.wait_call('wake','set_sensitivity')
  end=time.monotonic()+3;saved=None
  while time.monotonic()<end:
   saved=json.loads(self.config.read_text()).get('wake_sensitivity')
   if saved==44:break
   time.sleep(.05)
  self.assertEqual(saved,44,'sensitivity not inserted into the web config')

if __name__=='__main__':unittest.main()
