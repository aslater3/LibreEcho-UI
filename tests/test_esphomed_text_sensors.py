"""Last-turn text sensors (#398): the last wake word (waked's model name) is always listed; the last
transcript and reply are listed and published only with esphome_speech_text_sensors set. When the voice
history generation changes (any history clear), all three read empty. Real daemon, fake adapters, raw frames."""
import time,unittest
from test_esphomed import Daemon,frame,receive,num,text
from test_esphomed_buttons import parse,key_of

WAKE,TRANSCRIPT,REPLY=70,71,72
BASE={'hostname':'fixture-satellite','wifi_mac':'02:00:00:00:00:01','ha_protocol':'esphome','voice_assistant_mode':1,'esphome_noise_key':''}

class TextSensors(Daemon):
 def listing(self):
  # Keys of the ListEntitiesTextSensorResponse messages (type 18), up to ListEntitiesDone (19).
  self.hello();self.s.sendall(frame(11));keys=[]
  while True:
   t,b=receive(self.s)
   if t==18:keys.append(key_of(parse(b)[2][0]))
   if t==19:return keys
 def texts(self,count,seconds=5):
  # TextSensorStateResponse (type 27) as (key, value, missing); other traffic is drained.
  got=[];end=time.monotonic()+seconds
  while len(got)<count and time.monotonic()<end:
   self.s.settimeout(.05)
   try:t,b=receive(self.s)
   except TimeoutError:continue
   if t==27:p=parse(b);got.append((key_of(p[1][0]),p[2][0].decode() if 2 in p else '',3 in p))
  self.s.settimeout(3);return got
 def turn(self):
  # Opens a voice turn as Home Assistant does: voice subscription, then a wake word on the wake stream.
  self.s.sendall(frame(89,num(1,1)+num(2,4)));a=self.adapters['wake'];a.wait_streams()
  for i in range(0,6400,320):a.samples(i)
  time.sleep(.1);a.wake(4000)
  return a

class WakeWordOnly(TextSensors):
 def test_only_wake_word_is_listed_and_published_by_default(self):
  self.assertEqual(self.listing(),[WAKE])
  self.s.sendall(frame(20));self.assertEqual(self.texts(1),[(WAKE,'',True)])
  self.adapters['wake'].wait_streams();self.adapters['wake'].wake(100)
  self.assertEqual(self.texts(1),[(WAKE,'Alexa',False)])

 def test_speech_events_publish_nothing_by_default(self):
  self.listing();self.s.sendall(frame(20));self.texts(1)
  self.turn();self.assertEqual(self.texts(1),[(WAKE,'Alexa',False)])
  self.s.sendall(frame(92,num(1,4)+text(2,text(1,'text')+text(2,'turn on the lamp'))))
  self.s.sendall(frame(92,num(1,7)+text(2,text(1,'text')+text(2,'The lamp is on.'))))
  self.assertEqual(self.texts(3,seconds=1.5),[],'transcript and reply stay off without the owner setting')

class SpeechText(TextSensors):
 config_fields=dict(BASE,esphome_speech_text_sensors=True)

 def test_speech_sensors_are_listed_and_follow_the_turn(self):
  self.assertEqual(self.listing(),[WAKE,TRANSCRIPT,REPLY])
  self.s.sendall(frame(20));self.assertEqual(self.texts(3),[(WAKE,'',True),(TRANSCRIPT,'',True),(REPLY,'',True)])
  self.turn();self.assertEqual(self.texts(1),[(WAKE,'Alexa',False)])
  self.s.sendall(frame(92,num(1,4)+text(2,text(1,'text')+text(2,'turn on the lamp'))))
  self.assertEqual(self.texts(1),[(TRANSCRIPT,'turn on the lamp',False)])
  self.s.sendall(frame(92,num(1,7)+text(2,text(1,'text')+text(2,'The lamp is on.'))))
  self.assertEqual(self.texts(1),[(REPLY,'The lamp is on.',False)])

 def test_history_clear_empties_all_last_turn_sensors(self):
  agent=self.adapters['agent'];agent.generation=1;time.sleep(2.5)  # first poll records the generation
  self.listing();self.s.sendall(frame(20));self.texts(3);self.turn();self.texts(1)
  self.s.sendall(frame(92,num(1,4)+text(2,text(1,'text')+text(2,'turn on the lamp'))))
  self.assertEqual(self.texts(1),[(TRANSCRIPT,'turn on the lamp',False)])
  agent.generation=2  # Only values that held something publish a change; the unset reply stays silent.
  self.assertEqual(self.texts(3,seconds=4),[(WAKE,'',True),(TRANSCRIPT,'',True)])
  self.assertEqual(self.texts(1,seconds=1),[])

if __name__=='__main__':unittest.main()
