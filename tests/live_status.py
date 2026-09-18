#!/usr/bin/env python3
"""Print libreecho-lived's status JSON, over a unix socket or a tcp port."""
import json
import socket
import sys
def main():
    target = sys.argv[1]
    payload = json.dumps({"v": 1, "id": 1, "cmd": "status", "args": {}})
    if target.isdigit():
        conn = socket.create_connection(("127.0.0.1", int(target)), timeout=5)
    else:
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.settimeout(5)
        conn.connect(target)
    with conn:
        conn.sendall(payload.encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
    reply = json.loads(data.decode())
    print(json.dumps(reply.get("data", reply)))
if __name__ == "__main__":
    main()