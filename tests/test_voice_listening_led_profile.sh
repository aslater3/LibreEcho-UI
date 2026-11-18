#!/bin/sh
set -eu

# Keep the producer of the listening cue aligned with the exact request
# covered by test_wake_led_profile.c. The real ledd handler test verifies the
# behaviour; this catches the helper's wire-format string drifting away from
# that fixture and the recognition caller losing its owner.
grep -Fq '\"profile\":\"listening\"' src/adapter/listening_ring.c
grep -Fq 'le_listening_ring_show(' src/adapter/voice_listening_led.c
grep -Fq '"voice-listening"' src/adapter/voice_listening_led.c
echo 'voice-listening LED profile contract: ok'
