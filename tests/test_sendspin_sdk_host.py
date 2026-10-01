#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline opt-in SDK/lifecycle/production-engine lane; never uses hardware.

Requires SENDSPIN_PLATFORM_DIR and SENDSPIN_ARCHIVE_DIR. All sources are
regenerated from verified archives and the Platform lock's inventoried patches
in an isolated scratch directory. No unverified source directory is compiled.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
UI = Path(__file__).resolve().parents[1]

def run(command, *, timeout=180):
    print('+', shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, timeout=timeout)

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f'cannot load fixture module: {path}')
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

def compile_engine(platform, work, sanitize):
    airplay = platform / 'tools/mt8163-arm32/airplay'
    fixture = load('sendspin_engine_host_fixture', airplay / 'test_audio_engine_sendspin.py')
    includes = work / 'include/tinyalsa'
    includes.mkdir(parents=True)
    (includes / 'pcm.h').write_text(fixture.PCM_HEADER)
    (includes / 'mixer.h').write_text(fixture.MIXER_HEADER)
    # Reuse the existing fake TinyALSA + audited syscall wrappers, but execute
    # the real run_engine loop directly for the SDK client in another process.
    source = fixture.LOOP_PROGRAM.split('/* Parent helpers')[0]
    # This split leaves the comment opener of the following section in place.
    source = source[:source.rfind('/* ------------------------------------------------------------------ */')]
    old = '    dump_log(root, 0);\n    _exit(0);'
    if old not in source:
        raise RuntimeError('Platform engine fixture child contract changed')
    source = source.replace(old, '''    dump_log(root, 0);
    {
        char path[800];
        size_t maximum = sizeof(g_written) / sizeof(g_written[0]) / OUTPUT_CHANNELS;
        int fd;
        (void)snprintf(path, sizeof(path), "%s/pcm.raw", root);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || g_written_frames > maximum) _exit(3);
        if (write(fd, g_written, g_written_frames * OUTPUT_CHANNELS * sizeof(int16_t)) !=
            (ssize_t)(g_written_frames * OUTPUT_CHANNELS * sizeof(int16_t))) _exit(4);
        close(fd);
    }
    _exit(g_audit_denied ? 5 : 0);''')
    # A period must consume real model time: an instantaneous fake pcm_writei
    # otherwise runs at the engine poll cadence (not 48 kHz), creating a false
    # clock-drift/priming-only success. This modifies the fake, not run_engine.
    paced = """    notify('W');
    return (int)accepted;"""
    if paced not in source:
        raise RuntimeError('Platform fake PCM write contract changed')
    source = source.replace(paced, """    {
        const uint64_t ns = (uint64_t)accepted * 1000000000u / DEFAULT_RATE;
        struct timespec duration = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
        while (nanosleep(&duration, &duration) != 0 && errno == EINTR && !stopping) {}
    }
    notify('W');
    return (int)accepted;""")
    source += '''
/* Test-only calibration: the fake PCM has a known zero-latency model. This
 * linker wrapper is not part of the production engine or deployed artifact.
 * The production latency-calibrated gate remains closed. */
extern struct le_audio_timing *__real_le_audio_timing_create(
    const struct le_audio_timing_config *config);
struct le_audio_timing *__wrap_le_audio_timing_create(
    const struct le_audio_timing_config *config) {
    struct le_audio_timing_config fixture = *config;
    fixture.output_latency_us = 0;
    fixture.latency_calibrated = 1;
    return __real_le_audio_timing_create(&fixture);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    child_main(argv[1], SCEN_FINISH, -1, -1);
    return 0;
}
'''
    cfile = work / 'engine.c'
    cfile.write_text(source)
    binary = work / 'engine'
    command = [os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
               '-ffunction-sections', '-fdata-sections', '-I', includes.parent, '-I', airplay,
               cfile, *[airplay / name for name in ('audio_sink.c', 'audio_timing.c',
               'aec_reference.c', 'audio_visualizer.c', 'playback_status.c')],
               '-Wl,--gc-sections', '-Wl,--wrap=open', '-Wl,--wrap=open64',
               '-Wl,--wrap=openat', '-Wl,--wrap=connect',
               '-Wl,--wrap=le_audio_timing_create', '-lm', '-o', binary]
    if sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    run(command)
    return binary

def engine_cycle(platform, build, work, sanitize):
    binary = compile_engine(platform, work, sanitize)
    root = work / 'r'
    root.mkdir()
    (root / 'master.volume').write_text('80\n')
    socket = root / 'sendspin.sock'
    if len(os.fsencode(socket)) > 107:
        raise RuntimeError('scratch path exceeds AF_UNIX bound; use a shorter scratch parent')
    engine_log = work / 'engine-output.log'
    with engine_log.open('wb') as output:
        proc = subprocess.Popen([str(binary), str(root)], stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 5
            while not socket.exists():
                if proc.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('real audio engine did not bind its private sink socket')
                time.sleep(0.01)
            client = subprocess.run([str(build / 'sendspin_engine_sdk_tests'), str(socket)],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=30)
            print(client.stdout, end='', flush=True)
            client.check_returncode()
        finally:
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
                raise RuntimeError('production engine did not terminate within teardown bound')
            print(engine_log.read_text(), end='', flush=True)
    if proc.returncode != 0:
        raise RuntimeError(f'production engine fixture exited {proc.returncode}')
    data = (root / 'engine.log').read_bytes()
    magic, count, denied = struct.unpack_from('=III', data)
    if magic != 0x4C4F4F50 or denied != 0 or count > 16384:
        raise RuntimeError('invalid engine audit log or host hardware access attempted')
    events = [struct.unpack_from('=IIIIi', data, 12 + i * 20) for i in range(count)]
    opens = [e for e in events if e[0] == 1]
    closes = [e for e in events if e[0] == 4]
    writes = [e for e in events if e[0] == 3]
    pcm = (root / 'pcm.raw').read_bytes()
    samples = struct.unpack('=' + 'h' * (len(pcm) // 2), pcm)
    nonzero = sum(value != 0 for value in samples)
    result = re.search(r'SDK_ENGINE_RESULT accepted=(\d+) reported=(\d+) completed=(\d+)', client.stdout)
    if not result:
        raise RuntimeError('SDK client omitted its final acceptance evidence')
    accepted, reported, completed = map(int, result.groups())
    if not opens or len(opens) != len(closes) or not writes or not nonzero:
        raise RuntimeError('real loop did not open, render audible PCM, and close its sole fake PCM')
    if accepted < 8192 or accepted != reported or completed != 1:
        raise RuntimeError('SDK acceptance and physically played tail do not agree')
    evidence = dict(accepted=accepted, reported=reported, completed=True,
                    pcm_opens=len(opens), pcm_closes=len(closes), pcm_writes=len(writes),
                    captured_frames=len(pcm)//4, nonzero_samples=nonzero,
                    hardware_path_attempts=denied, physical_hardware=False)
    (work / 'engine-result.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print('production-loop SDK integration PASS:', json.dumps(evidence), flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    missing = [key for key in ('SENDSPIN_PLATFORM_DIR', 'SENDSPIN_ARCHIVE_DIR') if not os.environ.get(key)]
    if missing:
        parser.error('required provisioning: ' + ', '.join(missing))
    platform = Path(os.environ['SENDSPIN_PLATFORM_DIR']).resolve()
    archives = Path(os.environ['SENDSPIN_ARCHIVE_DIR']).resolve()
    tools = platform / 'tools/mt8163-arm32/sendspin'
    # Never fall back to /tmp. Environment TMPDIR, or the user cache, is the
    # only scratch parent; an overlong socket path is an explicit failure.
    parent = Path(os.environ.get('SENDSPIN_HOST_SCRATCH', os.environ.get('TMPDIR',
                  str(Path.home() / '.cache/libreecho-tests')))).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='ssh-', dir=parent))
    print('Sendspin SDK evidence directory:', work, flush=True)
    source = load('sendspin_sources_host', tools / 'verify_sendspin_sources.py')
    lock = source.load_lock(tools / 'SOURCE.lock')
    patches = source.load_patch_inventory(lock, tools / 'SOURCE.lock', tools / 'patches')
    stage = work / 'stage'
    receipt = work / 'receipt.json'
    source.stage(lock, archives, stage, receipt, patches)
    source.verify_receipt(lock, stage, receipt, archives, patch_map=patches)
    print('Verified archive + patch staging:', json.loads(receipt.read_text())['entries']['identity::sdk']['tree_digest'], flush=True)
    build = work / 'build'
    command = ['cmake', '-S', UI / 'src/adapter/sendspin', '-B', build,
               '-DSENDSPIN_PLATFORM_DIR=' + str(platform),
               '-DSENDSPIN_SDK_DIR=' + str(stage / lock['identity::sdk']['path'])]
    for name, variable in [('ArduinoJson', 'ARDUINOJSON'), ('micro-flac', 'MICRO_FLAC'),
                           ('IXWebSocket', 'IXWEBSOCKET'), ('noise-c', 'NOISE_C')]:
        command.append('-DSENDSPIN_' + variable + '_DIR=' + str(stage / lock[name]['path']))
    if args.sanitize:
        flags = '-fsanitize=address,undefined -fno-omit-frame-pointer -g'
        command += ['-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags,
                    '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
    run(command)
    run(['cmake', '--build', build, '-j4'], timeout=300)
    run(['ctest', '--test-dir', build, '--output-on-failure'], timeout=180)
    engine_work = work / 'engine-loop'
    engine_work.mkdir()
    engine_cycle(platform, build, engine_work, args.sanitize)
    print('Sendspin host SDK lane PASS (not ARM, not hardware)', flush=True)
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as exc:
        print('Sendspin host SDK lane FAIL:', exc, file=sys.stderr)
        raise SystemExit(1)
