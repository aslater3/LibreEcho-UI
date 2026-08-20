#!/usr/bin/env python3
"""Issue #86 regression test against a deliberately slow assistant socket."""
from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def slow_agent(path: Path, delay: float) -> None:
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(path))
    server.listen(1)
    client, _ = server.accept()
    with client:
        request = json.loads(client.recv(4096).split(b"\n", 1)[0])
        time.sleep(delay)
        reply = {"v": 1, "id": request["id"], "ok": True,
                 "data": {"text": "slow model answer"}}
        client.sendall((json.dumps(reply) + "\n").encode())
    server.close()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="libreecho-issue-86-") as temp:
        root = Path(temp)
        source = root / "client.c"
        binary = root / "client"
        source.write_text(
            '#include "adapter.h"\n'
            '#include <stdio.h>\n'
            'int main(int argc, char **argv) {\n'
            '  char out[4096]; struct le_adapter *a; int rc;\n'
            '  if (argc != 2) return 2;\n'
            '  a = le_adapter_connect(argv[1], 1000); if (!a) return 3;\n'
            '  if (le_adapter_set_io_timeout(a, 120000) != 0) return 4;\n'
            '  rc = le_adapter_call(a, "respond", "{\\"text\\":\\"hello\\"}", out, sizeof(out));\n'
            '  printf("rc=%d output=%s\\n", rc, out); le_adapter_close(a); return rc;\n'
            '}\n'
        )
        subprocess.run(
            ["cc", "-D_POSIX_C_SOURCE=200809L", "-std=c99", "-Wall", "-Wextra",
             "-Isrc", "-Isrc/adapter", str(source), "src/adapter/adapter_client.c",
             "src/log.c", "-o", str(binary)], cwd=ROOT, check=True,
        )
        socket_path = root / "agent.sock"
        server = threading.Thread(target=slow_agent, args=(socket_path, 6), daemon=True)
        server.start()
        deadline = time.monotonic() + 2
        while not socket_path.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        started = time.monotonic()
        result = subprocess.run([str(binary), str(socket_path)], cwd=ROOT,
                                capture_output=True, text=True)
        elapsed = time.monotonic() - started
        server.join(timeout=8)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "slow model answer" in result.stdout
        assert elapsed >= 6, elapsed
        print("issue_86_slow_assistant=PASS")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
