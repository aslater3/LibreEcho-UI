"""Pinned aioesphomeapi 46.2.0 view of the #392 illuminance sensor (no install fallback)."""
import asyncio,time,unittest
from test_esphomed_aio import APIClient
from test_esphomed_light import LightSensor
from aioesphomeapi.model import SensorInfo,SensorState,SensorStateClass
class PinnedLight(LightSensor):
 def test_pinned_client_sees_lux_sensor_and_updates(self):
  self.s.close();time.sleep(.05)
  async def run():
   client=APIClient('127.0.0.1',self.port,provide_time=False)
   try:
    await client.connect(login=True);entities,_=await client.list_entities_services()
    self.assertEqual({e.key for e in entities},{1,2,3})
    sensor=next(e for e in entities if isinstance(e,SensorInfo))
    self.assertEqual((sensor.object_id,sensor.name,sensor.unit_of_measurement,sensor.device_class,sensor.state_class,sensor.accuracy_decimals),('illuminance','Illuminance','lx','illuminance',SensorStateClass.MEASUREMENT,0))
    states=[];client.subscribe_states(lambda s:states.append(s) if isinstance(s,SensorState) else None)
    end=time.monotonic()+2
    while not states and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertTrue(states);self.assertEqual((states[0].key,states[0].state,states[0].missing_state),(3,120.0,False))
    self.set_lux(640);end=time.monotonic()+2.5
    while states[-1].state!=640.0 and time.monotonic()<end:await asyncio.sleep(.02)
    self.assertEqual(states[-1].state,640.0)
   finally:await client.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),10))
if __name__=='__main__':unittest.main(defaultTest='PinnedLight.test_pinned_client_sees_lux_sensor_and_updates')
