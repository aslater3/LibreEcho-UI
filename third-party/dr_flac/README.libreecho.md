# dr_flac (vendored)

Upstream: https://github.com/mackron/dr_libs (`dr_flac.h`)
Version: v0.13.4 (2026-10-31), commit `806ee9ada8ca9827b22f76827f739c290f37ba48`
SHA-256 of `dr_flac.h`: `b8078eeb5d523502eb7964508cd7ec8450bc79d383bec90aa17fd9e2fefd9c24`
Licence: choice of public domain (Unlicense) or MIT No Attribution — see
`LICENSE`, copied verbatim from the end of the header.

Vendored unmodified as a single header. `dr_flac.c` is the only
implementation unit and is built with `DR_FLAC_NO_STDIO`, `DR_FLAC_NO_OGG`,
`DR_FLAC_NO_WCHAR` and `DR_FLAC_NO_SIMD`: radiod feeds it bytes from a network
stream through callbacks, never a file, and native FLAC is all TIDAL serves.

It exists for TIDAL playback, which LibreEcho requests as LOSSLESS: a single
unencrypted 16-bit/44.1 kHz FLAC file. The Sendspin companion's micro-flac is
C++ and confined to that companion by AGENTS.md, so radiod, which is C99,
cannot link it.

Like minimp3 and the Helix AAC decoder this breaches the "no new dependencies"
line in AGENTS.md. That is a deliberate, declared choice: playing a FLAC stream
needs a FLAC decoder.

To update: replace `dr_flac.h` with a tagged upstream release, update the
version, commit and hash above, and run `make test`.
