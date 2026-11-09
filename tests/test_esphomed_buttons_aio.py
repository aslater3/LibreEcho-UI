"""Pinned aioesphomeapi 46.2.0 view of the #394 button event entities.
Runs against an ALREADY INSTALLED 46.2.0 environment; no install fallback."""
import asyncio,time,unittest
from test_esphomed_buttons import ButtonEvents
from aioesphomeapi.model import Event,EventInfo

class PinnedButtons(ButtonEvents):
 def test_pinned_client_sees_button_event_entities_and_presses(self):
  self.s.close();time.sleep(.05)
  async def run():
   from test_esphomed_aio import APIClient
   client=APIClient('127.0.0.1',self.port,provide_time=False)
   try:
    await client.connect(login=True);entities,_=await client.list_entities_services()
    events=[e for e in entities if isinstance(e,EventInfo) and e.key<20]
    self.assertEqual([(e.object_id,e.key,e.name,e.event_types) for e in events],[
     ('volume_up',10,'Volume up button',['press','long_press','double_press']),
     ('volume_down',11,'Volume down button',['press','long_press','double_press']),
     ('microphone_mute',12,'Microphone mute button',['press','long_press','double_press']),
     ('action',13,'Action button',['press','long_press','double_press'])])
    activity=[e for e in entities if isinstance(e,EventInfo) and e.key>=20]
    self.assertEqual([(e.object_id,e.key,e.name,e.event_types) for e in activity],[
     ('voice_assistant',20,'Voice assistant',['wake_word','listening','thinking','replying','finished','stopped']),
     ('playback',21,'Playback',['started','stopped']),
     ('timer',22,'Timer',['started','updated','cancelled','finished'])])
    got=[];client.subscribe_states(lambda s:got.append(s) if isinstance(s,Event) else None)
    self.sender.sendto(b'action long_press',self.button_path)
    end=time.monotonic()+3
    while not got and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertEqual([(s.key,s.event_type) for s in got],[(13,'long_press')])
   finally:await client.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),10))

if __name__=='__main__':unittest.main(defaultTest='PinnedButtons.test_pinned_client_sees_button_event_entities_and_presses')
