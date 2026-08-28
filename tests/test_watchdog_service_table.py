#!/usr/bin/env python3
"""The watchdog's service table must match the init scripts.

This is a source contract, not behaviour. The table names a probe path per
service; if it names a path the daemon does not actually serve, the watchdog
declares a healthy service dead and restarts it until it gives up -- the exact
failure a watchdog exists to avoid. That bug was in the first version of this
table (networkd.sock for a daemon that serves network.sock) and nothing in the
build or the unit tests could see it, because both files were internally
consistent. Only comparing them catches it.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "src", "adapter", "watchdogd.c")
INIT = os.path.join(ROOT, "init")

# Services that must never be restarted on their own, and the group they
# belong to. waked is micd's only microphone-stream consumer and exits when
# that stream ends, so restarting micd alone takes the wake word down until
# the next reboot. Restarting waked alone does not work either -- micd offers
# the stream once, so the second waked gets "microphone stream: Protocol
# error". Only restarting both, micd first, leaves a working wake word. This
# rule exists so a later edit cannot quietly separate them.
NEVER_ALONE = {"waked": "capture", "micd": "capture"}

# Daemons with no init script of their own, or deliberately not supervised.
NOT_SUPERVISED = {
    # The web UI is started by its own unit and is not an adapter daemon.
    "web",
    # The watchdog does not supervise itself; something else would have to.
    "watchdogd",
}

ENTRY = re.compile(
    r'\{"(?P<name>\w+)",\s*(?P<kind>PROBE_SOCKET|PROBE_PIDFILE),\s*'
    r'"(?P<path>[^"]+)",\s*(?:\n\s*)?(?P<init>"[^"]+"|NULL),\s*'
    r'(?P<restart>[01]),\s*(?P<group>"[^"]+"|NULL)',
)


def table_entries(text):
    start = text.index("static const struct service_desc descriptors[MAX_SERVICES]")
    end = text.index("static struct supervised services", start)
    return [m.groupdict() for m in ENTRY.finditer(text[start:end])]


def init_defaults(path):
    text = open(path).read()
    socket = re.search(r"^SOCKET=\$\{SOCKET:-([^}]+)\}", text, re.M)
    pidfile = re.search(r"^PIDFILE=(?:\$\{PIDFILE:-)?([^}\n]+)\}?", text, re.M)
    return (socket.group(1) if socket else None,
            pidfile.group(1) if pidfile else None)


def main():
    text = open(TABLE).read()
    entries = table_entries(text)
    failures = []

    if not entries:
        print("could not parse the service table", file=sys.stderr)
        return 1

    scripts = {
        f[len("libreecho-"):-len(".init")]: os.path.join(INIT, f)
        for f in os.listdir(INIT)
        if f.startswith("libreecho-") and f.endswith(".init")
    }

    for entry in entries:
        name = entry["name"]
        script = scripts.get(name)
        if script is None:
            failures.append("%s: no init script init/libreecho-%s.init" % (name, name))
            continue

        socket, pidfile = init_defaults(script)
        want = socket if entry["kind"] == "PROBE_SOCKET" else pidfile
        if want is None:
            failures.append(
                "%s: probed as %s but its init script defines no such path"
                % (name, entry["kind"]))
        elif want != entry["path"]:
            failures.append(
                "%s: watchdog probes %s, init script uses %s"
                % (name, entry["path"], want))

        # A socket-serving daemon must not be probed by pidfile: the pidfile
        # cannot tell a wedged daemon from a working one.
        if entry["kind"] == "PROBE_PIDFILE" and socket:
            failures.append(
                "%s: serves %s but is probed by pidfile" % (name, socket))

        if name in NEVER_ALONE:
            want_group = '"%s"' % NEVER_ALONE[name]
            if entry["group"] != want_group:
                failures.append(
                    "%s must be in group %s so it is never restarted alone "
                    "(see NEVER_ALONE)" % (name, want_group))
            elif len([e for e in entries if e["group"] == want_group]) < 2:
                failures.append(
                    "%s is alone in group %s, so a group restart is a solo "
                    "restart (see NEVER_ALONE)" % (name, want_group))
            if entry["init"] == "NULL":
                failures.append(
                    "%s names no init script, so its group cannot restart it"
                    % name)

        if entry["init"] != "NULL":
            init_path = entry["init"].strip('"')
            expected = "/etc/init.d/libreecho-%s.init" % name
            if init_path != expected:
                failures.append(
                    "%s: restarts via %s, expected %s"
                    % (name, init_path, expected))
            if entry["restart"] != "1":
                failures.append(
                    "%s: names an init script but is marked unrestartable"
                    % name)
        elif entry["restart"] != "0":
            failures.append(
                "%s: marked restartable but names no init script" % name)

    # Every daemon that ships must be supervised, or the omission must be
    # deliberate. A new daemon should not be able to arrive unsupervised
    # simply because nobody remembered this file.
    supervised = {e["name"] for e in entries}
    for name in sorted(scripts):
        if name not in supervised and name not in NOT_SUPERVISED:
            failures.append(
                "%s ships an init script but nothing supervises it "
                "(add it, or list it in NOT_SUPERVISED with a reason)" % name)

    if failures:
        for failure in failures:
            print("watchdog service table: " + failure, file=sys.stderr)
        return 1

    print("watchdog service table: ok (%d services)" % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
