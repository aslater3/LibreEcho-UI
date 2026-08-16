#!/bin/sh
set -eu

# Issue #52 closure: the MT8163 kernel HCI transport only provides the raw
# management + L2CAP surface; the userspace profile endpoints (SDP server,
# AVDTP A2DP-SINK, AVRCP target) must be registered by libreecho-btd itself.
# These source-level assertions pin that contract because the real MT8163
# controller is not available on the host.
python3 - <<'PY'
from pathlib import Path

source = Path('src/adapter/btd.c').read_text()
profile = Path('src/adapter/bt_profile.c').read_text()
header = Path('src/adapter/bt_profile.h').read_text()
makefile = Path('Makefile').read_text()

# btd owns the profile lifecycle.
assert 'le_profile_open(&context.profiles' in source
assert 'le_profile_close(&context.profiles)' in source
assert 'le_profile_poll_setup(&context.profiles' in source
assert 'le_profile_poll_events(&context.profiles' in source
assert '#include "bt_profile.h"' in source

# The profile layer registers all three endpoints on their assigned PSMs.
assert 'LE_PSM_SDP' in profile and '0x0001' in profile
assert 'LE_PSM_AVRCP' in profile and '0x0017' in profile
assert 'LE_PSM_AVDTP' in profile and '0x0019' in profile
assert 'l2cap_seqpacket_listen(LE_PSM_SDP)' in profile
assert 'l2cap_seqpacket_listen(LE_PSM_AVDTP)' in profile
assert 'l2cap_seqpacket_listen(LE_PSM_AVRCP)' in profile

# SDP must advertise the A2DP Sink and AVRCP records.
assert 'LE_SDP_RECORD_A2DP_SINK' in profile
assert 'LE_SDP_RECORD_AVRCP' in profile
assert '0x110b' in profile  # Audio Sink class id
assert '0x110e' in profile  # Audio/Video Remote Control class id
assert 'AVDTP 1.3' in profile or '0x0103' in profile
assert '0x0106' in profile  # AVRCP 1.6 profile version

# AVDTP must implement the A2DP-SINK signaling state machine.
for signal in (
    'LE_AVDTP_DISCOVER', 'LE_AVDTP_GET_CAPABILITIES',
    'LE_AVDTP_SET_CONFIGURATION', 'LE_AVDTP_OPEN', 'LE_AVDTP_START',
    'LE_AVDTP_SUSPEND', 'LE_AVDTP_CLOSE', 'LE_AVDTP_ABORT',
):
    assert signal in profile, f'{signal} missing from AVDTP implementation'
assert 'LE_AVDTP_CODEC_SBC' in profile
assert 'LE_AVDTP_MEDIA_TYPE_AUDIO' in profile

# Decoded audio must feed the shared media bus, not a private PCM device.
assert 'LE_MEDIA_FIFO' in profile
assert '/run/libreecho-audio/media.pcm' in profile
assert 'sbc_decode' in profile

# The vendored SBC codec is compiled into libreecho-btd.
assert 'src/adapter/bt-sbc/sbc.c' in makefile
assert 'src/adapter/bt_profile.c' in makefile
assert 'LE_PROFILE_MAX_FDS' in header

# Inbound protocol buffers must stay bounds-checked.
assert 'if (offset + 2 + cap_length > length)' in profile
assert 'if (5 + param_length > session->used)' in profile
assert 'if (session->fd < 0 || total > sizeof(packet))' in profile
assert '(LE_AVDTP_PKT_SINGLE << 2)' in profile
assert '(message_type & 0x03)' in profile
assert '(signal_id & 0x3f)' in profile
assert profile.count('frames = pcm_written /') == 2
assert profile.count('sizeof(int16_t) * (mono ? 1U : 2U)') == 2
PY

# Live smoke check: the vendored SBC codec must encode and decode a test
# tone on the build host before the daemon ships it on the ARM32 target.
make build/test-sbc-codec >/dev/null
./build/test-sbc-codec
printf '%s\n' 'bluetooth profile service contract: ok'
