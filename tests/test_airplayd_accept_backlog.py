#!/usr/bin/env python3
"""airplayd control socket must survive a silent client and a request burst.

audiod probes this socket for AirPlay liveness. A connected client that never
sends must not wedge the single-threaded loop, and a burst of queued requests
must all be answered promptly, otherwise the accept queue fills and the probe
reports a live controller as lost (music muted mid-session).
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

BINARY = os.environ.get('AIRPLAYD', 'build/libreecho-airplayd')


def status(path, timeout):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    s.sendall(b'{"v":1,"id":1,"cmd":"status","args":{}}\n')
    data = b''
    while b'\n' not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    return data


def main():
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'airplay.sock')
        env = dict(os.environ)
        proc = subprocess.Popen([BINARY, '--foreground', '--socket', path,
                                 '--root', tmp, '--engine', '/nonexistent'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        try:
            deadline = time.monotonic() + 40
            while True:
                try:
                    if b'"ok":true' in status(path, 1.0):
                        break
                except OSError:
                    pass
                if time.monotonic() > deadline or proc.poll() is not None:
                    print('airplayd accept backlog: controller never answered', file=sys.stderr)
                    return 1
                time.sleep(0.1)
            silent = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            silent.connect(path)
            try:
                started = time.monotonic()
                reply = status(path, 3.0)
                if b'"ok":true' not in reply or time.monotonic() - started > 2.0:
                    print('airplayd accept backlog: silent client stalled the loop', file=sys.stderr)
                    return 1
            except OSError:
                print('airplayd accept backlog: silent client stalled the loop', file=sys.stderr)
                return 1
            finally:
                silent.close()
            burst = []
            for _ in range(12):
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(3.0)
                s.connect(path)
                s.sendall(b'{"v":1,"id":2,"cmd":"status","args":{}}\n')
                burst.append(s)
            for s in burst:
                if b'"ok":true' not in s.recv(4096):
                    print('airplayd accept backlog: burst request unanswered', file=sys.stderr)
                    return 1
                s.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    print('airplayd accept backlog: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
