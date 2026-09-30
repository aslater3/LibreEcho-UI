"""Historical runner entry: real native ESPHome listener/client lease lifecycle.

The owner fixture uses production mdns_client/mdns_lease, private loopback TCP,
private seqpacket control and stub responder children. It does not exercise
hardware, the ESPHome message codec, or real Avahi network publication.
"""
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

def await_condition(test, condition, seconds=4):
    deadline = time.monotonic() + seconds
    while not condition() and time.monotonic() < deadline:
        time.sleep(0.02)
    test.assertTrue(condition())

OWNER = r'''#define _POSIX_C_SOURCE 200809L
#include "mdns_client.h"
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
int main(int argc, char **argv) {
    struct sockaddr_in address = {0};
    struct timespec pause = {0, 20000000};
    struct le_mdns_esphome_metadata metadata = {
        "2026.9.0", "02:00:00:00:00:01", "LibreEcho", "radar-puffin", "wifi",
        "Native listener café", LE_MDNS_ESPHOME_NOISE, "LibreEcho", "0.14.0"
    };
    int listener, lease = -1;
    unsigned int port;
    if (argc != 3) return 2;
    port = (unsigned int)strtoul(argv[1], NULL, 10);
    if (!port || port > 65535) return 2;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 4) < 0) return 1;
    signal(SIGTERM, stop); signal(SIGINT, stop);
    /* Crucially register only after the listener succeeds. */
    while (running) {
        if (lease < 0) lease = le_mdns_register_esphome(argv[2], port, &metadata);
        else if (le_mdns_receive(lease) < 0) { close(lease); lease = -1; }
        nanosleep(&pause, NULL);
    }
    if (lease >= 0) close(lease);
    close(listener); return 0;
}
'''

class ESPHomeListener(unittest.TestCase):
    def test_live_listener_owns_registration_and_recovers_supervisor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            services = root / 'etc/avahi/services'
            services.mkdir(parents=True)
            (root / 'run').mkdir()
            fake = root / 'chroot'
            fake.write_text('#!/bin/sh\ntrap "exit 0" TERM INT\ntrap : HUP\nwhile :; do sleep 0.05; done\n')
            fake.chmod(0o755)
            consumer_source = root / 'owner.c'
            consumer_source.write_text(OWNER)
            owner = root / 'libreecho-esphomed'
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'src/adapter'), str(consumer_source),
                str(ROOT / 'src/adapter/mdns_client.c'), str(ROOT / 'src/adapter/mdns_lease.c'),
                '-o', str(owner)], check=True, capture_output=True, timeout=30)
            mdns = root / 'mdnsd'
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                            '-DMDNS_CHROOT="' + str(fake) + '"',
                            '-DMDNS_OWNER_EXE="' + str(owner) + '"',
                            str(ROOT / 'src/adapter/mdnsd.c'),
                            str(ROOT / 'src/adapter/mdns_lease.c'), '-o', str(mdns)],
                           check=True, capture_output=True, timeout=30)
            control = root / 'mdns.sock'
            command = [str(mdns), '--root', str(root), '--socket', str(control)]
            supervisor = subprocess.Popen(command)
            consumer = None
            try:
                await_condition(self, control.exists)
                self.assertFalse(list(services.glob('*.service')))
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', 0))
                    port = probe.getsockname()[1]
                    # A failed listener must not lease a record.
                    failed = subprocess.run([str(owner), str(port), str(control)], timeout=3)
                    self.assertNotEqual(failed.returncode, 0)
                    self.assertFalse(list(services.glob('*.service')))
                consumer = subprocess.Popen([str(owner), str(port), str(control)],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                await_condition(self, lambda: bool(list(services.glob('esphome-*.service'))))
                self.assertIsNone(consumer.poll())
                self.assertEqual(len(list(services.glob('*.service'))), 1)
                self.assertIn(f'<port>{port}</port>', next(services.glob('*.service')).read_text())
                with socket.create_connection(('127.0.0.1', port), timeout=2):
                    pass
                # Shared supervisor restart does not require listener restart.
                supervisor.terminate(); supervisor.wait(timeout=3)
                await_condition(self, lambda: not list(services.glob('*.service')))
                supervisor = subprocess.Popen(command)
                await_condition(self, control.exists)
                await_condition(self, lambda: bool(list(services.glob('*.service'))))
                self.assertEqual(len(list(services.glob('*.service'))), 1)
                self.assertIsNone(consumer.poll())
                consumer.terminate(); consumer.communicate(timeout=3)
                self.assertEqual(consumer.returncode, 0)
                await_condition(self, lambda: not list(services.glob('*.service')))
            finally:
                if consumer and consumer.poll() is None:
                    consumer.terminate(); consumer.communicate(timeout=3)
                if supervisor.poll() is None:
                    supervisor.terminate(); supervisor.wait(timeout=3)

if __name__ == '__main__':
    unittest.main()
