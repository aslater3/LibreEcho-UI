# minimp3 (vendored)

Upstream: https://github.com/lieff/minimp3
Licence: CC0-1.0 (public domain dedication) — see `LICENSE`.

Vendored unmodified as a single header. Built with `MINIMP3_ONLY_MP3` (no
MPEG-1 Layer I/II, which internet radio does not use) and `MINIMP3_NO_SIMD`.

Measured on ARMv7 with `-Os`, that configuration is **16,178 bytes of text**
and 11,276 bytes of bss — smaller than `libreecho-capture-mux`. It is vendored
rather than packaged because the image has no package manager and the build
must stay a plain `make` with no fetch step.

Note this breaches the "no new dependencies" line in AGENTS.md. That is a
deliberate, declared choice: playing an internet radio stream requires an MP3
decoder, and writing one is not a reasonable alternative. It is called out
explicitly in the pull request rather than left to be discovered.
