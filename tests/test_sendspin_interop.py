#!/usr/bin/env python3
"""Two-player Sendspin synchronization lane (host only; never hardware).

Two complete players, each the real audio engine on the realtime fake PCM
plus a host-built sendspind, are grouped by the pinned aiosendspin server,
which streams a click every second. Each fake PCM logs the true
CLOCK_MONOTONIC time at which every click leaves its fake DAC, optionally
with its DAC crystal running fast or slow (LE_FAKE_PCM_PPM). The lane checks
that every click plays on both players and that, once the SDKs have
converged, the two players stay within the scenario's bound.

Requires SENDSPIN_PLATFORM_DIR, SENDSPIN_ARCHIVE_DIR and
SENDSPIN_SERVER_PYTHON (a Python with aiosendspin's dependencies, including
its "server" extra; the aiosendspin code itself is the archive-verified pin).
Not acoustic evidence: both players share the host clock, and only the DAC
crystal error is modelled.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time

UI = Path(__file__).resolve().parents[1]
CLICKS_SKIPPED = 3          # SDK convergence after stream start


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def onsets(path):
    if not path.exists():
        return []
    return [float(line.split('=', 1)[1]) for line in path.read_text().split() if line.startswith('onset_ns=')]


def pair_offsets(a, b, window_ns=400e6):
    """Offset (b - a, ms) for each click of a that b also played."""
    out = []
    for t in a:
        nearest = min(b, key=lambda x: abs(x - t), default=None)
        if nearest is not None and abs(nearest - t) < window_ns:
            out.append((nearest - t) / 1e6)
    return out


def start_player(host, engine, sendspind, work, name, port, ppm, profile, extra_us=0):
    root = work / name
    root.mkdir()
    (root / 'master.volume').write_text('80\n')
    log = work / f'{name}-onsets.log'
    env = dict(os.environ, LE_AUDIO_ENGINE_SENDSPIN_PROFILE_DIR=str(profile),
               LE_FAKE_PCM_REALTIME='1', LE_FAKE_PCM_PPM=str(ppm),
               LE_FAKE_PCM_ONSET_LOG=str(log), LE_FAKE_PCM_EXTRA_LATENCY_US=str(extra_us))
    engine_out = (work / f'{name}-engine.log').open('wb')
    engine_proc = subprocess.Popen([str(engine), str(root)], stdout=engine_out, stderr=engine_out, env=env)
    socket = root / 'sendspin.sock'
    deadline = time.monotonic() + 10
    while not socket.exists():
        if engine_proc.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError(f'{name}: engine did not bind its sink')
        time.sleep(0.02)
    player_out = (work / f'{name}-sendspind.log').open('wb')
    player = subprocess.Popen([str(sendspind), '--run', '--dev-allow-unpaired', '--quiet',
                               '--socket', str(socket), '--port', str(port), '--name', name,
                               '--control-socket', ''],
                              stdout=player_out, stderr=player_out)
    return [player, engine_proc], log


def stop(procs):
    for proc in procs:
        if proc.poll() is None:
            proc.terminate()
    for proc in procs:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def wait_listening(port, deadline_s=15):
    end = time.monotonic() + deadline_s
    hex_port = f':{port:04X} '
    while time.monotonic() < end:
        for table in ('/proc/net/tcp', '/proc/net/tcp6'):
            try:
                if any(hex_port in line and line.split()[3] == '0A' for line in open(table).readlines()[1:]):
                    return
            except OSError:
                pass
        time.sleep(0.05)
    raise RuntimeError(f'player on port {port} never listened')


def scenario(args, ctx, label, ppm_a, ppm_b, seconds, bound_ms, extra_b_us=0, expect_ms=None):
    work = ctx['work'] / label
    work.mkdir()
    procs = []
    try:
        pa, log_a = start_player(*ctx['players'], work, 'player-a', 19431, ppm_a, ctx['profile'])
        procs += pa
        pb, log_b = start_player(*ctx['players'], work, 'player-b', 19432, ppm_b, ctx['profile'], extra_b_us)
        procs += pb
        wait_listening(19431)
        wait_listening(19432)
        env = dict(os.environ, PYTHONPATH=str(ctx['server_tree']))
        server = subprocess.run(
            [args.server_python, str(ctx['probe']), 'ws://127.0.0.1:19431/sendspin',
             'ws://127.0.0.1:19432/sendspin', '--seconds', str(seconds), '--tone', '0',
             '--level-db', '-6'],
            env=env, capture_output=True, text=True, timeout=seconds + 90)
        (work / 'server.log').write_text(server.stdout + server.stderr)
        if server.returncode != 0:
            raise RuntimeError(f'{label}: server failed:\n{server.stderr[-2000:]}')
    finally:
        stop(procs)
    a, b = onsets(log_a), onsets(log_b)
    # The players advertise FLAC first: the pinned server must stream FLAC,
    # which the SDK decodes before the engine sink.
    codecs = {}
    for name in ('player-a', 'player-b'):
        text = (work / f'{name}-sendspind.log').read_text(errors='replace')
        codecs[name] = sorted(set(m.split(',')[0].strip() for m in
                                  text.split('Processed new codec header:')[1:]))
    offsets = pair_offsets(a, b)
    settled = offsets[CLICKS_SKIPPED:]
    result = dict(scenario=label, ppm=[ppm_a, ppm_b], clicks_sent=seconds, clicks_a=len(a),
                  clicks_b=len(b), paired=len(offsets),
                  settled_median_ms=round(statistics.median(settled), 3) if settled else None,
                  settled_max_abs_ms=round(max(abs(x) for x in settled), 3) if settled else None,
                  bound_ms=bound_ms, codecs=codecs, offsets_ms=[round(x, 3) for x in offsets])
    print('interop', json.dumps(result), flush=True)
    problems = []
    if any(c != ['flac'] for c in codecs.values()):
        problems.append(f'stream was not FLAC: {codecs}')
    if min(len(a), len(b), len(offsets)) < seconds:
        problems.append(f'clicks lost (sent {seconds}, a {len(a)}, b {len(b)}, paired {len(offsets)})')
    if expect_ms is not None:
        # Detector check: a known uncompensated offset must be measured.
        if not settled or abs(result['settled_median_ms'] - expect_ms) > bound_ms:
            problems.append(f'did not measure the injected {expect_ms} ms offset')
    elif not settled or result['settled_max_abs_ms'] > bound_ms:
        problems.append(f'players drifted apart beyond {bound_ms} ms')
    return result, problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--server-python', default=os.environ.get('SENDSPIN_SERVER_PYTHON'))
    args = parser.parse_args()
    missing = [k for k in ('SENDSPIN_PLATFORM_DIR', 'SENDSPIN_ARCHIVE_DIR') if not os.environ.get(k)]
    if missing or not args.server_python:
        parser.error('required: SENDSPIN_PLATFORM_DIR, SENDSPIN_ARCHIVE_DIR, SENDSPIN_SERVER_PYTHON')
    platform = Path(os.environ['SENDSPIN_PLATFORM_DIR']).resolve()
    archives = Path(os.environ['SENDSPIN_ARCHIVE_DIR']).resolve()
    host = load('sendspin_sdk_host_lane', UI / 'tests/test_sendspin_sdk_host.py')
    parent = Path(os.environ.get('TMPDIR', str(Path.home() / '.cache/libreecho-tests'))).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='ssi-', dir=parent))
    print('Sendspin interop evidence directory:', work, flush=True)
    build, stage, lock = host.build_sdk_tree(platform, archives, work, False, targets=['sendspind'])
    engine_work = work / 'engine'
    engine_work.mkdir()
    engine = host.compile_engine(platform, engine_work, False)
    sys.path.insert(0, str(platform / 'tools/mt8163-arm32'))
    import libreecho_platform_targets as targets
    profile = work / 'profile'
    profile.mkdir()
    (profile / 'target').write_bytes(targets.identity_bytes('radar_puffin'))
    (profile / 'audio-profile').write_bytes(targets.audio_profile_bytes('radar_puffin'))
    ctx = dict(work=work, profile=profile, players=(host, engine, build / 'sendspind'),
               server_tree=stage / lock['identity::server_oracle']['path'],
               probe=platform / 'tools/mt8163-arm32/sendspin/hotstage/ss_probe_serve.py')
    failures = []
    for label, ppm_a, ppm_b, bound in [('same-clock', 0, 0, 1.0),
                                       ('drift-200ppm', 100, -100, 2.0)]:
        _, problems = scenario(args, ctx, label, ppm_a, ppm_b, args.seconds, bound)
        failures += [f'{label}: {p}' for p in problems]
    # The lane must see a real offset: player B's output is 3 ms later than
    # its engine believes (an output-latency calibration error).
    _, problems = scenario(args, ctx, 'detects-3ms-latency-error', 0, 0, min(args.seconds, 15),
                           0.5, extra_b_us=3000, expect_ms=3.0)
    failures += [f'detects-3ms-latency-error: {p}' for p in problems]
    if failures:
        print('Sendspin interop FAIL:', '; '.join(failures), file=sys.stderr)
        return 1
    print('Sendspin interop PASS (host clocks, fake DACs; not acoustic)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
