"""Pinned aioesphomeapi 46.2.0 view of the #395 LED ring light (RGB, brightness, effects).
Runs against an ALREADY INSTALLED 46.2.0 environment; no install fallback."""
import asyncio,time,unittest
from test_esphomed_led_light import LedLight
from aioesphomeapi.model import ColorMode,LightInfo,LightState

class PinnedLedLight(LedLight):
 def test_pinned_client_sees_led_ring_light_and_commands(self):
  self.s.close();time.sleep(.05)
  async def run():
   from test_esphomed_aio import APIClient
   client=APIClient('127.0.0.1',self.port,provide_time=False)
   try:
    await client.connect(login=True);entities,_=await client.list_entities_services()
    lights=[e for e in entities if isinstance(e,LightInfo)]
    self.assertEqual([(e.object_id,e.key,e.name,e.effects) for e in lights],[('led_ring',30,'LED ring',['Solid','Pulse','Flash'])])
    self.assertEqual(lights[0].supported_color_modes,[ColorMode.RGB])
    states=[];client.subscribe_states(lambda s:states.append(s) if isinstance(s,LightState) else None)
    end=time.monotonic()+3
    while not states and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertTrue(states,'no light state on subscribe');self.assertEqual((states[0].key,states[0].state,states[0].effect),(30,False,'Solid'))
    client.light_command(30,state=True,rgb=(0.0,1.0,0.0),brightness=0.25,effect='Flash')
    def led_calls():return [r['args'] for r in self.adapters['led'].calls if r['cmd']=='pattern']
    end=time.monotonic()+3
    while {'name':'flash','r':0,'g':255,'b':0,'brightness':25,'repeats':0,'owner':'esphome-light'} not in led_calls() and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertIn({'name':'flash','r':0,'g':255,'b':0,'brightness':25,'repeats':0,'owner':'esphome-light'},led_calls(),'flash overlay not sent')
    end=time.monotonic()+3
    while not any(s.state and s.effect=='Flash' for s in states) and time.monotonic()<end:await asyncio.sleep(.02)
    on=[s for s in states if s.state];self.assertTrue(on,'light on not reported back');self.assertEqual(on[-1].effect,'Flash');self.assertAlmostEqual(on[-1].brightness,0.25,places=2)
    client.light_command(30,state=False)
    end=time.monotonic()+3
    while (not led_calls() or led_calls()[-1]!={'name':'stop','owner':'esphome-light'}) and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertEqual(led_calls()[-1],{'name':'stop','owner':'esphome-light'},'off did not release the HA overlay')
    end=time.monotonic()+3
    while states[-1].state and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertFalse(states[-1].state)
   finally:await client.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),10))

if __name__=='__main__':unittest.main(defaultTest='PinnedLedLight.test_pinned_client_sees_led_ring_light_and_commands')
