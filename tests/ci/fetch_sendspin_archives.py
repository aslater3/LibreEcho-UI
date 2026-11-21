#!/usr/bin/env python3
"""Fetch the Sendspin source archives pinned in Platform's SOURCE.lock.

Every identity and dependency record (and nested submodule) with an
archive_url is downloaded once,
checked against its archive_sha256 and stored as <repository>-<commit>.tar.gz,
the layout the Sendspin host lanes read from SENDSPIN_ARCHIVE_DIR. An archive
already present with the right hash is kept; a wrong hash is an error.

  fetch_sendspin_archives.py PLATFORM_DIR OUT_DIR
"""
import hashlib
import json
import os
import sys
import tempfile
import urllib.request
from pathlib import Path


def records(lock):
    def walk(record):
        yield record
        for sub in record.get('submodules', []):  # e.g. micro-flac's micro-ogg-demuxer
            yield from walk(sub)
    for record in lock.get('identity', {}).values():
        yield from walk(record)
    for record in lock.get('dependencies', []):
        yield from walk(record)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for block in iter(lambda: handle.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    platform, out = Path(sys.argv[1]), Path(sys.argv[2])
    lock = json.loads((platform / 'tools/mt8163-arm32/sendspin/SOURCE.lock').read_text())
    out.mkdir(parents=True, exist_ok=True)
    for record in records(lock):
        url, want = record.get('archive_url'), record.get('archive_sha256')
        if not url or not want:
            continue
        repo = record['repository'].rstrip('/').rsplit('/', 1)[1].removesuffix('.git')
        target = out / f"{repo}-{record['commit']}.tar.gz"
        if target.exists():
            if sha256(target) != want:
                sys.exit(f'{target.name}: present with the wrong SHA-256')
            print(f'kept    {target.name}')
            continue
        fd, tmp = tempfile.mkstemp(dir=out, prefix='.fetch-')
        try:
            with os.fdopen(fd, 'wb') as handle, urllib.request.urlopen(url, timeout=120) as response:
                for block in iter(lambda: response.read(1 << 20), b''):
                    handle.write(block)
            got = sha256(tmp)
            if got != want:
                sys.exit(f'{target.name}: SHA-256 {got} != pinned {want}')
            os.replace(tmp, target)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        print(f'fetched {target.name}')


if __name__ == '__main__':
    main()
