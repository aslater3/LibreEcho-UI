# Bundled sounds

Raw PCM, **mono, signed 16-bit little-endian, 48 kHz** -- the format the audio
bus takes, so playback is a copy with each sample duplicated into both
channels and needs no decoder in the image.

`action-1..3.raw` are cut from
[Farting sound effects.webm](https://commons.wikimedia.org/wiki/File:Farting_sound_effects.webm)
on Wikimedia Commons, released under **CC0 1.0 Universal** (public domain
dedication, no attribution required). Trimmed to three bursts of different
length, faded 15 ms in and 30 ms out so the bus never sees a click, and
loudness-normalised to -14 LUFS.

Raw PCM barely compresses, so each second of audio is ~96 KB of boot image.
Keep them short.
