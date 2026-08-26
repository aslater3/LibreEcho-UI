#!/usr/bin/env python3
"""Measure end-to-end voice-turn latency on a live LibreEcho device.

Speaks a command out of *this* machine's speakers, waits for the device to
finish the turn, and reads the per-leg timings the device recorded. Repeats N
times and reports the distribution, so a tuning change can be judged on
numbers instead of impressions.

Why it drives the wake word synthetically
-----------------------------------------
The acoustic wake word currently fires only once per boot: after the first
sound comes out of the speaker it stops detecting until the device is
rebooted. A twenty-iteration acoustic benchmark would therefore measure one
turn and nineteen silences. So by default each iteration triggers listening
through POST /api/v1/wake-word/test and only the *command* is spoken aloud.

That means this tool measures the capture -> STT -> LLM -> TTS path, and does
NOT measure wake-word detection. Pass --acoustic to speak the wake word too;
expect it to work exactly once per boot until that bug is fixed.

Timings come from GET /api/v1/assistant/history, which the device maintains:

  stt_audio_ms       audio actually captured for the utterance
  stt_processing_ms  end of capture -> transcript ready
  first_text_ms      first token back from the LLM
  first_pcm_ms       first audio sample handed to the speaker
  stt_total_ms       whole turn

first_pcm_ms is the number a listener actually feels; agentd targets < 3000ms.

Credentials come from the environment, never the command line, and are never
printed:

  LIBREECHO_URL       e.g. http://192.0.2.10:8080
  LIBREECHO_USERNAME
  LIBREECHO_PASSWORD

Examples
--------
  LIBREECHO_URL=http://192.0.2.10:8080 LIBREECHO_USERNAME=someone \\
  LIBREECHO_PASSWORD=... python3 tools/voice_latency_bench.py -n 20

  # with a neural voice instead of macOS `say` (far more human, which matters
  # if you use --acoustic, since `say` voices do not trigger openwakeword)
  ... --speak-cmd './say-kokoro.sh {text}'
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request

DEFAULT_PHRASES = [
    "what time is it",
    "what is the weather",
    "what is two plus two",
    "how many days are in September",
    "what is the capital of France",
]


def api(url, path, token=None, csrf=None, method="GET", body=None, timeout=30):
    req = urllib.request.Request(f"{url}/api/v1/{path}", method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    if csrf:
        req.add_header("X-LibreEcho-CSRF", csrf)
    data = json.dumps(body).encode() if body is not None else None
    with urllib.request.urlopen(req, data, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


def login(url, username, password):
    """CSRF first: the login route rejects a request without it."""
    csrf = api(url, "config")["data"]["csrf_token"]
    out = api(url, "auth/login", csrf=csrf, method="POST",
              body={"username": username, "password": password})
    token = (out.get("data") or {}).get("token")
    if not token:
        raise SystemExit("login failed (check LIBREECHO_USERNAME/PASSWORD)")
    return token, csrf


def speak(template, text):
    if "{text}" in template:
        cmd = template.replace("{text}", text)
    else:
        cmd = f'{template} "{text}"'
    subprocess.run(cmd, shell=True, check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def newest_turn(url, token, csrf):
    try:
        turns = api(url, "assistant/history", token, csrf)["data"]["turns"]
    except (urllib.error.HTTPError, urllib.error.URLError, KeyError):
        return None
    return turns[0] if turns else None


def run_once(args, token, csrf, phrase, previous):
    """One turn. Returns the recorded turn dict, or None if nothing landed.

    `previous` is the newest turn before this iteration; a turn is only ours
    once at_ms changes, which is what distinguishes a fresh turn from the
    device simply still holding the last one.
    """
    if not args.acoustic:
        try:
            api(args.url, "wake-word/test", token, csrf, method="POST", body={})
        except urllib.error.HTTPError as exc:
            print(f"    wake trigger failed: {exc}", file=sys.stderr)
            return None
        time.sleep(args.wake_settle)
    else:
        speak(args.speak_cmd, args.wake_word)
        time.sleep(args.wake_settle)

    started = time.time()
    speak(args.speak_cmd, phrase)

    prev_at = (previous or {}).get("at_ms")
    while time.time() - started < args.turn_timeout:
        turn = newest_turn(args.url, token, csrf)
        if turn and turn.get("at_ms") != prev_at:
            return turn
        time.sleep(0.5)
    return None


def summarise(values, label, unit="ms"):
    if not values:
        return f"  {label:<20} (no samples)"
    values = sorted(values)
    p95 = values[min(len(values) - 1, int(round(0.95 * (len(values) - 1))))]
    return (f"  {label:<20} median={statistics.median(values):>7.0f}{unit}"
            f"  mean={statistics.fmean(values):>7.0f}{unit}"
            f"  min={values[0]:>6.0f}  max={values[-1]:>6.0f}  p95={p95:>6.0f}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default=os.environ.get("LIBREECHO_URL"))
    p.add_argument("-n", "--iterations", type=int, default=20)
    p.add_argument("--speak-cmd", default=os.environ.get("LIBREECHO_SPEAK_CMD", "say -v Samantha"),
                   help="shell command to speak text; {text} is substituted if present")
    p.add_argument("--acoustic", action="store_true",
                   help="also speak the wake word (works once per boot; see module docstring)")
    p.add_argument("--wake-word", default="Alexa")
    p.add_argument("--wake-settle", type=float, default=0.4,
                   help="seconds between triggering listening and speaking")
    p.add_argument("--turn-timeout", type=float, default=30.0)
    p.add_argument("--gap", type=float, default=4.0,
                   help="seconds between iterations, to let TTS finish")
    p.add_argument("--report", help="write the raw per-turn JSON here")
    args = p.parse_args()

    if not args.url:
        raise SystemExit("set --url or LIBREECHO_URL")
    user = os.environ.get("LIBREECHO_USERNAME")
    pw = os.environ.get("LIBREECHO_PASSWORD")
    if not user or not pw:
        raise SystemExit("set LIBREECHO_USERNAME and LIBREECHO_PASSWORD")

    token, csrf = login(args.url, user, pw)
    listening = api(args.url, "voice-pipeline", token, csrf)["data"]["listening"]
    print(f"device   : {args.url}")
    print(f"listening: {listening}")
    print(f"wake     : {'acoustic' if args.acoustic else 'synthetic trigger (see --acoustic)'}")
    print(f"speaking : {args.speak_cmd}\n")

    turns, failures, previous = [], 0, newest_turn(args.url, token, csrf)
    for i in range(1, args.iterations + 1):
        phrase = DEFAULT_PHRASES[(i - 1) % len(DEFAULT_PHRASES)]
        turn = run_once(args, token, csrf, phrase, previous)
        if turn is None:
            failures += 1
            print(f"  {i:>3}/{args.iterations}  NO TURN            {phrase!r}")
        else:
            previous = turn
            turn["phrase"] = phrase
            turns.append(turn)
            print(f"  {i:>3}/{args.iterations}  total={turn['stt_total_ms']:>6}ms"
                  f"  audio={turn['stt_audio_ms']:>5}ms"
                  f"  stt={turn['stt_processing_ms']:>6}ms"
                  f"  first_pcm={turn['first_pcm_ms']:>6}ms  {phrase!r}")
        time.sleep(args.gap)

    print(f"\n{len(turns)}/{args.iterations} turns completed, {failures} produced nothing")
    if turns:
        print(summarise([t["stt_total_ms"] for t in turns], "total"))
        print(summarise([t["stt_audio_ms"] for t in turns], "captured audio"))
        print(summarise([t["stt_processing_ms"] for t in turns], "stt processing"))
        print(summarise([t["first_text_ms"] for t in turns], "first LLM text"))
        print(summarise([t["first_pcm_ms"] for t in turns], "first audio out"))
        under = sum(1 for t in turns if t["first_pcm_ms"] < 3000)
        print(f"\n  first_pcm under the 3000ms target: {under}/{len(turns)}")
    if args.report:
        with open(args.report, "w") as fh:
            json.dump({"listening": listening, "turns": turns,
                       "failures": failures}, fh, indent=2)
        print(f"  raw turns written to {args.report}")
    return 0 if turns and not failures else 1


if __name__ == "__main__":
    sys.exit(main())
