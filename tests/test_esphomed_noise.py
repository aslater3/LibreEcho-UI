"""Independent cryptography-backed Noise initiator. NOT pinned aio client proof.
Run with an existing Python containing cryptography; never install in this test.
The wire contract is pinned aioesphomeapi v46.2.0 _frame_helper/noise.py.
"""
import base64, hashlib, hmac, json, os, socket, struct, time, unittest
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey,X25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding,PublicFormat
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from test_esphomed import Fixture,num,text,frame

def hkdf(ck,data,count=2):
 temp=hmac.new(ck,data,hashlib.sha256).digest();out=[];last=b''
 for i in range(1,count+1):last=hmac.new(temp,last+bytes([i]),hashlib.sha256).digest();out.append(last)
 return out
class Noise:
 def __init__(self,s,key):self.s=s;self.psk=key;self.tx=self.rx=0
 def read_exact(self,n):
  out=b''
  while len(out)<n:
   part=self.s.recv(n-len(out))
   if not part:raise EOFError()
   out+=part
  return out
 def outer(self,b):return b'\1'+struct.pack('>H',len(b))+b
 def read_outer(self):
  h=self.read_exact(3);assert h[0]==1;return self.read_exact(struct.unpack('>H',h[1:])[0])
 def mix(self,b):self.h=hashlib.sha256(self.h+b).digest()
 def handshake(self):
  self.h=hashlib.sha256(b'Noise_NNpsk0_25519_ChaChaPoly_SHA256').digest();self.ck=self.h;self.mix(b'NoiseAPIInit\0\0')
  self.ck,tmp,key=hkdf(self.ck,self.psk,3);self.mix(tmp)
  ephemeral=X25519PrivateKey.generate();pub=ephemeral.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
  self.mix(pub);self.ck,key=hkdf(self.ck,pub);cipher=ChaCha20Poly1305(key).encrypt(b'\0'*12,b'',self.h);self.mix(cipher)
  # Pinned client coalesces client hello and its first handshake message.
  self.s.sendall(b'\1\0\0'+self.outer(b'\0'+pub+cipher));hello=self.read_outer();assert hello==b'\1fixture-satellite\0'+b'02:00:00:00:00:01\0',hello
  reply=self.read_outer()
  if reply[0]:return reply
  assert len(reply)==49;peer=reply[1:33];self.mix(peer);self.ck,key=hkdf(self.ck,peer);self.ck,key=hkdf(self.ck,ephemeral.exchange(X25519PublicKey.from_public_bytes(peer)))
  assert ChaCha20Poly1305(key).decrypt(b'\0'*12,reply[33:],self.h)==b'';self.mix(reply[33:]);self.txkey,self.rxkey=hkdf(self.ck,b'');return reply
 def packet(self,t,b=b''):
  cipher=ChaCha20Poly1305(self.txkey).encrypt(struct.pack('<IQ',0,self.tx),struct.pack('>HH',t,len(b))+b,None);self.tx+=1;return self.outer(cipher)
 def send(self,t,b=b''):self.s.sendall(self.packet(t,b))
 def receive(self):
  msg=ChaCha20Poly1305(self.rxkey).decrypt(struct.pack('<IQ',0,self.rx),self.read_outer(),None);self.rx+=1;t,n=struct.unpack('>HH',msg[:4]);assert len(msg)==n+4;return t,msg[4:]
class NoiseFixture(Fixture):
 def test_noise_provision_persist_reconnect_wrong_key_and_plain_refusal(self):
  c=Noise(self.s,b'\0'*32);self.assertEqual(c.handshake()[0],0);c.send(1,num(2,1)+num(3,14));self.assertEqual(c.receive()[0],2);c.send(3);c.send(9);t,b=c.receive();self.assertEqual(t,10);self.assertIn(num(26,1),b)
  original=json.loads(self.config.read_text());self.config.write_text(json.dumps({'shadow':{'esphome_noise_key':'must-not-change'},**original}));key=bytes(range(32));c.send(124,text(1,key));self.assertEqual(c.receive(),(125,num(1,1)));self.assertEqual(self.s.recv(1),b'')
  saved=json.loads(self.config.read_text());self.assertEqual(saved['esphome_noise_key'],base64.b64encode(key).decode());self.assertEqual(saved['wifi_mac'],'02:00:00:00:00:01');self.assertEqual(saved['shadow']['esphome_noise_key'],'must-not-change');self.assertEqual(os.stat(self.config).st_mode&0o777,0o600)
  plain=self.connect();plain.sendall(frame(1));self.assertEqual(plain.recv(1),b'');time.sleep(.05)
  wrong=self.connect();bad=Noise(wrong,b'\0'*32);self.assertEqual(bad.handshake(),b'\1Handshake MAC failure');self.assertEqual(wrong.recv(1),b'');time.sleep(.05)
  good=self.connect();c=Noise(good,key);self.assertEqual(c.handshake()[0],0);c.send(1);self.assertEqual(c.receive()[0],2);c.send(9);t,b=c.receive();self.assertEqual(t,10);self.assertIn(num(26,0),b)
  packet=bytearray(c.packet(7));packet[-1]^=1;good.sendall(packet);self.assertEqual(good.recv(1),b'')
 def test_noise_replay_is_rejected(self):
  c=Noise(self.s,b'\0'*32);self.assertEqual(c.handshake()[0],0);c.send(1);self.assertEqual(c.receive()[0],2);packet=c.packet(7);self.s.sendall(packet);self.assertEqual(c.receive(),(8,b''));self.s.sendall(packet);self.assertEqual(self.s.recv(1),b'')
if __name__=='__main__':unittest.main()
