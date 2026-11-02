#!/usr/bin/env python3
"""Wiring contract for owner HTTPS certificates and the private CA trust bundle.

The PEM validation in src/tls.c needs mbedTLS, which the host build does not
have; the CI TLS lane compiles and exercises it. This file checks that the
declarations, the stub, the routes, the boot path and the outbound consumers
all agree, and that verification is never turned off.
"""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def read(path):
    return (root / path).read_text(encoding="utf-8")


tls_h = read("src/tls.h")
tls_c = read("src/tls.c")
stub = read("src/tls_stub.c")
api_h = read("src/api.h")
api = read("src/api.c")
main = read("src/main.c")
openapi = json.loads(read("web/openapi.json"))
docs = read("docs/API.md")

for fn in ("le_tls_check_certificate_pair", "le_tls_check_ca_bundle",
           "le_tls_install_file", "le_tls_available"):
    assert fn in tls_h, fn
    assert fn in stub, f"stub missing {fn}"
    assert fn in tls_c, f"tls.c missing {fn}"

assert 'LE_TLS_CUSTOM_MARKER "https-custom-certificate"' in tls_h
# Validation rules: key must match the leaf, certificate must not be expired,
# and a private CA bundle may hold CA certificates only.
assert "mbedtls_pk_check_pair(&crt.pk, &key" in tls_c
assert "mbedtls_x509_time_is_past(&crt.valid_to)" in tls_c
assert "mbedtls_x509_crt_get_ca_istrue(c) != 1" in tls_c
# Stub builds report unavailable instead of pretending validation passed.
assert "return LE_NOT_SUPPORTED;" in stub.split("le_tls_check_certificate_pair", 1)[1][:400]

assert "api_set_tls_dir(&api,tls_dir);" in main
assert "static int tls_ensure_certificate(" in main
assert "tls_ensure_certificate(tls_dir,o.tls_cert,o.tls_key)" in main

assert "void api_set_tls_dir(struct api_context*c" in api
assert "char tls_dir[320];" in api_h
assert 'strcmp(q->path,"/api/v1/https/certificate")' in api
assert 'strcmp(q->path,"/api/v1/https/private-ca")' in api
# Outbound verification stays on: the bundle is the system roots plus the CA,
# and news keeps --cacert (never --insecure).
assert "trust-bundle.pem" in api
assert "--insecure" not in api and '"-k"' not in api
assert "news_ca_path(ca,sizeof(ca))" in api

for init in ("init/libreecho-agentd.init", "init/libreecho-lived.init",
             "init/libreecho-esphomed.init"):
    text = read(init)
    assert "/data/libreecho/config/trust-bundle.pem" in text, init
    assert "-k " not in text and "--insecure" not in text, init

paths = openapi["paths"]
assert set(paths["/https/certificate"]) == {"get", "put", "delete"}
assert set(paths["/https/private-ca"]) == {"get", "put", "delete"}
assert "### HTTPS certificate and private CA" in docs
print("https trust contract: ok")
