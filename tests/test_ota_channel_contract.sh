#!/bin/sh
set -eu
python3 - <<'PY'
from pathlib import Path

api = Path('src/api.c').read_text()
http = Path('src/http_server.c').read_text()
ui = Path('web/js/app.js').read_text()
tests = Path('tests/test_api.sh').read_text()

# The channel authorizer must reject malformed JSON before support/action checks
# and return the parsed value to its caller.
a = api.index('int api_update_channel_authorize')
b = api.index('static int key_from_stream', a)
fn = api[a:b]
assert 'if(!body_ok(q,r))return 0;' in fn
assert 'char*channel,size_t channel_size' in fn
assert 'json_get_string(q->body,"channel",channel,channel_size)' in fn

# The HTTP route must use that parsed value, not re-scan the raw body.
route = http[http.index('if(!strcmp(q.path,"/api/v1/system/update/channel")'):]
route = route[:route.index('if(!strncmp(q.path,"/api/v1/baby-monitor/stream"')]
assert 'api_update_channel_authorize(api,&q,&r,channel,sizeof(channel))' in route
assert 'snprintf(action,sizeof(action),"set-channel-%s",channel)' in route
assert 'json_get_string(q.body,"channel",channel,sizeof(channel))' not in route

# Channel command failures are persistence/service failures, not bad requests;
# successful channel responses must not claim installation or reboot state.
run = http[http.index('static int run_update_fetch'):http.index('static int start_update_fetch')]
assert 'if(channel_action)update_error(fd,503' in run
assert 'The update channel could not be saved' in run
success = run[run.index('if(channel_action){'):run.index('}else{', run.index('if(channel_action){'))]
assert 'channel' in success
assert 'reboot-pending' not in success
assert 'installed' not in success

# The UI captures both values before either request and avoids mutate(), which
# rerenders the page after each individual request.
save = ui[ui.index("if($('#save-update-settings'))"):ui.index("if($('#check-update'))", ui.index("if($('#save-update-settings'))"))]
assert "const channel=$('#update-channel').value,automatic=$('#automatic-updates').checked" in save
assert "await api('/system/update/channel'" in save
assert "await api('/system/update/automatic'" in save
assert 'await mutate(' not in save

# Existing integration coverage verifies unsupported OTA and response envelope.
assert '--data \'{"channel":"stable"}\'' in tests
assert '[ "$code" = 501 ]' in tests
print('ota channel contract: ok')
PY
