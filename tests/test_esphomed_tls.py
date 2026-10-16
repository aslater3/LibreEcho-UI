"""Actual verified mbedTLS HTTPS, private generated certificates and PCM FIFO."""
import io,os,pathlib,ssl,struct,subprocess,tempfile,threading,unittest,wave
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from test_esphomed import Fixture,frame,num,text
class TLSFixture(Fixture):
 def setUp(self):
  self.certs=tempfile.TemporaryDirectory(prefix='esphomed-tls-',dir=os.environ.get('TMPDIR'));self.addCleanup(self.certs.cleanup);self.certdir=pathlib.Path(self.certs.name)
  for name,san in [('trusted','IP:127.0.0.1'),('untrusted','IP:127.0.0.1'),('wronghost','DNS:localhost')]:
   subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-sha256','-days','1','-subj','/CN=fixture','-addext','subjectAltName='+san,'-keyout',str(self.certdir/(name+'.key')),'-out',str(self.certdir/(name+'.crt'))],check=True,timeout=15,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
  # Wrong-host CA is deliberately trusted too: identity must still fail.
  ca=self.certdir/'ca.pem';ca.write_bytes((self.certdir/'trusted.crt').read_bytes()+(self.certdir/'wronghost.crt').read_bytes());self.extra_args=['--tls-ca',str(ca)];super().setUp()
 def https(self,name):
  wav=io.BytesIO()
  with wave.open(wav,'wb') as w:w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes(struct.pack('<h',1000)*1000)
  data=wav.getvalue()
  class HTTP(BaseHTTPRequestHandler):
   def do_GET(self):self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
   def log_message(self,format,*args):pass
  server=ThreadingHTTPServer(('127.0.0.1',0),HTTP);ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(self.certdir/(name+'.crt'),self.certdir/(name+'.key'));server.socket=ctx.wrap_socket(server.socket,server_side=True)
  thread=threading.Thread(target=server.serve_forever);thread.start();self.addCleanup(server.server_close);self.addCleanup(thread.join,2);self.addCleanup(server.shutdown);return f'https://127.0.0.1:{server.server_port}/tts.wav'
 def test_https_validated_ca_hostname_wav(self):
  url=self.https('trusted');self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));self.s.sendall(frame(119,text(1,url)));self.wait_completion();self.assertEqual(self.busbytes,struct.pack('<h',1000)*2000)
 def test_https_untrusted_is_negative_completion(self):
  url=self.https('untrusted');self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));self.s.sendall(frame(119,text(1,url)));self.wait_completion(False);self.assertEqual(self.busbytes,b'')
 def test_https_trusted_wrong_hostname_is_negative_completion(self):
  url=self.https('wronghost');self.hello();self.s.sendall(frame(89,num(1,1)+num(2,4)));self.s.sendall(frame(119,text(1,url)));self.wait_completion(False);self.assertEqual(self.busbytes,b'')
if __name__=='__main__':unittest.main(defaultTest='TLSFixture')
