"""Short diagnostic turn deadline preserves timers until ownership is released."""
import json,time,unittest
from test_esphomed import Fixture,frame,num,text,receive
class WatchdogFixture(Fixture):
 def setUp(self):self.extra_args=['--turn-timeout-ms','500'];super().setUp()
 def test_turn_deadline_preserves_remote_timers_until_unsubscribe(self):
  self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4))+frame(115,text(2,'watchdog-timer')+num(4,60)+num(5,55)+num(6,1)));self.wait_call('timer','remote_event')
  clears=sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls)
  self.assertEqual(clears,1,'startup recovery must clear remote ownership once before timer events')
  a=self.adapters['wake'];a.wait_streams();a.samples(0);time.sleep(.1);a.wake(100);self.assertEqual(receive(self.s)[0],90);self.assertEqual(receive(self.s),(90,num(1,0)))
  status=json.loads((self.p/'status.json').read_text());self.assertEqual(status['last_result'],'timeout');self.assertFalse(status['in_progress']);self.assertEqual(sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls),clears)
  self.s.sendall(frame(123));self.assertEqual(receive(self.s)[0],122);time.sleep(.05);self.assertEqual(sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls),clears)
  self.s.sendall(frame(89));self.wait_call('timer','remote_clear',after=clears);self.assertEqual(sum(r['cmd']=='remote_clear' for r in self.adapters['timer'].calls),clears+1)
if __name__=='__main__':unittest.main(defaultTest='WatchdogFixture.test_turn_deadline_preserves_remote_timers_until_unsubscribe')
