#!/usr/bin/env python3
"""Host-only signed Platform-to-C/API integration; no device paths or release keys.

Run after make, with LIBREECHO_PLATFORM_SRC pointing at the companion Platform
checkout. The dedicated hosted lane pins that source to an immutable commit.
"""
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = Path(os.environ.get('LIBREECHO_PLATFORM_SRC', '../LibreEcho-Platform')).resolve()
CASES = ('valid-mixed-signed-authority', 'runtime-signature-invalidation',
         'mounted-airplay-daemon-invalidation', 'payload-invalidation',
         'installed-id-invalidation', 'cold-tampered-system-signature',
         'cold-missing-system-signature', 'cold-tampered-system-manifest',
         'cold-wrong-installed-id', 'cold-partial-runtime-authority',
         'recovery-after-missing-signature', 'authenticated-http')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(folder):
    identities = {}
    sources = ((PLATFORM, 'tools/mt8163-arm32/initramfs/libreecho-feature-transaction'),
               (PLATFORM, 'tools/mt8163-arm32/ota/test_feature_provenance.py'),
               (ROOT, 'src/authority_provenance.c'), (ROOT, 'src/authority_provenance.h'))
    for root, relative in sources:
        source = root / relative
        data = source.read_bytes()
        (folder / source.name).write_bytes(data)
        assert source.read_bytes() == data, f'source changed during capture: {source}'
        identities[str(source)] = {
            'sha256': hashlib.sha256(data).hexdigest(),
            'head': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()}
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                    '-shared', '-fPIC', str(folder / 'authority_provenance.c'),
                    '-o', str(folder / 'authority_provenance.so')], check=True, timeout=30)
    print(json.dumps({'source_identities': identities}), flush=True)


def check_http(fixture):
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    config = fixture.root / 'web-config.json'
    config.write_text('{}\n')
    def request(path, method='GET', body=None, headers=None):
        data = None if body is None else json.dumps(body).encode()
        req = Request(base + path, data=data, method=method,
                      headers={'Content-Type': 'application/json', **(headers or {})})
        try:
            with urlopen(req, timeout=2) as response:
                return response.status, json.load(response)
        except HTTPError as error:
            return error.code, json.load(error)
    with (fixture.root / 'web.log').open('w') as log:
        server = subprocess.Popen([str(ROOT / 'build/libreecho-web'), '--backend', 'mock',
                                   '--config', str(config), '--web-root', str(ROOT / 'web'),
                                   '--listen', f'127.0.0.1:{port}', '--users-file', str(fixture.root / 'users')],
                                  env=os.environ.copy(), stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                status, response = request('/api/v1/config')
                assert status == 200
                break
            except URLError:
                assert time.monotonic() < deadline and server.poll() is None, 'HTTP server not ready'
                time.sleep(0.02)
        assert request('/api/v1/provenance')[0] == 401
        password = secrets.token_urlsafe(24)
        status, response = request('/api/v1/auth/bootstrap', 'POST',
                                   {'username': 'fixture', 'password': password, 'password_confirm': password},
                                   {'X-LibreEcho-CSRF': response['data']['csrf_token']})
        assert status == 200
        auth = {'Authorization': 'Bearer ' + response['data']['token']}
        deadline = time.monotonic() + 10
        while True:
            status, response = request('/api/v1/provenance', headers=auth)
            assert status == 200
            authority = response['data']['authority_provenance']
            if authority['available']:
                break
            assert time.monotonic() < deadline, 'signed HTTP authority never became available'
            time.sleep(0.02)
        validate_authority(authority)
        assert next(f for f in authority['features'] if f['feature_id'] == 'stt')['release'] == '0.13.13'
        fixture.signature.unlink()
        status, response = request('/api/v1/provenance', headers=auth)
        assert status == 200 and not response['data']['authority_provenance']['available']
        validate_authority(response['data']['authority_provenance'])
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)
        assert server.poll() is not None


def validate_authority(value):
    import jsonschema
    document = json.loads((ROOT / 'web/openapi.json').read_text())
    schema = document['components']['schemas']['AuthorityProvenance']
    def json_schema(node):
        if isinstance(node, list):
            return [json_schema(item) for item in node]
        if not isinstance(node, dict):
            return node
        result = {key: json_schema(item) for key, item in node.items() if key != 'nullable'}
        return {'anyOf': [result, {'type': 'null'}]} if node.get('nullable') else result
    jsonschema.validate(value, json_schema(schema))


def child(folder, case):
    module = load_module('signed_fixture', folder / 'test_feature_provenance.py')
    setattr(module, 'TRANSACTION', folder / 'libreecho-feature-transaction')
    fixture = module.ProvenanceFixture()
    os.environ.update(fixture.env)
    os.environ.update({'LIBREECHO_UPDATE_ROOT': str(fixture.update),
                       'LIBREECHO_FEATURE_ROOT': str(fixture.features),
                       'LIBREECHO_FEATURE_RUN_ROOT': str(fixture.run_root),
                       'LIBREECHO_FEATURE_TRANSACTION_HELPER': str(fixture.fixture)})
    lib = ctypes.CDLL(str(folder / 'authority_provenance.so'))
    lib.le_authority_provenance_json.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    lib.le_authority_provenance_json.restype = None
    lib.le_authority_provenance_tick.restype = None
    lib.le_authority_provenance_shutdown.restype = None
    ticks = []
    result = {'case': case, 'passed': False, 'hardware_actions': False}
    def current():
        output = ctypes.create_string_buffer(16384)
        lib.le_authority_provenance_json(output, len(output))
        return json.loads(output.value)
    def tick():
        before = time.monotonic()
        lib.le_authority_provenance_tick()
        ticks.append(time.monotonic() - before)
        return current()
    def producer():
        return module.run([str(fixture.fixture), 'provenance'], fixture.env)
    def wrong_transaction():
        path = fixture.update / 'installed'
        text = path.read_text()
        changed = text.replace('txn-provenance-current', 'txn-provenance-other')
        assert changed != text
        path.write_text(changed)
    try:
        if case == 'authenticated-http':
            check_http(fixture)
        else:
            signature = fixture.signature.read_bytes()
            if case in ('cold-missing-system-signature', 'recovery-after-missing-signature'):
                fixture.signature.unlink()
            elif case == 'cold-tampered-system-signature':
                fixture.signature.write_text('0' * 128 + '\n')
            elif case == 'cold-tampered-system-manifest':
                with (fixture.update / 'committed-manifest').open('a') as stream:
                    stream.write('invalid_signed_field=1\n')
            elif case == 'cold-wrong-installed-id':
                wrong_transaction()
            elif case == 'cold-partial-runtime-authority':
                (fixture.update / 'committed-runtime-stt.sig').unlink()
            negative = case.startswith('cold-') or case == 'recovery-after-missing-signature'
            if negative:
                direct = producer()
                assert direct.returncode != 0 and not direct.stdout, 'producer accepted invalid authority'
                until = time.monotonic() + 1
                while time.monotonic() < until:
                    assert not tick()['available'], 'UI exposed invalid identity'
                    time.sleep(0.01)
                validate_authority(current())
                if case == 'recovery-after-missing-signature':
                    fixture.signature.write_bytes(signature)
            if not case.startswith('cold-'):
                direct = producer()
                assert direct.returncode == 0, direct.stderr
                expected = dict(line.split('=', 1) for line in direct.stdout.splitlines())
                value = {}
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    value = tick()
                    if value['available']:
                        break
                    time.sleep(0.01)
                assert value['available'], 'valid signed authority never became available'
                validate_authority(value)
                features = {f['feature_id']: f for f in value['features']}
                assert len(features) == 5
                assert features['stt']['action'] == 'preserve' and features['stt']['kind'] == 'runtime'
                assert features['stt']['release'] == '0.13.13', 'inherited authority relabeled'
                for feature_id, item in features.items():
                    for field in ('action', 'kind', 'release', 'source_commit', 'payload_sha256', 'manifest_sha256', 'daemon_sha256'):
                        assert item[field] == expected[f'feature_{feature_id}_{field}'], (feature_id, field)
                if case == 'runtime-signature-invalidation':
                    (fixture.update / 'committed-runtime-stt.sig').write_text('0' * 128 + '\n')
                elif case == 'mounted-airplay-daemon-invalidation':
                    (fixture.run_root / 'libreecho/features/airplay2/root' / module.DAEMONS['airplay2']).write_bytes(b'changed daemon')
                elif case == 'payload-invalidation':
                    (fixture.features / 'airplay2/payload.squashfs').write_bytes(b'changed payload')
                elif case == 'installed-id-invalidation':
                    wrong_transaction()
                if case.endswith('-invalidation'):
                    direct = producer()
                    assert direct.returncode != 0 and not direct.stdout
                    assert not current()['available'], 'UI retained verified identity after evidence changed'
                    validate_authority(current())
        assert max(ticks, default=0) < 0.2, 'C tick blocked for >=200ms on tiny host fixture'
        result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
    finally:
        lib.le_authority_provenance_shutdown()
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                result['child_cleanup'] = 'no children remain'
                break
            if pid == 0:
                time.sleep(0.01)
        else:
            result['child_cleanup'] = 'unreaped child remains'
            result['passed'] = False
        root = fixture.root
        fixture.close()
        result['fixture_cleanup'] = not root.exists()
        result['max_tick_seconds'] = max(ticks, default=0)
    print(json.dumps(result))
    return 0 if result['passed'] else 1


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--case':
        assert sys.argv[2] in CASES
        return child(Path(sys.argv[3]), sys.argv[2])
    with tempfile.TemporaryDirectory(prefix='signed-provenance-integration-') as temporary:
        folder = Path(temporary)
        prepare(folder)
        results = []
        for case in CASES:
            run = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--case', case, str(folder)],
                                 capture_output=True, text=True, timeout=40)
            try:
                result = json.loads(run.stdout)
            except ValueError:
                result = {'case': case, 'passed': False, 'error': run.stderr or run.stdout}
            result['exit_code'] = run.returncode
            results.append(result)
            print(json.dumps(result), flush=True)
        assert len(results) == len(CASES)
        return 0 if all(r['passed'] and r['exit_code'] == 0 for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
