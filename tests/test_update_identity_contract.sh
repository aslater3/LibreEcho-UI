#!/bin/sh
set -eu
python3 - <<'PY'
import json
import re
from pathlib import Path

# Source contract for the UI half of the update-check identity work: the API
# reads the recorded candidate identity through the bounded, format-checked
# reader only, publishes both keys additively in the existing envelope, and the
# OpenAPI document, API reference, page and test runner all carry them.
api = Path('src/api.c').read_text()
header = Path('src/update_identity.h').read_text()
module = Path('src/update_identity.c').read_text()
ui = Path('web/js/app.js').read_text()
docs = Path('docs/API.md').read_text()
openapi = json.loads(Path('web/openapi.json').read_text())
runner = Path('tests/run_tests.sh').read_text()
makefile = Path('Makefile').read_text()

# --- the reader owns the two keys and enforces the recorded grammar ---------
assert '#define LE_UPDATE_TAG_KEY "resolved_release_tag"' in header
assert '#define LE_UPDATE_SHA_KEY "ota_sha256"' in header
assert 'radar-puffin-' in module
assert 'radar-puffin-build-' in module and 'radar-puffin-nightly-' in module
assert 'hex_run(value, 64)' in module
assert 'len < size' in module and 'sizeof(line)' in module

# --- api.c reads both keys from the check record through that reader --------
assert '#include "update_identity.h"' in api
body = api[api.index('static void update_status_json'):]
body = body[:body.index('static const char*agent_socket_path')]
assert 'update_identity_value("/data/libreecho/update/check-status",' in body
assert 'LE_UPDATE_TAG_KEY,resolved_tag,sizeof(resolved_tag)' in body
assert 'LE_UPDATE_SHA_KEY,ota_sha,sizeof(ota_sha)' in body
assert 'char resolved_tag[LE_UPDATE_TAG_SIZE]="",ota_sha[LE_UPDATE_SHA_SIZE]=""' in body
# The identity keys never go through the unchecked reader, which would publish
# a truncated or malformed value as though the device had resolved it.
assert 'key_from_file("/data/libreecho/update/check-status","resolved_release_tag"' not in api
assert 'key_from_file("/data/libreecho/update/check-status","ota_sha256"' not in api

# --- both values are escaped before they reach the envelope -----------------
escape_tag = 'json_escape(escaped_resolved_tag,sizeof(escaped_resolved_tag),resolved_tag);'
escape_sha = 'json_escape(escaped_ota_sha,sizeof(escaped_ota_sha),ota_sha);'
assert escape_tag in body and escape_sha in body
assert body.index(escape_tag) < body.index('out(r,200,')
assert body.index(escape_sha) < body.index('out(r,200,')
assert '"\\"resolved_release_tag\\":\\"%s\\",\\"ota_sha256\\":\\"%s\\",' in body
# Additive: the keys sit beside the existing version fields in the same
# success envelope, and the error envelope shape is untouched.
assert '"\\"installed_version\\":\\"%s\\",\\"latest_version\\":\\"%s\\",' in body
assert 'out(r,200,"{\\"ok\\":true,\\"data\\":{\\"supported\\":%s,' in body

# --- OpenAPI documents both additive string properties ----------------------
get = openapi['paths']['/system/update']['get']
props = get['responses']['200']['content']['application/json']['schema']
props = props['allOf'][1]['properties']['data']['properties']
assert 'authority_provenance' in props, 'existing properties must be preserved'
for key in ('resolved_release_tag', 'ota_sha256'):
    assert key in props, f'{key} missing from the OpenAPI response schema'
    assert props[key]['type'] == 'string'
    assert 'empty' in props[key]['description']
assert 'radar-puffin-(build|nightly)-<7 hex>-<16 hex>-<16 hex>' in \
    props['resolved_release_tag']['description']
assert '64' in props['ota_sha256']['description']
assert 'resolved_release_tag and ota_sha256 are additive keys' in get['description']

# --- the API reference documents the fields and the honesty rule ------------
assert '#### GET /api/v1/system/update' in docs
assert '"resolved_release_tag"' in docs and '"ota_sha256"' in docs
assert 'radar-puffin-(build|nightly)-<7 hex>-<16 hex>-<16 hex>' in docs
assert 'treat an absent key, an empty value' in docs
assert 'never\ntruncated into a plausible-looking tag or digest' in docs
# The new section must not swallow the channel endpoint that follows it.
assert docs.index('#### GET /api/v1/system/update') < \
    docs.index('#### PUT /api/v1/system/update/channel')

# --- the page renders both identity fields escaped and honestly -------------
assert 'function updateIdentity(ota){' in ui
assert "const tag=String(ota&&ota.resolved_release_tag||'').trim();" in ui
assert "const digest=String(ota&&ota.ota_sha256||'').trim();" in ui
assert 'const identity=updateIdentity(ota);' in ui
assert '<dt>Release tag</dt><dd class="mono wrap">${esc(identity.tag)}</dd>' in ui
assert '<dt>OTA SHA-256</dt><dd class="mono wrap">${esc(identity.digest)}</dd>' in ui
assert "(identity.text?': '+esc(identity.text)+'.':'.')" in ui
# Each row is gated on its own value, so an older device or a failed check
# leaves the version-only message standing instead of printing "undefined".
assert '${identity.tag?`<dt>Release tag</dt>' in ui
assert '${identity.digest?`<dt>OTA SHA-256</dt>' in ui
# Whitespace-only or otherwise unusable values are trimmed to nothing, so the
# page decides on the value it would actually render.
assert "const tag=String(ota&&ota.resolved_release_tag||'').trim();" in ui
assert "const digest=String(ota&&ota.ota_sha256||'').trim();" in ui
# The raw record fields never reach the DOM unescaped or without that guard.
assert not re.search(r'\$\{[^`]*ota\.resolved_release_tag', ui)
assert not re.search(r'\$\{[^`]*ota\.ota_sha256', ui)

# --- the tests are wired into the real runner and the build -----------------
assert 'make build/test-update-identity' in runner
assert './build/test-update-identity' in runner
assert 'sh tests/test_update_identity_contract.sh' in runner
assert 'node tests/test_update_identity_ui.js' in runner
assert 'tests/test_update_identity.c' in makefile
assert 'src/update_identity.c' in makefile
print('update identity contract: ok')
PY
