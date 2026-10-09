"""Real-process health regressions across API, boot init and watchdog probes."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

from test_esphomed import Fixture, frame, receive

ROOT = Path(__file__).resolve().parents[1]


class HealthFixture(Fixture):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="esphome-health-build-", dir=os.environ.get("TMPDIR"))
        cls.probe = Path(cls.build.name) / "probe"
        flags = [os.environ.get("CC", "cc"), "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-O2",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-Wno-format-truncation", "-Isrc", "-Isrc/adapter",
                 '-DLE_INIT_ESPHOMED="' + cls.build.name + '/no-op.init"',
                 '-DLE_INIT_AGENTD="/nonexistent/fixture-agent.init"',
                 '-DLE_INIT_STTD="/nonexistent/fixture-stt.init"',
                 '-DLE_INIT_TTSD="/nonexistent/fixture-tts.init"',
                 '-DLE_SOURCE_COMMIT="fixture"', '-DLE_SOURCE_DIGEST="fixture"', '-DLE_SOURCE_DIRTY="1"']
        subprocess.run(flags + ["tests/test_esphome_health_probe.c", "src/backend.c", "src/json.c",
                               "src/config_store.c", "src/service_env.c", "src/event_bus.c", "src/log.c",
                               "src/adapter/wyoming_client.c", "src/adapter/adapter_client.c", "src/adapter/watchdog_policy.c",
                               "-o", str(cls.probe)], cwd=ROOT, check=True, timeout=60)
        script = (ROOT / "init/libreecho-web.init").read_text()
        start = script.index("esphome_service_ready() {")
        cls.predicate = script[start:script.index("\nmark_startup_ready()", start)] + "\nesphome_service_ready\n"

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        super().setUp()
        self.pidfile = self.p / "esphomed.pid"
        self.pidfile.write_text(str(self.proc.pid) + "\n")
        self.daemon = str(Path(self.proc.args[0]).resolve())
        self.env = dict(os.environ, LIBREECHO_ESPHOMED_DAEMON=self.daemon,
                        LIBREECHO_ESPHOMED_PIDFILE=str(self.pidfile),
                        LIBREECHO_ESPHOME_STATUS_FILE=str(self.p / "status.json"),
                        LIBREECHO_ESPHOME_PORT=str(self.port), ESPHOMED_DAEMON=self.daemon,
                        ESPHOMED_PIDFILE=str(self.pidfile), ESPHOME_STATUS_FILE=str(self.p / "status.json"),
                        ESPHOME_PORT=str(self.port))

    def status(self):
        return json.loads((self.p / "status.json").read_text())

    def publish(self, status):
        temp = self.p / "status.test"
        temp.write_text(json.dumps(status))
        temp.replace(self.p / "status.json")

    def check_surfaces(self, ready, connected, label):
        output = subprocess.check_output([str(self.probe), "probe"], env=self.env, cwd=ROOT, timeout=3)
        observed = tuple(map(int, output.split()))
        self.assertEqual(observed, (int(ready), int(connected), int(ready)), label + " API/connected/watchdog")
        boot = subprocess.run(["sh", "-c", self.predicate], env=self.env, cwd=ROOT, timeout=3,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(boot.returncode == 0, ready, label + " boot init: " + boot.stderr.decode())
        service_env = dict(self.env, DEFAULTS=str(self.p / "missing"), DAEMON=self.daemon,
                           PIDFILE=str(self.pidfile), STATUS_FILE=str(self.p / "status.json"),
                           PORT=self.env["LIBREECHO_ESPHOME_PORT"])
        service = subprocess.run(["sh", str(ROOT / "init/libreecho-esphomed.init"), "status"],
                                 env=service_env, cwd=ROOT, timeout=3,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(service.returncode == 0, ready, label + " service init status")

    def test_start_init_cannot_accept_genuine_pid_with_unowned_listener(self):
        original = self.status()
        with socket.socket() as foreign:
            foreign.bind(("127.0.0.1", 6053))
            foreign.listen()
            self.publish(dict(original, port=6053,
                              listener_inode=str(os.fstat(foreign.fileno()).st_ino)))
            env = dict(self.env, DEFAULTS=str(self.p / "missing"), DAEMON=self.daemon,
                       PIDFILE=str(self.pidfile), STATUS_FILE=str(self.p / "status.json"), PORT="6053")
            start = subprocess.run(["sh", str(ROOT / "init/libreecho-esphomed.init"), "start"],
                                   env=env, cwd=ROOT, timeout=4)
            self.assertNotEqual(start.returncode, 0, "init start accepted process without its expected listener")
            self.assertIsNone(self.proc.poll(), "init start killed an already running daemon")

    def test_unrelated_live_pid_and_forged_argv0_fail_closed(self):
        # Supply correct live PID/start/boot/listener evidence so only the exact
        # executable check can reject these identities. argv[0] is not identity.
        with socket.socket() as foreign:
            foreign.bind(("127.0.0.1", 0))
            foreign.listen()
            port = foreign.getsockname()[1]
            self.env.update(LIBREECHO_ESPHOME_PORT=str(port), ESPHOME_PORT=str(port))
            status = dict(self.status(), ready=True, connected=True, pid=os.getpid(), port=port,
                          listener_inode=str(os.fstat(foreign.fileno()).st_ino),
                          start_time=Path("/proc/self/stat").read_text().rsplit(")", 1)[1].split()[19])
            self.pidfile.write_text(str(os.getpid()) + "\n")
            self.publish(status)
            self.check_surfaces(False, False, "unrelated Python PID with its own listener and bound true status")
            sleeper = subprocess.Popen([self.daemon, "20"], executable="/usr/bin/sleep",
                                       pass_fds=(foreign.fileno(),))
            try:
                self.pidfile.write_text(str(sleeper.pid) + "\n")
                status.update(pid=sleeper.pid,
                              start_time=Path("/proc", str(sleeper.pid), "stat").read_text().rsplit(")", 1)[1].split()[19])
                self.publish(status)
                self.assertEqual(Path("/proc", str(sleeper.pid), "cmdline").read_bytes().split(b"\0")[0].decode(), self.daemon)
                self.assertEqual(os.readlink(Path("/proc", str(sleeper.pid), "fd", str(foreign.fileno()))),
                                 "socket:[" + status["listener_inode"] + "]")
                self.check_surfaces(False, False, "sleep forged argv0 with matching start and owned listener")
                env = dict(self.env, DEFAULTS=str(self.p / "missing"), DAEMON=self.daemon,
                           PIDFILE=str(self.pidfile), STATUS_FILE=str(self.p / "status.json"), PORT=str(port))
                subprocess.run(["sh", str(ROOT / "init/libreecho-esphomed.init"), "stop"],
                               env=env, check=True, timeout=8)
                self.assertIsNone(sleeper.poll(), "init stop killed unrelated forged-argv0 process")
            finally:
                if sleeper.poll() is None:
                    sleeper.terminate()
                sleeper.wait(timeout=3)

    def test_stale_process_identity_and_unrelated_listener_fail_closed(self):
        original = self.status()
        self.assertEqual(original.get("pid"), self.proc.pid, "status must carry actual creator PID")
        start = Path("/proc", str(self.proc.pid), "stat").read_text().rsplit(")", 1)[1].split()[19]
        self.assertEqual(original.get("start_time"), start)
        self.check_surfaces(True, False, "genuine daemon without HA")
        for patch in ({"pid": self.proc.pid + 1}, {"start_time": str(int(start) + 1)},
                      {"boot_id": "stale-boot"}, {"listener_inode": "1"},
                      {"ready": False, "connected": True}):
            self.publish(dict(original, **patch))
            self.check_surfaces(False, False, "stale/reused identity " + str(patch))
        self.publish(original)
        with socket.socket() as foreign:
            foreign.bind(("127.0.0.1", 6053))
            foreign.listen()
            self.env.update(LIBREECHO_ESPHOME_PORT="6053", ESPHOME_PORT="6053")
            self.publish(dict(original, port=6053, listener_inode=str(os.fstat(foreign.fileno()).st_ino)))
            self.check_surfaces(False, False, "genuine PID cannot borrow unrelated TCP6053 listener inode")
        self.env.update(LIBREECHO_ESPHOME_PORT=str(self.port), ESPHOME_PORT=str(self.port))
        self.publish(original)
        self.proc.terminate()
        self.proc.communicate(timeout=3)
        with socket.socket() as unrelated:
            unrelated.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            unrelated.bind(("127.0.0.1", self.port))
            unrelated.listen()
            self.pidfile.write_text(str(os.getpid()) + "\n")
            self.publish(dict(original, ready=True, connected=True, pid=os.getpid()))
            self.check_surfaces(False, False, "unrelated listener on expected TCP port")
        # The production port specifically must not be accepted either.
        with socket.socket() as unrelated:
            unrelated.bind(("127.0.0.1", 6053))
            unrelated.listen()
            self.env.update(LIBREECHO_ESPHOME_PORT="6053", ESPHOME_PORT="6053")
            self.check_surfaces(False, False, "unrelated TCP6053 listener")

    def test_malformed_missing_symlink_and_dead_pid_fail_closed(self):
        original = self.status()
        for body in ("{}", "{bad}", '{"ready":true,"connected":true}',
                     json.dumps(dict(original, connected="true")),
                     json.dumps(original)[:-1] + ',"ready":true}'):
            (self.p / "status.json").write_text(body)
            self.check_surfaces(False, False, "malformed or unbound status")
        (self.p / "status.json").unlink()
        self.check_surfaces(False, False, "missing status")
        target = self.p / "target.json"
        target.write_text(json.dumps(original))
        (self.p / "status.json").symlink_to(target)
        self.check_surfaces(False, False, "status symlink")
        (self.p / "status.json").unlink()
        self.publish(original)
        for pid in ("99999999\n", "invalid\n", str(self.proc.pid) + "suffix\n"):
            self.pidfile.write_text(pid)
            self.check_surfaces(False, False, "dead or malformed pidfile")
        self.pidfile.unlink()
        self.check_surfaces(False, False, "missing pidfile")

    def test_boot_init_rejects_unrelated_live_pid(self):
        self.pidfile.write_text(str(os.getpid()) + "\n")
        self.publish({"ready": True, "connected": True})
        boot = subprocess.run(["sh", "-c", self.predicate], env=self.env, cwd=ROOT, timeout=3)
        self.assertNotEqual(boot.returncode, 0, "boot accepted unrelated PID and stale true status")

    def test_noop_init_cannot_complete_api_activation(self):
        init = Path(self.build.name) / "no-op.init"
        init.write_text("#!/bin/sh\nexit 0\n")
        init.chmod(0o700)
        self.pidfile.write_text(str(os.getpid()) + "\n")
        self.publish({"ready": True, "connected": True})
        result = subprocess.run([str(self.probe), "restart"], env=self.env, cwd=ROOT, timeout=8)
        self.assertNotEqual(result.returncode, 0, "no-op init reported ready without genuine listener owner")

    def test_watchdog_recovery_not_suppressed_by_reused_unrelated_pid(self):
        marker = self.p / "recovery"
        init = self.p / "recovery.init"
        init_env = dict(self.env, DAEMON=self.daemon, DEFAULTS=str(self.p / "missing"),
                        PIDFILE=str(self.pidfile), LOGFILE=str(self.p / "restarted.log"),
                        CONFIG=str(self.config), STATUS_FILE=str(self.p / "status.json"),
                        PORT=str(self.port), BIND="127.0.0.1", AUDIO_BUS=str(self.bus),
                        PRIVACY_STATE=str(self.privacy), IDME_ROOT=str(self.p / "idme"),
                        TLS_CA=str(self.p / "missing-ca"))
        for kind in ("audio", "wake", "radio", "timer", "led", "mdns"):
            init_env[kind.upper() + "_SOCKET"] = str(self.p / (kind + ".sock"))
        import shlex
        assignments = " ".join(key + "=" + shlex.quote(init_env[key]) for key in
                               ("DAEMON", "DEFAULTS", "PIDFILE", "LOGFILE", "CONFIG", "STATUS_FILE",
                                "PORT", "BIND", "AUDIO_BUS", "PRIVACY_STATE", "IDME_ROOT", "TLS_CA",
                                "AUDIO_SOCKET", "WAKE_SOCKET", "RADIO_SOCKET", "TIMER_SOCKET", "LED_SOCKET", "MDNS_SOCKET"))
        init.write_text("#!/bin/sh\nprintf '%s\\n' \"$1\" >> '" + str(marker) + "'\n" + assignments +
                        " exec sh " + shlex.quote(str(ROOT / "init/libreecho-esphomed.init")) + " \"$1\"\n")
        init.chmod(0o700)
        with (self.p / "watchdog.log").open("w+") as log:
            supervisor = subprocess.Popen([str(self.probe), "watchdog", "--passes", "5", "--interval", "1",
                                           "--service", "esphomed:" + str(self.pidfile) + ":" + str(init)],
                                          env=self.env, cwd=ROOT, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 2
                while "supervising esphomed" not in (self.p / "watchdog.log").read_text() and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertIn("supervising esphomed", (self.p / "watchdog.log").read_text())
                self.proc.terminate()
                self.proc.communicate(timeout=3)
                self.pidfile.write_text(str(os.getpid()) + "\n")
                self.assertEqual(supervisor.wait(timeout=8), 0)
                self.assertEqual(marker.read_text().splitlines(), ["stop", "start"])
                self.check_surfaces(True, False, "actual watchdog recovered genuine listener")
            finally:
                if supervisor.poll() is None:
                    supervisor.terminate()
                supervisor.wait(timeout=3)
                subprocess.run(["sh", str(ROOT / "init/libreecho-esphomed.init"), "stop"],
                               env=init_env, cwd=ROOT, timeout=8, check=True)
                self.assertFalse(self.pidfile.exists(), "recovered daemon pidfile survived cleanup")

    def test_actual_daemon_authenticated_connection_and_disconnect(self):
        self.check_surfaces(True, False, "listener alone is not HA")
        self.hello()
        self.check_surfaces(True, True, "actual ESPHome Hello")
        self.s.sendall(frame(5))
        self.assertEqual(receive(self.s), (6, b""))
        deadline = time.monotonic() + 2
        while self.status()["connected"] and time.monotonic() < deadline:
            time.sleep(.01)
        self.check_surfaces(True, False, "actual HA disconnect")


if __name__ == "__main__":
    names = ["HealthFixture." + name for name in HealthFixture.__dict__ if name.startswith("test_")]
    unittest.main(defaultTest=names)
