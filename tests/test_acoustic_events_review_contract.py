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

parse = features.index('want_aed=json_get_bool(q->body,"acoustic_events",&av)')
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

import_start = api_c.index('static int import_configuration(')
import_end = api_c.index('static int read_central_logs(', import_start)
importer = api_c[import_start:import_end]
assert 'json_get_bool(j,"feature_acoustic_events"' in importer, (
    "config import must read the exported acoustic feature flag"
)
assert 'if(acoustic_events_field>0)c->feature_acoustic_events=acoustic_events;' in importer, (
    "config import must restore the acoustic feature flag"
)

print("acoustic events review contracts: ok")
