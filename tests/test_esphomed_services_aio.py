"""Pinned aioesphomeapi 46.2.0 view of the #397 user services: the list a Home Assistant client
builds and the ExecuteServiceRequest it sends, through the real client. Runs against an ALREADY
INSTALLED 46.2.0 environment; no install fallback."""
import asyncio,time,unittest
from test_esphomed_services import Services,OWNER
from aioesphomeapi.model import UserServiceArgType

class PinnedServices(Services):
 def test_pinned_client_lists_and_executes_the_services(self):
  self.s.close();time.sleep(.05)
  async def wait_for(check,seconds=3):
   end=time.monotonic()+seconds
   while time.monotonic()<end:
    value=check()
    if value:return value
    await asyncio.sleep(.02)
   self.fail('expected daemon call not seen')
  async def run():
   from test_esphomed_aio import APIClient
   client=APIClient('127.0.0.1',self.port,provide_time=False)
   try:
    await client.connect(login=True);_,services=await client.list_entities_services()
    listed={s.name:s for s in services}
    self.assertEqual(sorted(listed),['announce','play_sound','set_ring'])
    self.assertEqual([(a.name,a.type) for a in listed['announce'].args],[('text',UserServiceArgType.STRING)])
    self.assertEqual([(a.name,a.type) for a in listed['play_sound'].args],[('name',UserServiceArgType.STRING)])
    self.assertEqual([(a.name,a.type) for a in listed['set_ring'].args],[('colour',UserServiceArgType.STRING),('effect',UserServiceArgType.STRING),('duration',UserServiceArgType.INT)])
    await client.execute_service(listed['announce'],{'text':'Tea is ready'})
    args=await wait_for(lambda:(self.calls('audio','speak') or [None])[-1])
    self.assertEqual(args['args'],{'text':'Tea is ready'})
    await client.execute_service(listed['play_sound'],{'name':'action-3'})
    self.assertEqual((await wait_for(lambda:(self.calls('audio','sample') or [None])[-1]))['args'],{'name':'action-3'})
    await client.execute_service(listed['set_ring'],{'colour':'#00ff10','effect':'Flash','duration':1})
    ring=await wait_for(lambda:next((r['args'] for r in self.calls('led','pattern') if r['args'].get('name')=='flash' and r['args'].get('owner')==OWNER),None))
    self.assertEqual(ring,{'name':'flash','r':0,'g':255,'b':16,'brightness':100,'repeats':0,'owner':OWNER})
    stop=await wait_for(lambda:len([r for r in self.calls('led','pattern') if r['args'].get('name')=='stop' and r['args'].get('owner')==OWNER])>1,seconds=4)
    self.assertTrue(stop)
   finally:await client.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),15))

if __name__=='__main__':unittest.main(defaultTest='PinnedServices.test_pinned_client_lists_and_executes_the_services')
