#!/usr/bin/env python3
"""First-boot provisioning: the installer document, applied by the real daemon.

Every credential here is synthetic, generated inside this script from a fresh
salt, and asserted only through what the device derives back. No real
credential is read, and no device path is touched: each case owns a temporary
directory holding the daemon's config, the delivered document and the result.

The document is applied by the actual binary over loopback with the mock
backend. That is deliberate. The provisioning path reuses the daemon's own
validators, writers and adapter calls, so a test that exercised anything else
would prove nothing about the contract: a rejected document must leave the
device untouched, a delivered account must sign in with the passphrase its
digest was derived from, and a completion marker must mean an association that
actually reported an address.
"""
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DAEMON = ROOT / "build/libreecho-web"
SCHEMA = "libreecho-provision/1"

# Synthetic credentials. They exist only in this process and in the bytes the
# daemon hashes; no case may write either one into an assertion or a fixture.
ADMIN_PASSWORD = "provision-admin-passphrase"
WIFI_PASSPHRASE = "provision-wifi-passphrase"

def free_port():
    """Reserve an ephemeral port from the kernel, then hand it to the daemon.

    Probing and rebinding keeps the cases independent without hard-coding a
    port range that another test on this host might already hold.
    """
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def users_line(username, password):
    """The canonical line, derived the way the contract says it is derived.

    Salt is 32 hex characters of fresh randomness, and the digest is SHA-256
    over the salt text, a colon, and the passphrase -- the same construction
    tools/create-user.sh and the daemon's own hashing use. Deriving it here,
    independently of the daemon, is what makes the later sign-in a real check
    rather than a round trip through the same code twice.
    """
    salt = secrets.token_hex(16)
    digest = hashlib.sha256(f"{salt}:{password}".encode()).hexdigest()
    return f"{username}:sha256:{salt}:{digest}"


def document(**sections):
    return dict({"schema": SCHEMA}, **sections)


class Device:
    """One daemon over one config directory, with the real files around it."""

    def __init__(self, directory, name, port=None):
        self.dir = Path(directory) / name
        self.dir.mkdir(parents=True)
        self.config = self.dir / "web-config.json"
        self.config.write_bytes((ROOT / "config/defaults.json").read_bytes())
        self.config.chmod(0o600)
        self.users = self.dir / "users"
        self.provision = self.dir / "provision.json"
        self.result = self.dir / "provision.result"
        self.marker = Path(f"{self.config}.setup-complete")
        self.port = port or free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.process = None
        self.log_path = self.dir / "web.log"

    # -- file side -------------------------------------------------------
    def deliver(self, body):
        """Write the document the way the installer does: owner-only file."""
        self.provision.write_text(json.dumps(body))
        self.provision.chmod(0o600)

    def deliver_raw(self, text, mode=0o600):
        self.provision.write_text(text)
        self.provision.chmod(mode)

    def result_fields(self):
        if not self.result.exists():
            return {}
        fields = {}
        for line in self.result.read_text().splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                fields[key] = value
        return fields

    def wait_result(self, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.result.exists():
                return self.result_fields()
            time.sleep(0.05)
        raise AssertionError("no provision.result was written")

    def wait_result_value(self, key, value, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.result.exists() and self.result_fields().get(key) == value:
                return self.result_fields()
            time.sleep(0.05)
        raise AssertionError(f"{key} never became {value!r}: {self.result_fields()}")

    def wait_unlinked(self, timeout=5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.provision.exists() and not self.provision.is_symlink():
                return
            time.sleep(0.05)
        raise AssertionError("the provision file was not unlinked")

    def config_json(self):
        return json.loads(self.config.read_text())

    def users_lines(self):
        if not self.users.exists():
            return []
        return [line for line in self.users.read_text().splitlines() if line]

    def log_text(self):
        return self.log_path.read_text(errors="replace") if self.log_path.exists() else ""

    # -- process side ----------------------------------------------------
    def start(self, env=None, users_file=True):
        argv = [str(DAEMON), "--backend", "mock", "--config", str(self.config),
                "--mock-config", str(ROOT / "config/mock-state.json"),
                "--web-root", str(ROOT / "web"), "--listen", f"127.0.0.1:{self.port}",
                "--seed", "42", "--dev-controls"]
        if users_file:
            argv += ["--users-file", str(self.users)]
        child_env = dict(os.environ)
        if env:
            child_env.update(env)
        log = self.log_path.open("ab")
        self.process = subprocess.Popen(argv, env=child_env, stdout=log, stderr=log)
        self._await_ready()
        return self

    def _await_ready(self, timeout=20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                status, _ = self.request("/api/v1/config")
                if status == 200:
                    return
            except OSError:
                pass
            time.sleep(0.05)
        raise AssertionError(f"daemon did not answer: {self.log_text()}")

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()
        return False

    # -- HTTP side -------------------------------------------------------
    def request(self, route, method="GET", csrf=None, body=None):
        headers = {}
        if csrf:
            headers["X-LibreEcho-CSRF"] = csrf
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.url + route, method=method,
                                     headers=headers, data=data)
        try:
            with urllib.request.urlopen(req, timeout=10) as reply:
                return reply.status, reply.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    def config_status(self):
        status, body = self.request("/api/v1/config")
        assert status == 200, status
        return json.loads(body)["data"]

    def setup_completed(self):
        return bool(self.config_status()["setup_completed"])

    def csrf(self):
        return self.config_status()["csrf_token"]

    def login(self, username, password):
        return self.request("/api/v1/auth/login", "POST", self.csrf(),
                            {"username": username, "password": password})

    def wait_complete(self, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.setup_completed():
                return
            time.sleep(0.1)
        raise AssertionError("setup never completed")

    def wait_incomplete(self, settle=2.0):
        """Give a marker a real chance to appear before claiming it did not."""
        time.sleep(settle)
        assert not self.setup_completed(), "setup completed unexpectedly"
        assert not self.marker.exists(), "a setup-complete marker was written"


def assert_no_secret_on_disk(device, label):
    """No plaintext credential anywhere under the case's own directory."""
    for path in device.dir.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        data = path.read_bytes()
        for secret in (ADMIN_PASSWORD, WIFI_PASSPHRASE):
            assert secret.encode() not in data, f"{label}: {secret!r} survived in {path.name}"


def case_absent(directory):
    """No document is not a failure: nothing is written and nothing changes."""
    with Device(directory, "absent") as device:
        device.start()
        assert not device.result.exists(), "a result was written with no document"
        assert not device.users.exists(), "an account appeared with no document"
        device.wait_incomplete(settle=1.0)
    print("absent provision file: noop")


def case_valid(directory):
    """The whole path: account, settings, hand-off, verified completion."""
    line = users_line("provisioner", ADMIN_PASSWORD)
    with Device(directory, "valid") as device:
        device.deliver(document(
            binding={"release": "radar-puffin-v0.14.0", "target": "radar-puffin"},
            admin={"users_line": line},
            wifi={"ssid": "ProvisionNet", "security": "wpa2",
                  "password": WIFI_PASSPHRASE},
            settings={"hostname": "provisioned-echo", "volume": 37,
                      "wake_word": "Alexa", "wake_sensitivity": 61,
                      "privacy_local_only": True, "privacy_telemetry": False}))
        device.start()
        # One-shot: the file is gone as soon as it has been read, and never
        # comes back, whatever the outcome was.
        device.wait_unlinked()

        fields = device.wait_result()
        assert fields["admin"] == "created", fields
        assert fields["settings"] == "applied", fields
        assert fields["wifi"] == "handed-off", fields

        # The delivered line is the only thing that was written, and it is
        # the line the document carried.
        lines = device.users_lines()
        assert len(lines) == 1, lines
        assert lines[0] == line, "the stored account is not the delivered one"
        assert (device.users.stat().st_mode & 0o777) == 0o600

        # The passphrase that the digest was derived from signs in, and only
        # that one does: the account really is the canonical representation.
        status, body = device.login("provisioner", ADMIN_PASSWORD)
        assert status == 200, (status, body)
        payload = json.loads(body)
        assert payload["ok"] and payload["data"]["username"] == "provisioner"
        status, _ = device.login("provisioner", "not-the-passphrase")
        assert status == 401, status

        # Settings merged through the canonical persist path, which writes the
        # hostname with hostname_persisted and the AirPlay bit setup always sets.
        config = device.config_json()
        assert config["hostname"] == "provisioned-echo", config["hostname"]
        assert config["hostname_persisted"] is True
        assert config["volume"] == 37
        assert config["wake_word"] == "Alexa"
        assert config["wake_sensitivity"] == 61
        assert config["privacy_local_only"] is True
        assert config["privacy_telemetry"] is False
        assert config["integrations"] & 16, "the AirPlay bit was not set"

        # Completion waits for an association with an address, and is then
        # recorded by the same marker the wizard writes.
        device.wait_complete()
        assert device.marker.read_text().strip() == "schema=1"
        fields = device.result_fields()
        assert fields["result"] == "applied", fields
        assert fields["error"] == "none", fields

        # The status file carries codes only: no document value of any kind.
        result_text = device.result.read_text()
        for secret in (ADMIN_PASSWORD, WIFI_PASSPHRASE, "ProvisionNet",
                       "provisioned-echo", "Alexa", "provisioner"):
            assert secret not in result_text, f"{secret!r} leaked into provision.result"

        assert_no_secret_on_disk(device, "valid")
        log = device.log_text()
        for secret in (ADMIN_PASSWORD, WIFI_PASSPHRASE):
            assert secret not in log, f"{secret!r} reached the daemon log"
    print("valid document: account created, settings applied, Wi-Fi handed off, setup completed")


def case_rejects(directory):
    """Every rejection path: nothing applied, file unlinked, wizard intact."""
    good = users_line("provisioner", ADMIN_PASSWORD)
    admin = {"users_line": good}
    long_digest = "0123456789abcdef" * 4

    cases = {
        "unknown-key": document(admin=admin, root_password="nope"),
        "unknown-nested-key": document(admin=dict(admin, is_admin=True)),
        "unknown-wifi-key": document(admin=admin,
                                     wifi={"ssid": "N", "security": "wpa2",
                                           "password": "12345678", "psk": "x"}),
        "unknown-settings-key": document(admin=admin, settings={"volume": 10,
                                                                "timezone": "UTC"}),
        # Raw text, not a dict: Python would collapse a repeated key before
        # the document ever reached the daemon.
        "duplicate-key": '{"schema": "%s", "admin": {"users_line": "%s"},'
                         ' "settings": {"volume": 10, "volume": 90}}' % (SCHEMA, good),
        "duplicate-admin": '{"schema": "%s", "admin": {"users_line": "%s"},'
                           ' "admin": {"users_line": "%s"}}' % (SCHEMA, good, good),
        "duplicate-schema": '{"schema": "%s", "schema": "%s", "admin": {"users_line": "%s"}}'
                            % (SCHEMA, SCHEMA, good),
        "wrong-schema": dict(document(admin=admin), schema="libreecho-provision/2"),
        "missing-schema": {"admin": admin},
        "missing-admin": document(settings={"volume": 10}),
        "admin-not-object": {"schema": SCHEMA, "admin": "provisioner"},
        "missing-users-line": {"schema": SCHEMA, "admin": {}},
        "users-line-not-string": {"schema": SCHEMA, "admin": {"users_line": 7}},
        "users-line-wrong-scheme": document(
            admin={"users_line": "provisioner:sha1:abc:def"}),
        "users-line-digest-not-hex": document(
            admin={"users_line": f"provisioner:sha256:0123456789abcdef:{long_digest}x"}),
        "users-line-digest-short": document(
            admin={"users_line": "provisioner:sha256:0123456789abcdef:abcd"}),
        "users-line-salt-too-short": document(
            admin={"users_line": f"provisioner:sha256:abcd:{long_digest}"}),
        "users-line-username-charset": document(
            admin={"users_line": f"provisioner!name:sha256:0123456789abcdef:{long_digest}"}),
        "users-line-username-too-long": document(
            admin={"users_line": f"{'a' * 32}:sha256:0123456789abcdef:{long_digest}"}),
        "users-line-extra-field": document(
            admin={"users_line": f"provisioner:sha256:0123456789abcdef:{long_digest}:extra"}),
        "trailing-hyphen-hostname": document(admin=admin, settings={"hostname": "echo-"}),
        "leading-hyphen-hostname": document(admin=admin, settings={"hostname": "-echo"}),
        "hostname-too-long": document(admin=admin, settings={"hostname": "e" * 64}),
        "hostname-charset": document(admin=admin, settings={"hostname": "echo room"}),
        "security-not-in-enum": document(
            admin=admin, wifi={"ssid": "N", "security": "wpa3", "password": "12345678"}),
        "wifi-password-too-short": document(
            admin=admin, wifi={"ssid": "N", "security": "wpa2", "password": "short"}),
        "wifi-password-too-long": document(
            admin=admin, wifi={"ssid": "N", "security": "wpa2", "password": "x" * 64}),
        "wifi-password-on-open": document(
            admin=admin, wifi={"ssid": "N", "security": "open", "password": WIFI_PASSPHRASE}),
        "wifi-ssid-too-long": document(
            admin=admin, wifi={"ssid": "s" * 33, "security": "wpa2", "password": "12345678"}),
        "wifi-ssid-empty": document(
            admin=admin, wifi={"ssid": "", "security": "wpa2", "password": "12345678"}),
        "wifi-missing-ssid": document(
            admin=admin, wifi={"security": "wpa2", "password": "12345678"}),
        "wifi-bad-security-type": document(
            admin=admin, wifi={"ssid": "N", "security": 2, "password": "12345678"}),
        "volume-out-of-range": document(admin=admin, settings={"volume": 101}),
        "volume-negative": document(admin=admin, settings={"volume": -1}),
        "volume-wrong-type": document(admin=admin, settings={"volume": "50"}),
        "sensitivity-out-of-range": document(admin=admin, settings={"wake_sensitivity": 101}),
        "privacy-wrong-type": document(admin=admin, settings={"privacy_telemetry": "false"}),
        "wake-word-empty": document(admin=admin, settings={"wake_word": ""}),
        "settings-not-object": {"schema": SCHEMA, "admin": admin, "settings": []},
        "binding-unknown-key": document(admin=admin,
                                        binding={"release": "r", "target": "t", "extra": 1}),
        "not-an-object": ["schema"],
        "malformed-json": '{"schema":"libreecho-provision/1","admin":',
        "trailing-content": '{"schema":"libreecho-provision/1","admin":{"users_line":"%s"}}x' % good,
    }

    for label, body in cases.items():
        with Device(directory, f"reject-{label}") as device:
            device.deliver_raw(body if isinstance(body, str) else json.dumps(body))
            device.start()
            device.wait_unlinked()
            fields = device.result_fields()
            assert fields.get("result") == "rejected", (label, fields)
            assert fields.get("admin") == "skipped", (label, fields)
            assert fields.get("wifi") == "absent", (label, fields)
            assert fields.get("settings") == "absent", (label, fields)
            # Nothing applied: no account, and the config is still as shipped.
            assert not device.users.exists(), f"{label}: an account was created"
            config = device.config_json()
            assert config["volume"] == 50, f"{label}: settings changed"
            assert config["wake_word"] == "Alexa", f"{label}: settings changed"
            device.wait_incomplete(settle=0.5)
            assert_no_secret_on_disk(device, label)
    print(f"rejections: {len(cases)} documents refused, nothing applied, each file unlinked")


def case_symlink(directory):
    """A symlink is never followed, and its target is not destroyed either."""
    with Device(directory, "reject-symlink") as device:
        device.deliver(document(admin={"users_line": users_line("provisioner",
                                                               ADMIN_PASSWORD)}))
        real = device.provision.read_text()
        device.provision.unlink()
        target = device.dir / "provision.real"
        target.write_text(real)
        device.provision.symlink_to("provision.real")
        device.start()
        fields = device.result_fields()
        assert fields.get("result") == "rejected", fields
        assert fields.get("error") == "file-symlink", fields
        assert not device.users.exists()
        # The contract protects the delivered path. Removing a symlink must
        # not reach through it and delete whatever it names.
        assert target.exists(), "the symlink target was destroyed"
        device.wait_incomplete(settle=0.5)
    print("symlink: refused, target neither read nor destroyed")


def case_modes(directory):
    """Anything but owner-only 0600 is refused before it is parsed."""
    for mode, label in ((0o644, "world-readable"), (0o640, "group-readable"),
                        (0o604, "world-writable"), (0o600 | 0o4000, "setuid")):
        with Device(directory, f"mode-{oct(mode)[2:]}") as device:
            device.deliver_raw(json.dumps(document(
                admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)})),
                mode=mode)
            device.start()
            fields = device.result_fields()
            assert fields.get("result") == "rejected", (label, fields)
            assert fields.get("error") == "file-mode", (label, fields)
            assert not device.users.exists(), label
            device.wait_incomplete(settle=0.3)
        print(f"mode {oct(mode)[2:]}: refused")


def case_oversize(directory):
    """Past 4096 bytes the document is never parsed at all."""
    with Device(directory, "oversize") as device:
        body = json.dumps(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)}))
        padded = body[:-1] + ',"padding":"' + "x" * 4200 + '"}'
        assert len(padded) > 4096
        device.deliver_raw(padded)
        device.start()
        fields = device.result_fields()
        assert fields.get("result") == "rejected", fields
        assert fields.get("error") == "file-size", fields
        assert not device.users.exists()
        device.wait_incomplete(settle=0.3)
    print("oversize: refused before parsing")

    # The largest accepted document is not refused for its size.
    with Device(directory, "just-under") as device:
        line = users_line("provisioner", ADMIN_PASSWORD)
        padding = 4096 - len(json.dumps(document(admin={"users_line": line}))) - 20
        body = json.dumps(document(admin={"users_line": line}))
        body = body[:-1] + ',"note":"' + "y" * padding + '"}'
        assert len(body) <= 4096, len(body)
        device.deliver_raw(body)
        device.start()
        device.wait_unlinked()
        fields = device.wait_result()
        assert fields["result"] != "rejected" or fields["error"] == "unknown-key", fields
    print("a document at the size ceiling is parsed, not refused for size")


def case_existing_account(directory):
    """An existing account is kept, never replaced or reset."""
    with Device(directory, "keep") as device:
        existing = ROOT / "tools/create-user.sh"
        created = subprocess.run([str(existing), "existing-owner", ADMIN_PASSWORD],
                                 capture_output=True, text=True, check=True).stdout
        device.users.write_text(created)
        device.users.chmod(0o600)
        before = device.users.read_text()

        device.deliver(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)},
            settings={"volume": 23}))
        device.start()
        device.wait_unlinked()
        fields = device.wait_result()
        assert fields["admin"] == "kept", fields
        assert fields["settings"] == "applied", fields
        assert fields["result"] == "partial", fields
        # The existing account's own line is byte-identical: a retry must not
        # reformat the file or reset an account the owner already has.
        assert device.users.read_text() == before
        assert device.config_json()["volume"] == 23
        # It still signs in, which is the part that would break if the file had
        # been rewritten.
        status, _ = device.login("existing-owner", ADMIN_PASSWORD)
        assert status == 200, status
        device.wait_incomplete(settle=1.0)
    print("existing account: kept, byte-identical, still usable")


def case_wifi_absent(directory):
    """No Wi-Fi means nothing that could later prove the network works."""
    with Device(directory, "wifi-absent") as device:
        device.deliver(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)},
            settings={"volume": 44}))
        device.start()
        device.wait_unlinked()
        fields = device.wait_result()
        assert fields["admin"] == "created", fields
        assert fields["settings"] == "applied", fields
        assert fields["wifi"] == "absent", fields
        assert fields["result"] == "partial", fields
        assert fields["error"] == "wifi-absent", fields
        device.wait_incomplete(settle=1.0)
    print("no Wi-Fi: account and settings applied, no marker")


def case_wifi_failed(directory):
    """A refused hand-off is recorded as failed and completes nothing."""
    with Device(directory, "wifi-failed") as device:
        device.deliver(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)},
            wifi={"ssid": "FailNet", "security": "wpa2",
                  "password": WIFI_PASSPHRASE}))
        device.start()
        device.wait_unlinked()
        fields = device.wait_result()
        assert fields["wifi"] == "failed", fields
        assert fields["result"] == "partial", fields
        assert fields["error"] == "wifi-handoff", fields
        device.wait_incomplete(settle=1.0)
        assert_no_secret_on_disk(device, "wifi-failed")
    print("refused Wi-Fi: wifi=failed, no marker")


def case_wifi_timeout(directory):
    """An association that never completes closes on its own bounded window."""
    with Device(directory, "wifi-timeout") as device:
        device.deliver(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)},
            wifi={"ssid": "ProvisionNet", "security": "wpa2",
                  "password": WIFI_PASSPHRASE},
            settings={"volume": 29}))
        device.start(env={"LIBREECHO_PROVISION_ASSOC_TIMEOUT_SECONDS": "1"})
        device.wait_unlinked()
        fields = device.wait_result_value("wifi", "failed", timeout=25)
        assert fields["result"] == "partial", fields
        assert fields["error"] == "assoc-timeout", fields
        # What did apply stays applied: the account and settings are not undone
        # by a network that never came up.
        assert fields["admin"] == "created", fields
        assert fields["settings"] == "applied", fields
        assert device.config_json()["volume"] == 29
        status, _ = device.login("provisioner", ADMIN_PASSWORD)
        assert status == 200, status
        device.wait_incomplete(settle=1.0)
    print("bounded association timeout: wifi=failed, applied state kept")


def case_already_complete(directory):
    """A device whose owner already finished setup is never re-provisioned."""
    with Device(directory, "already-complete") as device:
        created = subprocess.run(
            [str(ROOT / "tools/create-user.sh"), "existing-owner", ADMIN_PASSWORD],
            capture_output=True, text=True, check=True).stdout
        device.users.write_text(created)
        device.users.chmod(0o600)
        device.marker.write_text("schema=1\n")
        device.marker.chmod(0o600)
        before_users = device.users.read_text()
        before_config = device.config.read_bytes()

        device.deliver(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)},
            settings={"volume": 88}))
        device.start()
        device.wait_unlinked()
        fields = device.result_fields()
        assert fields.get("result") == "rejected", fields
        assert fields.get("error") == "already-complete", fields
        assert device.users.read_text() == before_users
        assert device.config.read_bytes() == before_config
        assert device.setup_completed()
    print("already-configured device: document removed, nothing overwritten")


def case_restart(directory):
    """Completion survives a restart, and the document stays consumed."""
    with Device(directory, "restart") as device:
        device.deliver(document(
            admin={"users_line": users_line("provisioner", ADMIN_PASSWORD)},
            wifi={"ssid": "ProvisionNet", "security": "wpa2",
                  "password": WIFI_PASSPHRASE}))
        device.start()
        device.wait_complete()
        users_after_first = device.users.read_text()
        result_after_first = device.result_fields()
        assert result_after_first["result"] == "applied", result_after_first
        device.stop()

        # A restart with the document already consumed applies nothing at all.
        assert not device.provision.exists()
        device.start()
        device.wait_complete()
        assert device.users.read_text() == users_after_first
        assert device.result_fields()["result"] == "applied"
    print("completion survives a restart; the document is not applied twice")


def main():
    if not DAEMON.exists():
        raise SystemExit(f"{DAEMON} is not built")
    cases = [case_absent, case_valid, case_rejects, case_symlink, case_modes,
             case_oversize, case_existing_account, case_wifi_absent,
             case_wifi_failed, case_wifi_timeout, case_already_complete,
             case_restart]
    for case in cases:
        with tempfile.TemporaryDirectory(prefix="le-provision-") as directory:
            case(directory)
    print(f"first-boot provisioning regression: ok ({len(cases)} cases)")


if __name__ == "__main__":
    main()
