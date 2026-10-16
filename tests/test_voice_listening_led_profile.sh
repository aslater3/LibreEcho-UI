#!/bin/sh
set -eu

# Keep the producer of the second pulse aligned with the exact request covered
# by test_wake_led_profile.c. The real ledd handler test verifies the behavior;
# this catches the helper's wire-format string drifting away from that fixture.
grep -Fq '\"profile\":\"listening\"' src/adapter/voice_listening_led.c
grep -Fq '\"owner\":\"voice-listening\"' src/adapter/voice_listening_led.c
echo 'voice-listening LED profile contract: ok'