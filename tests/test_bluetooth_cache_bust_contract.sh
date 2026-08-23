#!/bin/sh
set -eu
python3 - <<'PY'
import re
from pathlib import Path
p=Path('web/index.html').read_text()
# Match the whole revision, not a prefix of it. The substring form passed for
# rev=2 and every rev=2x by accident, then failed the moment the counter
# reached 3x -- the assertion was tracking a digit, not a version.
revs=set(re.findall(r'/js/bluetooth\.js\?rev=(\d+)', p))
assert len(revs)==1, f'expected exactly one bluetooth.js revision, got {sorted(revs)}'
rev=int(revs.pop())
assert rev>=2, f'bluetooth.js cache-bust revision went backwards: {rev}'
print(f'bluetooth cache-bust contract: ok (rev={rev})')
PY
