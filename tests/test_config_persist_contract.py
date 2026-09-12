#!/usr/bin/env python3
"""Every persisted flag in api_context must be written by configuration_json.

persist_configuration builds the file from configuration_json, so a flag that
struct api_context stores but that function never emits is silently dropped on
the next config write. The symptom is remote from the cause: toggling USB
storage rewrote the config and turned HTTPS off, because feature_https was in
the struct and in the load path but missing from the writer.

Cheap to check at the source level, and it fails loudly rather than three
reboots later.
"""
import pathlib
import re
import sys

api_h = pathlib.Path("src/api.h").read_text(encoding="utf-8")
api_c = pathlib.Path("src/api.c").read_text(encoding="utf-8")

struct = re.search(r"struct api_context\{(.*?)\n", api_h, re.S)
assert struct, "could not find struct api_context"
fields = set(re.findall(r"\b(feature_[a-z_]+)\b", struct.group(1)))
assert fields, "no feature_* fields found; the check would pass vacuously"

writer = re.search(r"static int configuration_json\(.*?\n", api_c, re.S)
assert writer, "could not find configuration_json"
emitted = set(re.findall(r'\\"(feature_[a-z_]+)\\"', writer.group(0)))

loader = re.search(r"int api_init\(.*?\n", api_c, re.S)
assert loader, "could not find api_init"
loaded = set(re.findall(r'json_get_bool\(saved,"(feature_[a-z_]+)"', loader.group(0)))

problems = []
for missing in sorted(fields - emitted):
    problems.append(f"{missing} is stored but configuration_json never writes it")
for missing in sorted(fields - loaded):
    problems.append(f"{missing} is written but api_init never reads it back")

if problems:
    for p in problems:
        print(f"config persist contract: FAIL - {p}", file=sys.stderr)
    raise SystemExit(1)
print(f"config persist contract: ok ({len(fields)} feature flags round-trip)")
