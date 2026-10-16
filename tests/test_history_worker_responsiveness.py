#!/usr/bin/env python3
"""Real HTTP and silent agent socket: history must not block static requests."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="le-history-worker-") as directory:
        directory = Path(directory)
        agent = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        path = str(directory / "agent.sock")
        agent.bind(path)
        agent.listen(10)
        agent.settimeout(0.1)
        stopping = threading.Event()
        entered = threading.Event()

        def serve():
            while not stopping.is_set():
                try:
                    peer, _ = agent.accept()
                except socket.timeout:
                    continue
                with peer:
                    peer.settimeout(2)
                    request = json.loads(peer.recv(16384))
                    entered.set()
                    # A stalled adapter is unavoidable input, not a mocked
                    # HTTP dispatch. The real server has to detach this work.
                    stopping.wait(0.8)
                    peer.sendall((json.dumps({"id": request["id"], "ok": False,
                                              "error": "fixture refusal"}) + "\n").encode())

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = directory / "config.json"
        config.write_text("{}")
        env = dict(os.environ, LIBREECHO_AGENT_SOCKET=path)
        with (directory / "web.log").open("wb") as log:
            web = subprocess.Popen([str(ROOT / "build/libreecho-web"), "--backend", "mock",
                                    "--config", str(config), "--mock-config", str(ROOT / "config/mock-state.json"),
                                    "--web-root", str(ROOT / "web"), "--listen", f"127.0.0.1:{port}",
                                    "--dev-controls"], env=env, stdout=log, stderr=log)
            url = f"http://127.0.0.1:{port}"

            def request(route, method="GET", csrf=None):
                headers = {"X-LibreEcho-CSRF": csrf} if csrf else {}
                req = urllib.request.Request(url + route, method=method, headers=headers)
                try:
                    with urllib.request.urlopen(req, timeout=3) as reply:
                        return reply.status, reply.read()
                except urllib.error.HTTPError as error:
                    return error.code, error.read()

            try:
                for _ in range(100):
                    try:
                        status, body = request("/api/v1/config")
                        if status == 200:
                            break
                    except OSError:
                        pass
                    time.sleep(0.02)
                else:
                    raise AssertionError("fixture web did not start")
                csrf = json.loads(body)["data"]["csrf_token"]
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    for method, route in [("GET", "/api/v1/assistant/history"),
                                          ("GET", "/api/v1/assistant/history/4242"),
                                          ("DELETE", "/api/v1/assistant/history"),
                                          ("POST", "/api/v1/assistant/history/clear")]:
                        entered.clear()
                        pending = pool.submit(request, route, method, csrf)
                        assert entered.wait(2), f"{method} {route} never reached agentd"
                        start = time.monotonic()
                        status, _ = request("/css/app.css")
                        elapsed = time.monotonic() - start
                        assert status == 200
                        assert elapsed < 0.35, f"{method} {route} blocked static HTTP for {elapsed:.3f}s"
                        assert pending.result()[0] >= 400  # Refusal propagated, not fabricated success.
                        print(f"{method} {route}: responsive ({elapsed:.3f}s), refusal preserved")
            finally:
                web.terminate()
                web.wait(timeout=5)
                stopping.set()
                thread.join(timeout=3)
                agent.close()


if __name__ == "__main__":
    main()
