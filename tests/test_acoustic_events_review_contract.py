#!/usr/bin/env python3
"""Regression contracts for acoustic feature update and restore paths.

These checks keep the validation and application ordering explicit because the
USB role switch is unavailable on ordinary host test machines.  A request that
names both settings must validate every boolean before the USB branch can
return, while a malformed acoustic value must return before either setting is
changed.
"""
from pathlib import Path

api_c = Path("src/api.c").read_text(encoding="utf-8")
features_start = api_c.index('if(!strcmp(p,"/api/v1/system/features"))')
features_end = api_c.index('if(!strcmp(p,"/api/v1/integrations/radio/play")', features_start)
features = api_c[features_start:features_end]

feature_keys = ("simulation", "https", "acoustic_events", "usb_host")
for key in feature_keys:
    marker = f'json_get_top_level_bool(q->body,q->body_len,"{key}"'
    assert marker in features, f"{key} must be read only from the top level"
    duplicate = f'json_duplicate_key(q->body,q->body_len,"{key}")'
    assert duplicate in features, f"duplicate {key} properties must be rejected"
assert 'json_get_bool(q->body' not in features, (
    "feature updates must not use the nested-key parser"
)
duplicate_check = features.index('json_duplicate_key(q->body')
parse = features.index('want_host=json_get_top_level_bool(q->body,q->body_len,"usb_host"')
assert duplicate_check < parse, "duplicate feature fields must be rejected before parsing"
validate = features.index('want_https<0||want_sim<0||want_aed<0||', parse)
usb_write = features.index('usb_role_write(')
usb_success = features.index('api_log(c,"info",host?"USB port switched', usb_write)
acoustic_apply = features.index('if(want_aed>0)c->feature_acoustic_events=av;', validate)
persist = features.index('rc=persist_configuration(c);', validate)

assert parse < validate < usb_write, (
    "all feature booleans must be parsed and validated before USB can apply"
)
assert validate < usb_success < acoustic_apply, (
    "valid mixed updates must apply USB before acoustic settings"
)
assert validate < acoustic_apply < persist, (
    "valid acoustic updates must be applied before the shared persistence step"
)
assert features.count('features_json(c,r);return;') == 2, (
    "the USB branch must not return before applying other feature settings"
)
assert 'if(want_host>0){int urc=usb_role_write(host?"host":"device");' in features
assert 'json_get_top_level_bool(q->body,q->body_len,"acoustic_events",&av)' in features

import_start = api_c.index('static int import_configuration(')
import_end = api_c.index('static int read_central_logs(', import_start)
importer = api_c[import_start:import_end]
assert 'json_get_top_level_bool(j,strlen(j),"feature_acoustic_events"' in importer, (
    "config import must read the exported acoustic feature flag only at top level"
)
assert 'json_get_bool(j,"feature_acoustic_events"' not in importer, (
    "config import must not use the depth-insensitive feature parser"
)
assert 'if(acoustic_events_field>0)c->feature_acoustic_events=acoustic_events;' in importer, (
    "config import must restore the acoustic feature flag"
)

print("acoustic events review contracts: ok")
