#!/usr/bin/env python3
"""Everything that ships must actually be installed.

libreecho-watchdogd was built, tested and given an init script, and the init
script was not in the install target. The daemon would have landed on the
device with nothing to start it: a service that exists, passes its tests, and
never runs. Nothing catches that -- the build is green either way.

So: every init script is installed, every installed daemon has one, and a
daemon that deliberately has no init script has to say so here.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAKEFILE = os.path.join(ROOT, "Makefile")
INIT = os.path.join(ROOT, "init")

# Built, but not started by an init script of their own -- with the reason.
NO_INIT_SCRIPT = {
    # micd spawns it as part of the capture chain.
    "libreecho-capture-mux",
    # Selected by libreecho-wyomingd.init, not started directly.
    "libreecho-sttd-wyoming",
    "libreecho-ttsd-wyoming",
}

# Shipped some other way than the init/ directory.
NOT_AN_INIT_SCRIPT = {
    # A systemd unit, for hosts that use it instead of sysvinit.
    "libreecho-web.service",
}


def makefile_text():
    return open(MAKEFILE).read().replace("\\\n", " ")


def main():
    text = makefile_text()
    failures = []

    install = re.search(r"^install:.*?(?=\n\n|\nclean:)", text, re.S | re.M)
    if not install:
        print("could not find the install target", file=sys.stderr)
        return 1
    installed_scripts = set(re.findall(r"init/(libreecho-[\w.-]+\.init)",
                                       install.group(0)))

    targets = re.search(r"^ADAPTER_TARGETS\s*=(.*)$", text, re.M)
    if not targets:
        print("could not find ADAPTER_TARGETS", file=sys.stderr)
        return 1
    daemons = set(re.findall(r"libreecho-[\w-]+", targets.group(1)))
    # Installed by their own rules rather than through ADAPTER_TARGETS.
    daemons |= {"libreecho-web", "libreecho-logd", "libreecho-waked"}

    on_disk = {f for f in os.listdir(INIT) if f not in NOT_AN_INIT_SCRIPT}

    for script in sorted(on_disk):
        if script not in installed_scripts:
            failures.append(
                "init/%s exists but the install target does not install it; "
                "the daemon would ship with nothing to start it" % script)

    for script in sorted(installed_scripts):
        if script not in on_disk:
            failures.append(
                "the install target installs init/%s, which does not exist"
                % script)

    for script in sorted(installed_scripts & on_disk):
        daemon = script[:-len(".init")]
        if daemon not in daemons:
            failures.append(
                "init/%s starts %s, which nothing builds" % (script, daemon))

    for daemon in sorted(daemons):
        if daemon + ".init" not in on_disk and daemon not in NO_INIT_SCRIPT:
            failures.append(
                "%s is built and installed but has no init script "
                "(add one, or list it in NO_INIT_SCRIPT with a reason)"
                % daemon)

    if failures:
        for failure in failures:
            print("install completeness: " + failure, file=sys.stderr)
        return 1

    print("install completeness: ok (%d services)" % len(installed_scripts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
