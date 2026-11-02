"""Pinned aioesphomeapi 46.2.0 view of the #398 last-turn text sensors (no install fallback)."""
import asyncio,time,unittest
from test_esphomed_aio import APIClient
from test_esphomed_text_sensors import SpeechText
from aioesphomeapi.model import TextSensorInfo,TextSensorState

class PinnedText(SpeechText):
 def test_pinned_client_sees_last_turn_text_sensors(self):
  self.s.close();time.sleep(.05)
  async def run():
   client=APIClient('127.0.0.1',self.port,provide_time=False)
   try:
    await client.connect(login=True);entities,_=await client.list_entities_services()
    texts=[e for e in entities if isinstance(e,TextSensorInfo)]
    self.assertEqual([(e.key,e.object_id,e.name,e.icon) for e in texts],[
     (70,'last_wake_word','Last wake word','mdi:account-voice'),
     (71,'last_transcript','Last transcript','mdi:microphone-message'),
     (72,'last_reply','Last reply','mdi:message-reply-text')])
    states=[];client.subscribe_states(lambda s:states.append(s) if isinstance(s,TextSensorState) else None)
    end=time.monotonic()+2
    while len(states)<3 and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertEqual([(s.key,s.state,s.missing_state) for s in states],[(70,'',True),(71,'',True),(72,'',True)])
   finally:await client.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),10))
if __name__=='__main__':unittest.main(defaultTest='PinnedText.test_pinned_client_sees_last_turn_text_sensors')
