#!/usr/bin/env python3
"""End-to-end voice pipeline check: audio -> Wyoming STT (Whisper) -> external LLM.

Exercises the same protocols the device uses:
  - STT over the Wyoming protocol (as libreecho-sttd-wyoming does)
  - LLM over an OpenAI-compatible /v1/chat/completions endpoint (as agentd does)

Usage:
  python3 pipeline.py path/to/speech.wav        # 16 kHz mono s16le WAV

Env overrides:
  WHISPER_HOST/PORT   (default localhost:10300)
  LLM_URL             (default http://localhost:11434/v1/chat/completions)
  LLM_MODEL           (default qwen2.5:0.5b)
"""
import json, os, socket, sys, wave, urllib.request

WHOST = os.environ.get("WHISPER_HOST", "localhost")
WPORT = int(os.environ.get("WHISPER_PORT", "10300"))
LLM_URL = os.environ.get("LLM_URL", "http://localhost:11434/v1/chat/completions")
LLM_MODEL = os.environ.get("LLM_MODEL", "qwen2.5:0.5b")


def transcribe(wav_path):
    w = wave.open(wav_path, "rb")
    rate, width, ch = w.getframerate(), w.getsampwidth(), w.getnchannels()
    pcm = w.readframes(w.getnframes())
    w.close()
    s = socket.create_connection((WHOST, WPORT), timeout=90)

    def send(obj, payload=b""):
        if payload:
            obj["payload_length"] = len(payload)
        s.sendall((json.dumps(obj) + "\n").encode() + payload)

    send({"type": "transcribe", "data": {"language": "en"}})
    send({"type": "audio-start", "data": {"rate": rate, "width": width, "channels": ch, "timestamp": 0}})
    for i in range(0, len(pcm), 8192):
        send({"type": "audio-chunk", "data": {"rate": rate, "width": width, "channels": ch}}, pcm[i:i + 8192])
    send({"type": "audio-stop", "data": {}})

    # Wyoming frames: a JSON header line, optionally followed by data_length bytes of JSON.
    buf = b""
    s.settimeout(90)
    while True:
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                return ""
            buf += chunk
        line, buf = buf.split(b"\n", 1)
        if not line.strip():
            continue
        msg = json.loads(line)
        dl = msg.get("data_length", 0)
        if dl:
            while len(buf) < dl:
                buf += s.recv(65536)
            data, buf = json.loads(buf[:dl]), buf[dl:]
        else:
            data = msg.get("data", {})
        if msg.get("type") == "transcript":
            return (data or {}).get("text", "")


def ask_llm(text):
    body = json.dumps({
        "model": LLM_MODEL,
        "messages": [
            {"role": "system", "content": "You are LibreEcho, a local voice assistant. Reply in one short sentence."},
            {"role": "user", "content": text},
        ],
        "stream": False,
    }).encode()
    req = urllib.request.Request(LLM_URL, data=body, headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=120).read())["choices"][0]["message"]["content"]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    transcript = transcribe(sys.argv[1])
    print("STT (Whisper) heard :", repr(transcript))
    if transcript.strip():
        print("LLM reply           :", repr(ask_llm(transcript)))


if __name__ == "__main__":
    main()
