"""Shared private build of the actual daemon for control/init regression runners."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def daemon_binary(directory):
    supplied = os.environ.get("ESPHOMED_BIN")
    if supplied:
        binary = Path(supplied).resolve()
        if not os.access(binary, os.X_OK):
            raise RuntimeError("ESPHOMED_BIN is not executable")
        return binary
    prefix = os.environ.get("ESPHOMED_TEST_TLS_PREFIX")
    if not prefix:
        raise RuntimeError("Set ESPHOMED_BIN or ESPHOMED_TEST_TLS_PREFIX to the approved mbedTLS prefix")
    tls = Path(prefix)
    binary = Path(directory) / "libreecho-esphomed"
    sources = ["esphomed", "esphome_proto", "esphome_frame", "esphome_noise", "esphome_playback",
               "radio_resample", "mdns_client", "mdns_lease"]
    command = [os.environ.get("CC", "cc"), "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-O1",
               "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation", "-I" + str(tls / "include")]
    command += ["src/adapter/" + source + ".c" for source in sources] + ["src/config_store.c", "src/json.c"]
    command += [str(tls / "lib" / ("lib" + name + ".a")) for name in ("mbedtls", "mbedx509", "mbedcrypto")]
    subprocess.run(command + ["-lm", "-o", str(binary)], cwd=ROOT, check=True, timeout=60)
    return binary
