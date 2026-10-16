import ctypes
import sys
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
class Metadata(ctypes.Structure):
    _fields_ = [(name, ctypes.c_char * size) for name, size in [
        ('version', 32), ('mac', 18), ('platform', 16), ('board', 32),
        ('network', 16), ('friendly_name', 64), ('api_encryption', 48),
        ('project_name', 64), ('project_version', 32)]]

def metadata():
    from test_mdns_esphome import FIELDS
    return Metadata(**{k: v.encode() for k, v in FIELDS.items()})

class Client(unittest.TestCase):
    def test_nonblocking_registration_and_owner_eof(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'client.so'
            proc = subprocess.run(['cc', '-shared', '-fPIC', '-std=c99', '-Wall', '-Wextra', '-Werror',
                                   str(ROOT / 'src/adapter/mdns_client.c'), str(ROOT / 'src/adapter/mdns_lease.c'), '-o', str(library)],
                                  capture_output=True, text=True, timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            api = ctypes.CDLL(str(library))
            self.assertTrue(hasattr(api, 'le_mdns_register_esphome'), 'ESPHome client helper missing')
            api.le_mdns_register_esphome.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(Metadata)]
            api.le_mdns_register_esphome.restype = ctypes.c_int
            info = metadata()
            api.le_mdns_receive.argtypes = [ctypes.c_int]
            path = str(Path(tmp) / 'mdns.sock')
            self.assertEqual(api.le_mdns_register_esphome(path.encode(), 6053, ctypes.byref(info)), -1)
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                server.bind(path); server.listen(1); server.settimeout(2)
                fd = api.le_mdns_register_esphome(path.encode(), 6053, ctypes.byref(info))
                self.assertGreaterEqual(fd, 0)
                with socket.socket(fileno=fd) as client:
                    connection, _ = server.accept()
                    with connection:
                        connection.settimeout(2)
                        from test_mdns_esphome import packet
                        self.assertEqual(connection.recv(1536), packet())
                        self.assertEqual(api.le_mdns_receive(client.fileno()), 0)
                        connection.sendall(b'pending\n')
                        self.assertEqual(api.le_mdns_receive(client.fileno()), 1)
                    self.assertEqual(api.le_mdns_receive(client.fileno()), -1)


    def test_invalid_metadata_is_rejected_before_connect(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'client.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'src/adapter/mdns_client.c'), str(ROOT / 'src/adapter/mdns_lease.c'),
                            '-o', str(library)], check=True, capture_output=True, timeout=30)
            api = ctypes.CDLL(str(library))
            self.assertTrue(hasattr(api, 'le_mdns_register_esphome'), 'ESPHome client helper missing')
            api.le_mdns_register_esphome.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(Metadata)]
            path = str(Path(tmp) / 'control.sock').encode()
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as server:
                server.bind(path); server.listen(1); server.settimeout(.05)
                info = metadata()
                for port in [0, 65536]:
                    self.assertEqual(api.le_mdns_register_esphome(path, port, ctypes.byref(info)), -1)
                self.assertEqual(api.le_mdns_register_esphome(path, 6053, None), -1)
                info.friendly_name = b'x' * 64
                self.assertEqual(api.le_mdns_register_esphome(path, 6053, ctypes.byref(info)), -1)
                self.assertEqual(api.le_mdns_connect(path, 10700), -1)
                with self.assertRaises(socket.timeout):
                    server.accept()

def supervisor(path, response):
    """Answer one STATUS/1 probe on a private seqpacket listener."""
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(path)
    server.listen(1)
    server.settimeout(3)

    def serve():
        try:
            connection, _ = server.accept()
            with connection:
                connection.settimeout(3)
                assert connection.recv(64) == b'STATUS/1\n'
                connection.sendall(response)
        finally:
            server.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return thread


class Status(unittest.TestCase):
    """A shared supervisor is an external dependency, so its readiness probe
    must be bounded and must never report ready for an absent or degraded
    responder."""

    def test_readiness_probe_reports_supervisor_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            library = Path(tmp) / 'client.so'
            proc = subprocess.run(['cc', '-shared', '-fPIC', '-std=c99', '-Wall', '-Wextra', '-Werror',
                                   str(ROOT / 'src/adapter/mdns_client.c'), str(ROOT / 'src/adapter/mdns_lease.c'), '-o', str(library)],
                                  capture_output=True, text=True, timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            api = ctypes.CDLL(str(library))
            api.le_mdns_status.argtypes = [ctypes.c_char_p]
            api.le_mdns_status.restype = ctypes.c_int

            absent = str(Path(tmp) / 'absent.sock')
            self.assertEqual(api.le_mdns_status(absent.encode()), 0)
            self.assertEqual(api.le_mdns_status(None), 0)

            running = str(Path(tmp) / 'running.sock')
            thread = supervisor(running, b'running\n')
            self.assertEqual(api.le_mdns_status(running.encode()), 1)
            thread.join(3)

            degraded = str(Path(tmp) / 'degraded.sock')
            thread = supervisor(degraded, b'degraded\n')
            self.assertEqual(api.le_mdns_status(degraded.encode()), 0)
            thread.join(3)

            # A listener that never answers must not block the caller: the
            # probe is bounded and reports unavailable.
            silent = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            silent.bind(str(Path(tmp) / 'silent.sock'))
            silent.listen(1)
            try:
                self.assertEqual(
                    api.le_mdns_status(str(Path(tmp) / 'silent.sock').encode()), 0)
            finally:
                silent.close()


if __name__ == '__main__':
    unittest.main()
