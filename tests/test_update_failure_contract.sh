#!/bin/sh
set -eu
mkdir -p build

# Exercise the exact parser from http_server.c with a token beyond its old 2 KiB read.
python3 - <<'PY'
from pathlib import Path
src=Path('src/http_server.c').read_text()
start=src.index('static void update_failure_reason(')
end=src.index('static int stream_update_upload(', start)
parser=src[start:end]
Path('build/test-update-failure-parser.c').write_text('''#include <fcntl.h>\n#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\n#include <stdlib.h>\n'''+parser+'''\nint main(void){char reason[64];int f=open("build/update-stderr.txt",O_RDONLY);if(f<0)return 2;update_failure_reason(f,reason,sizeof(reason));close(f);if(strcmp(reason,"current_slot_not_confirmed"))return 3;f=open("build/update-stderr.txt",O_WRONLY|O_TRUNC);if(f<0)return 4;write(f,"installer failed without a reason\\n",34);close(f);f=open("build/update-stderr.txt",O_RDONLY);if(f<0)return 5;update_failure_reason(f,reason,sizeof(reason));close(f);puts(reason);return reason[0]?6:0;}\n''')
Path('build/update-stderr.txt').write_text('x'*4096+'ERROR:bad-token!\\nERROR:current_slot_not_confirmed\\n')
PY
cc -std=c99 -Wall -Wextra -Werror -o build/test-update-failure-parser build/test-update-failure-parser.c
build/test-update-failure-parser

python3 - <<'PY'
from pathlib import Path
c=Path('src/http_server.c').read_text()
js=Path('web/js/app.js').read_text()
api=Path('docs/API.md').read_text()
openapi=Path('web/openapi.json').read_text()
assert 'mkstemp(errpath)' in c and 'unlink(errpath)' in c
assert 'pipe(errpipe)' in c and 'captured<65536' in c
assert 'child_reaped=0' in c and 'waited=waitpid(child,&status,WNOHANG)' in c
assert 'pipe_open||!child_reaped' in c
assert 'speech_samples' in Path('src/adapter/stt_engine_wyoming.c').read_text()
assert 'stream->speech_samples >= max_utterance_samples' in Path('src/adapter/stt_engine_wyoming.c').read_text()
assert 'RLIMIT_FSIZE' not in c and 'setrlimit' not in c
assert 'O_CREAT|O_TRUNC' not in c and 'LE_UPDATE_ERRLOG' not in c
assert 'lseek(f,0,SEEK_SET)' in c and 'read(f,buf,sizeof(buf))' in c
assert 'body.error?.reason' in js
assert '"reason":{"type":"string"' in openapi
assert 'error.reason' in api
PY
rm -f build/test-update-failure-parser build/test-update-failure-parser.c build/update-stderr.txt
echo 'update failure contract: ok'
