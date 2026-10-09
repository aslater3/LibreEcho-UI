"""Run using an ALREADY INSTALLED aioesphomeapi==46.2.0 environment.
No install/download fallback. Missing/wrong dependency is an explicit failure,
not a skipped interoperability claim. Shares private real-daemon fixtures.
"""
import asyncio,base64,importlib.metadata,json,os,time,unittest
from test_esphomed import Fixture
try:
 from aioesphomeapi import APIClient
 from aioesphomeapi.api_pb2 import PingRequest,PingResponse
 from aioesphomeapi.core import APIConnectionError
except ImportError as exc:
 raise SystemExit('NOT RUN: pinned aioesphomeapi client unavailable; dependency installation requires approval') from exc
if importlib.metadata.version('aioesphomeapi')!='46.2.0':
 raise SystemExit('NOT RUN: expected aioesphomeapi==46.2.0, refusing a different client')
class PinnedFixture(Fixture):
 async def wait_for(self,predicate,description):
  end=time.monotonic()+3
  while time.monotonic()<end:
   if predicate():return
   await asyncio.sleep(.01)
  self.fail('pinned client did not observe '+description)
 async def exercise_controls_and_voice(self,client):
  import contextlib,io,struct,wave
  from aioesphomeapi.model import MediaPlayerCommand,MediaPlayerEntityState,SwitchState,VoiceAssistantEventType as VE,VoiceAssistantTimerEventType as VT
  states=[];client.subscribe_states(states.append);await self.wait_for(lambda:any(isinstance(s,MediaPlayerEntityState) for s in states),'media state')
  client.media_player_command(1,media_url='http://example.invalid/fixture.mp3');await self.wait_for(lambda:any(r['cmd']=='play' for r in self.adapters['radio'].calls),'play URL command');self.assertEqual(self.wait_call('radio','play')['args'],{'url':'http://example.invalid/fixture.mp3'})
  for command,verb in [(MediaPlayerCommand.PAUSE,'pause'),(MediaPlayerCommand.PLAY,'resume'),(MediaPlayerCommand.STOP,'stop')]:
   client.media_player_command(1,command=command);await self.wait_for(lambda:any(r['cmd']==verb for r in self.adapters['radio'].calls),verb)
  client.media_player_command(1,volume=.42);await self.wait_for(lambda:any(r['cmd']=='set_volume' and r['args']=={'volume':42} for r in self.adapters['audio'].calls),'volume command')
  client.media_player_command(1,command=MediaPlayerCommand.MUTE);await self.wait_for(lambda:any(r['cmd']=='set_volume' and r['args']=={'volume':0} for r in self.adapters['audio'].calls),'output mute');await self.wait_for(lambda:any(isinstance(s,MediaPlayerEntityState) and s.muted for s in states),'output muted state')
  client.media_player_command(1,command=MediaPlayerCommand.UNMUTE);await self.wait_for(lambda:isinstance(states[-2],MediaPlayerEntityState) and not states[-2].muted if len(states)>1 else False,'output unmuted state')
  client.switch_command(2,True);await self.wait_for(lambda:self.adapters['audio'].muted,'microphone mute');self.privacy.write_text('1\n');await asyncio.sleep(.3);before=sum(r['cmd']=='set_mute' for r in self.adapters['audio'].calls);client.switch_command(2,False);await asyncio.sleep(.1);self.assertEqual(sum(r['cmd']=='set_mute' for r in self.adapters['audio'].calls),before)
  self.privacy.write_text('0\n');await asyncio.sleep(.3);client.switch_command(2,False);await self.wait_for(lambda:not self.adapters['audio'].muted,'microphone unmute');await asyncio.sleep(.1)
  starts=[];stops=[];audio=[];finished=[]
  async def start(conversation,flags,settings,wake):starts.append((conversation,flags,wake));return 0
  async def stop(abort):stops.append(abort)
  async def capture(data,data2):audio.append((data,data2))
  async def done(result):finished.append(result)
  unsubscribe=client.subscribe_voice_assistant(handle_start=start,handle_stop=stop,handle_audio=capture,handle_announcement_finished=done)
  a=self.adapters['wake'];a.wait_streams()
  for i in range(0,6400,320):a.samples(i)
  await asyncio.sleep(.1);a.wake(4000);await self.wait_for(lambda:len(starts)==1 and audio,'wake and indexed API audio');self.assertEqual(starts[0],('',1,'Alexa'),'device-side wake: USE_VAD only, never USE_WAKE_WORD');self.assertEqual(len(audio[0][0]),640);self.assertEqual(audio[0][0][:2],struct.pack('<h',800));self.assertFalse(audio[0][1])
  for kind in VT:
   client.send_voice_assistant_timer_event(kind,'pinned-timer','tea',60,45,True)
   await self.wait_for(lambda:len([r for r in self.adapters['timer'].calls if r['cmd']=='remote_event'])==int(kind)+1,'ordered timer event')
  self.assertEqual([r['args']['event_type'] for r in self.adapters['timer'].calls if r['cmd']=='remote_event'],[0,1,2,3])
  clears=sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls)
  self.assertEqual(clears,1,'startup clear must precede owner timer events')
  client.send_voice_assistant_event(VE.VOICE_ASSISTANT_ERROR,{'code':'fixture-error'});await self.wait_for(lambda:True in stops,'error cancellation');self.assertEqual(sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls),clears)
  a.wake(4300);await self.wait_for(lambda:len(starts)==2,'second wake');await asyncio.sleep(.05)
  client.send_voice_assistant_event(VE.VOICE_ASSISTANT_INTENT_END,{'conversation_id':'pinned-conversation','continue_conversation':'1'});client.send_voice_assistant_event(VE.VOICE_ASSISTANT_RUN_END,None);await self.wait_for(lambda:len(starts)==3,'no-TTS continuation');self.assertEqual(starts[-1],('pinned-conversation',1,None));self.assertIn(False,stops)
  client.send_voice_assistant_event(VE.VOICE_ASSISTANT_ERROR,None);await self.wait_for(lambda:not json.loads((self.p/'status.json').read_text())['in_progress'],'continued turn cleanup')
  await client.set_voice_assistant_configuration([]);cfg=await client.get_voice_assistant_configuration(3);self.assertEqual(cfg.active_wake_words,[]);self.assertEqual(json.loads(self.config.read_text())['esphome_active_wake_word'],'');self.assertEqual(sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls),clears)
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1900)*1000)
  url=self.http_audio(wav.getvalue());before=len(finished);client.media_player_command(1,media_url=url,announcement=True);end=time.monotonic()+3
  while len(finished)==before and time.monotonic()<end:
   with contextlib.suppress(BlockingIOError):self.busbytes+=os.read(self.busfd,65536)
   await asyncio.sleep(.01)
  self.assertEqual(len(finished),before+1);self.assertTrue(finished[-1].success);self.assertEqual(self.busbytes,struct.pack('<h',1900)*2000);self.busbytes.clear()
  announce=asyncio.create_task(client.send_voice_assistant_announcement_await_response(url,4,start_conversation=True))
  while not announce.done():
   with contextlib.suppress(BlockingIOError):self.busbytes+=os.read(self.busfd,65536)
   await asyncio.sleep(.01)
  self.assertTrue((await announce).success);self.assertEqual(self.busbytes,struct.pack('<h',1900)*2000);await self.wait_for(lambda:len(starts)==4,'remote START_CONVERSATION with wake disabled');self.assertEqual(starts[-1],('',1,None))
  unsubscribe();await self.wait_for(lambda:sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls)==clears+1,'unsubscribe ownership cleanup');self.assertFalse(json.loads((self.p/'status.json').read_text())['in_progress'])
  await client.set_voice_assistant_configuration(['alexa_v0.1']);cfg=await client.get_voice_assistant_configuration(3);self.assertEqual(cfg.active_wake_words,['alexa_v0.1']);self.assertEqual(self.wait_call('wake','set_word')['args'],{'word':'Alexa'})
  client.subscribe_voice_assistant(handle_start=start,handle_stop=stop,handle_audio=capture);await asyncio.sleep(.05);await client.disconnect();await self.wait_for(lambda:sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls)==clears+2,'disconnect ownership cleanup');self.assertFalse(json.loads((self.p/'status.json').read_text())['in_progress']);await client.connect(login=True)
 def test_pinned_plain_real_login_entities_ping_disconnect(self):
  self.s.close();time.sleep(.05)
  async def run():
   client=APIClient('127.0.0.1',self.port,client_info='LibreEcho isolated acceptance',provide_time=False)
   try:
    await client.connect(login=True);info=await client.device_info();self.assertEqual(info.name,'fixture-satellite');self.assertEqual(info.mac_address,'02:00:00:00:00:01');self.assertEqual(info.voice_assistant_feature_flags,61)
    self.assertEqual((client.api_version.major,client.api_version.minor),(1,14));entities,services=await client.list_entities_services();self.assertEqual({e.key for e in entities},{1,2});self.assertFalse(services)
    from aioesphomeapi.model import MediaPlayerEntityFeature,MediaPlayerInfo
    media=next(e for e in entities if isinstance(e,MediaPlayerInfo));self.assertTrue(media.supports_pause);self.assertEqual(media.feature_flags,int(MediaPlayerEntityFeature.PAUSE|MediaPlayerEntityFeature.PLAY|MediaPlayerEntityFeature.STOP|MediaPlayerEntityFeature.VOLUME_SET|MediaPlayerEntityFeature.VOLUME_MUTE|MediaPlayerEntityFeature.PLAY_MEDIA|MediaPlayerEntityFeature.MEDIA_ANNOUNCE))
    await self.exercise_controls_and_voice(client)
    await client._get_connection().send_message_await_response(PingRequest(),PingResponse)
    await client.disconnect()
   finally:await client.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),12))
 def test_pinned_noise_provision_reconnect_wrong_key(self):
  self.s.close();time.sleep(.05);key=bytes(range(32));zero=base64.b64encode(b'\0'*32).decode();encoded=base64.b64encode(key).decode()
  async def run():
   client=APIClient('127.0.0.1',self.port,noise_psk=zero,expected_name='fixture-satellite',provide_time=False)
   try:
    await client.connect(login=True);info=await client.device_info();self.assertTrue(info.api_encryption_provisionable);self.assertTrue(await client.noise_encryption_set_key(key))
   finally:await client.disconnect(force=True)
   await asyncio.sleep(.1)
   self.assertEqual(json.loads(self.config.read_text())['esphome_noise_key'],encoded)
   bad=APIClient('127.0.0.1',self.port,noise_psk=zero,provide_time=False)
   try:
    with self.assertRaises(APIConnectionError):await bad.connect(login=True,log_errors=False)
   finally:await bad.disconnect(force=True)
   await asyncio.sleep(.05)
   plain=APIClient('127.0.0.1',self.port,provide_time=False)
   try:
    with self.assertRaises(APIConnectionError):await plain.connect(login=True,log_errors=False)
   finally:await plain.disconnect(force=True)
   await asyncio.sleep(.05)
   good=APIClient('127.0.0.1',self.port,noise_psk=encoded,provide_time=False)
   try:
    await good.connect(login=True);info=await good.device_info();self.assertFalse(info.api_encryption_provisionable);entities,_=await good.list_entities_services();self.assertEqual({e.key for e in entities},{1,2})
    await self.exercise_controls_and_voice(good)
    await good._get_connection().send_message_await_response(PingRequest(),PingResponse);await good.disconnect()
   finally:await good.disconnect(force=True)
  asyncio.run(asyncio.wait_for(run(),20))
if __name__=='__main__':unittest.main(defaultTest='PinnedFixture')
