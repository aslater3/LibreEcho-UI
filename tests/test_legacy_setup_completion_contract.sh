#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path
source = Path('src/main.c').read_text()
start = source.index('static int setup_marker_present')
end = source.index('int main(', start)
function = source[start:end]
assert 'static int setup_account_pending' in source
assert 'setup_marker_present(cfg)||(!setup_account_pending(cfg)&&(users_path&&access(users_path,R_OK)==0))' in source
assert 'stat(config_path,&st)' not in function
print('legacy configured installations retain setup completion: ok')
PY
