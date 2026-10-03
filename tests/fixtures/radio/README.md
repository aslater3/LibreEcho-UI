# Radio test fixtures

Generated with the bundled ffmpeg (bit-exact flags, so the files are
reproducible). Both carry 2 s of a 1 kHz sine, 44.1 kHz stereo AAC-LC at 64 kb/s.

```
ffmpeg -fflags +bitexact -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=2" -ac 2 -c:a aac -b:a 64k -flags +bitexact -f mpegts -mpegts_flags +resend_headers -muxrate 0 -pat_period 0.1 -sdt_period 999 -metadata service_name=t -metadata service_provider=t aac-tone.ts
ffmpeg -fflags +bitexact -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=2" -ac 2 -c:a aac -b:a 64k -flags +bitexact -f adts aac-tone.adts
```

`aac-tone.adts` sha256 `de0da2040d39c56e5e8af8490b624dc9d1fde8b38c02b3798d28c65d8bab209c`

Invariant: demuxing `aac-tone.ts` with `radio_ts.c` yields `aac-tone.adts`
byte for byte (asserted by `tests/test_radio_ts.c`).
