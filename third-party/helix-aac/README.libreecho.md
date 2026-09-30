# Vendored Helix fixed-point AAC decoder (LibreEcho)

This directory contains the RealNetworks **Helix fixed-point HE-AAC decoder**
(2005) used by `libreecho-radiod` to decode the AAC-LC and HE-AAC v1 audio in
BBC HLS streams and in ADTS-over-Icecast streams.

## Upstream

| | |
|---|---|
| Project | `earlephilhower/ESP8266Audio` |
| URL | https://github.com/earlephilhower/ESP8266Audio |
| Commit | `10d929ac01436dfe8856e0a06fd9ec35a848c6e2` |
| Path | `src/libhelix-aac/` |
| Origin | RealNetworks Helix fixed-point HE-AAC decoder, 2005 (Jon Recker) |
| Licence | RealNetworks Public Source License v1.0 (RPSL-1.0); see `LICENSE-RPSL.txt` |

The 28 `*.c`, 7 `*.h` and `readme.txt` files in this directory are copied
**byte-for-byte** from that commit. They are unmodified: no file has been
edited, and the RPSL §2.1(c) "modified file" marker does not apply because
there are no modified files. The SHA-256 of every vendored file is listed
below and must match the upstream commit.

## What this decoder supports

From the upstream `readme.txt`:

- MPEG-2 and MPEG-4 low-complexity (LC) decoding: intensity stereo, M/S, TNS,
  PNS.
- Spectral band replication (SBR), high-quality mode (this is HE-AAC v1).
- Mono, stereo and multichannel modes.
- ADTS, ADIF and raw-data-block file formats.

Not supported (and not needed by the BBC streams, which are LC or HE-AAC v1):

- Main or SSR profile, LTP.
- Coupling channel elements (CCE).
- 960/1920-sample frame sizes.
- Low-power and downsampled (single-rate) SBR.
- **Parametric stereo (HE-AAC v2).**

## Why it was chosen

- Fixed-point, pure C, no assembly required and no dynamic allocation on the
  static path (we use `AACInitDecoderPre`), which matches the small ARM32
  target.
- Permissively linkable: MIT is on the RPSL's compatible-source-licence list,
  so linking into the MIT `radiod` is permitted (see `THIRD_PARTY_NOTICES.md`).
- Verified against the four BBC Radio 2 variants (48k/96k HE-AAC, 128k/320k
  LC): output bit-exact across builds and ~78-84 dB SNR vs ffmpeg, zero lag.

Rejected alternatives: FAAD2 (GPL-2, incompatible with the MIT repo),
fdk-aac (non-free patent-clause licence), FFmpeg libavcodec (would add >1 MB
and cross-repo build coupling).

## Measured size

- x86 `-Os` text: ~127 KB.
- Decoder state: `AACDecInfo` 120 B + `PSInfoBase` 28,752 B + `PSInfoSBR`
  50,788 B. `radio_aac.c` uses a fixed `LE_RADIO_AAC_STATE_BYTES` (81920)
  static buffer and asserts `AACInitDecoderPre` returned non-NULL.

## Build flags (Helix objects only)

Built with the Arduino code path enabled and warnings suppressed for the
vendored code only (the upstream `%d`/`sizeof` printf and signed/unsigned
habits are not ours to fix):

```
-DARDUINO -Ithird-party/helix-aac/shim -Ithird-party/helix-aac -w
```

`-DARDUINO` selects the Arduino branch that pulls in the standard
`<stdio.h>`/`<stdlib.h>`/`<string.h>` instead of the Helix `hlxclib/*` headers,
and it is an accepted platform in `aacdec.h`. Also builds with
`-funsigned-char` (the ARM default). Under `-fsanitize=address,undefined` the
vendored objects report upstream signed left-shift diagnostics (their
two's-complement shift idiom); with `-fno-sanitize=shift` on those objects
only, the run is clean, and LibreEcho's own wrapper and test are built with
full ASan/UBSan.

## Shims

`shim/Arduino.h` and `shim/pgmspace.h` are **LibreEcho-authored** (MIT), not
part of the upstream distribution. `aaccommon.h` includes `<Arduino.h>` and
`<pgmspace.h>`; the shim provides the standard C headers and makes `PROGMEM`
empty with `pgm_read_byte`/`pgm_read_word` as plain dereferences, so the
decoder's constant tables stay ordinary objects and the sources compile
unmodified.

## Licence obligation

RPSL §2.1(a)(b)(c)(e): every upstream licence header is kept intact, the
licence text ships alongside the source (`LICENSE-RPSL.txt`), no file is
modified (so no modification marker is required), and binary distributions
must carry a notice that the source is available. MIT is a compatible source
licence, so the static link into `radiod` is permitted. Do not describe this
code as MIT.

## Open item (not a code issue)

AAC/H.264 patent licensing is a distribution-time legal matter and is out of
scope for the source code. It is noted here as an open item; it does not
affect the build or the tests.

## SHA-256 manifest (as vendored, `sha256sum *.c *.h readme.txt`)

```
a90346159de4e87d23d5bebb7cd5bda19293fb02d3eeaffee0d45a05a3966a97  aaccommon.h
0a5fe4ab8214c50bd9e733049e1b98a195fe964a0a747b1ecf4d12049b269745  aacdec.c
17e120ccca644d46f147597f60f9394cec74f6803874a4efe6f50f456501cf3e  aacdec.h
aade1c340676343bd627fdd1deb8292c12c439ac2fe96c77ce589910e72db107  aactabs.c
5a185ddc0e75046e3b3a501a931817d9f00fe3e754ef707309a390148378ec29  assembly.h
b66a7f7dc777882e6c730188198d438be2b685f6cee10daa82d5d1d8f37810d2  bitstream.c
52fbbad45bb7ff3cef4e3b67c1a247168514923487b3f70febdcd865f27ab439  bitstream.h
96381cb3e3eb1fd101b951e7d61c57dbad0747b9c0e81d906d1b38ff79ba2fe3  buffers.c
f303590f1e0078906d7f77362b39f7bfe54af054b570a8423c8e2ee5d853118e  coder.h
2e9e0085a6b47eb65917097c1b6a1ebf55c086b5ba8807725137bf2bb54e8a71  dct4.c
0b1cecce524d75ac9d4ebd991693373a5802f3edef3af64bdad1a5fa00d68ad8  decelmnt.c
53c13a1a6522a507d5e1ea0fd775a192b42a96c44206b278f9422fc02373a47b  dequant.c
70bb5a3d32d647931954223a6fbe796657c1a0675edac9dc77bc2cafe45cb558  fft.c
2f7dbcd6830c39d12638f8068fde782f55c6bd414e8ad843bcc5b8e1c6abc728  filefmt.c
4b17675f432bc175493c5704a09e4420ddeba14be4454852f79f5ba4010c28cc  huffmanaac.c
ccdbd98b85e3a04cf3037268fad8c332bb2cb5c2bc10a515c773ddce60c1c69b  hufftabs.c
f9f801be7ca9a12cc71c0676a424bf337601934d9d0a458af5949051d39cfa42  imdct.c
79e33330b0da60867534ab3815ab3f6cd037b862b3853d2e47cb23400dd41be8  noiseless.c
c87ce4b6664262f4ed309768b10fd9c2ee586c722c2f817e1efcfeca7debce97  pns.c
d6bd4073d25e8a0cb742aa386e37ff266c4e616ddf7b64342c51d5eeceed0bb2  readme.txt
321ae0cc0d3d3d2e4b724eef43ac8492552143a6324b250ea38b0394f18bd813  sbr.c
9f33a9a7e7ae09a42d8f4827bfff6d1078c912d7897a80ea333618e81594c7c7  sbrfft.c
52c46db81528c19b859f73d01467dc3aa637578b29b197ecde9a9f644fe4c1f7  sbrfreq.c
a72551d3320f5171c9f5f8453180eac37929f949b4bbd63d05b21454a8b0b022  sbr.h
d63824e433cd22ecaecfec825747313b1f44e18c09738e8bee7d38ecbaaa4a00  sbrhfadj.c
0aa0787edc4814e061e4836caabf73b0771bf9ea3e9772ac6a18c55a0401e4af  sbrhfgen.c
4bcf1ed9cb173d2aa59d1428283fffbe63616f3b221cbd14647d59684633aa71  sbrhuff.c
bb378a02e113c32ade80d7a339af7b62158c098b58fb806be1c2fd40345c29aa  sbrimdct.c
cbd765c8311f7aabcdb913ec1ac96081345df9f50cbc37d776fe02471bb7945f  sbrmath.c
56cfb0cd0bd2dac2e32ec4a77bff9135aad7ca670ad8c0eda9c95dd59cd92b17  sbrqmf.c
bd1c105a8845d45844d18c68180597ee7d89badddc019421341ca0f3b16c3f0d  sbrside.c
4db131e22cc2167b5459328027e2d737dc0f121b0c808d3697e8f113c2f2b3cc  sbrtabs.c
0ab641c29d829fd87f72d7f6bc26faec5d8d00ce998ca29e5d5356da658aa406  statname.h
bb7fde46993cb4a21813bd61061c3d77d1650321142c1761e5baa91b6074adf2  stproc.c
6f07ab8f2966a5a23ae1753415fce66b220ee227ceade81b20ea9e5cf9172a54  tns.c
ffbb843c9711523a36dc9b4774d3cd7f9e8415051fdcbee235aec84b3c63316a  trigtabs.c
```
