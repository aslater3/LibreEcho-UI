#!/usr/bin/env python3
"""Focused contract checks for the bounded feature provenance UI slice."""
from pathlib import Path

api = Path("src/api.c").read_text()
formatter = Path("src/feature_provenance.c").read_text()
diag = Path("src/diagnostic_export.c").read_text()
app = Path("web/js/app.js").read_text()
docs = Path("docs/API.md").read_text()
openapi = Path("web/openapi.json").read_text()

for field in (
    "components",
    "feature_id",
    "release",
    "source_commit",
    "effective_payload_sha256",
    "runtime_capsule_sha256",
    "candidate_kind",
    "candidate_payload_sha256",
    "candidate_status",
    "running_daemon_sha256",
    "running_daemon_status",
    "effective",
    "activation",
    "last_transaction_result",
):
    assert field in formatter, field
    assert field in app, field
    assert field in docs, field
    assert field in openapi, field

assert "components" in api
assert "components" in diag
assert "commit_pending" in formatter
assert "reboot_required" in formatter
assert "running_daemon_sha256" in formatter
assert "effective_payload_sha256" in formatter
assert "rollback" in app
assert "sidebar-version" in app
assert "unavailable" in app
assert "512 MiB" in docs
assert "536870912" in docs
assert "512 MiB" in openapi
assert "536870912" in openapi
assert "262144" in docs and "65536" in docs and "8192 bytes inclusive" in docs
assert "Mutable installed manifests are not provenance authority" in openapi
assert "LE_FEATURE_CONTROL_MAX" in formatter
assert "LE_FEATURE_RECORD_MAX" in formatter
assert "LE_HASH_PENDING" in formatter
assert "le_feature_provenance_tick" in Path("src/http_server.c").read_text()
print("feature provenance contract: ok")
