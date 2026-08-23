"""Minimal Wyoming (JSONL + binary payload) client -- matches
src/adapter/wyoming_protocol.c on the wire."""
import json, socket


class Wyoming:
    def __init__(self, host, port, timeout=60.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.f = self.sock.makefile("rwb")

    def send(self, etype, data=None, payload=b""):
        header = {"type": etype}
        if data is not None:
            header["data"] = data
        if payload:
            header["payload_length"] = len(payload)
        line = (json.dumps(header) + "\n").encode()
        self.f.write(line)
        if payload:
            self.f.write(payload)
        self.f.flush()

    def read(self):
        """Wyoming frames the JSON `data` object OUT OF LINE when the header
        carries data_length: header line, then data_length bytes of JSON,
        then payload_length bytes of binary.  Servers use both forms."""
        line = self.f.readline()
        if not line:
            return None, b""
        header = json.loads(line)
        dn = header.get("data_length") or 0
        if dn:
            header["data"] = json.loads(self.f.read(dn).decode("utf-8"))
        n = header.get("payload_length") or 0
        payload = self.f.read(n) if n else b""
        return header, payload

    def close(self):
        try:
            self.f.close()
        finally:
            self.sock.close()


def describe(host, port):
    w = Wyoming(host, port, timeout=10)
    w.send("describe")
    hdr, _ = w.read()
    w.close()
    return hdr


def synthesize(host, port, text, voice=None):
    """Return (rate, width, channels, pcm_bytes)."""
    w = Wyoming(host, port)
    data = {"text": text}
    if voice:
        data["voice"] = {"name": voice}
    w.send("synthesize", data)
    rate = width = channels = None
    chunks = []
    while True:
        hdr, payload = w.read()
        if hdr is None:
            break
        t = hdr.get("type")
        d = hdr.get("data") or {}
        if t == "audio-start":
            rate, width, channels = d.get("rate"), d.get("width"), d.get("channels")
        elif t == "audio-chunk":
            rate = d.get("rate", rate)
            width = d.get("width", width)
            channels = d.get("channels", channels)
            chunks.append(payload)
        elif t == "audio-stop":
            break
    w.close()
    return rate, width, channels, b"".join(chunks)


def transcribe(host, port, pcm16k_mono, language="en", chunk=1024):
    w = Wyoming(host, port)
    w.send("transcribe", {"language": language})
    w.send("audio-start", {"rate": 16000, "width": 2, "channels": 1, "timestamp": 0})
    for i in range(0, len(pcm16k_mono), chunk):
        w.send("audio-chunk",
               {"rate": 16000, "width": 2, "channels": 1},
               pcm16k_mono[i:i + chunk])
    w.send("audio-stop", {"timestamp": 0})
    text = None
    while True:
        hdr, _ = w.read()
        if hdr is None:
            break
        if hdr.get("type") == "transcript":
            text = (hdr.get("data") or {}).get("text")
            break
    w.close()
    return text
