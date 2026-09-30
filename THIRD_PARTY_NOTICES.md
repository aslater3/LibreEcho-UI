# Third-party notices

This file records third-party code and runtime integrations that are relevant to
this repository. It does not relicense those components. Each component keeps
its own copyright and license terms.

## LibreEcho-authored source

Unless a file contains another notice, LibreEcho-authored source and artwork in
this repository are Copyright (c) 2026 LibreEcho contributors and are licensed
under the MIT License in [`LICENSE`](LICENSE).

The MIT license does not apply to the third-party code listed below, to the
Linux kernel, to vendor firmware, to device images, to model weights or voices,
or to components supplied by the separate LibreEcho build/product repositories.

## Vendored BlueZ SBC codec

The directory [`src/adapter/bt-sbc/`](src/adapter/bt-sbc/) contains the SBC
codec implementation compiled into `libreecho-btd`. It retains the upstream
copyright notices in each applicable source/header file and declares:

```text
SPDX-License-Identifier: LGPL-2.1-or-later
```

The SBC implementation is distributed under the GNU Lesser General Public
License, version 2.1 or later. The applicable license text is available from the
[GNU LGPL v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).
The source files' SPDX and copyright headers must remain intact in source and
binary distributions.

The SBC code is used as a separately licensed component; inclusion in this
repository does not make it MIT-licensed. Changes to the SBC component must
preserve its upstream notices and LGPL boundary.

## Vendored Helix fixed-point AAC decoder

The directory [`third-party/helix-aac/`](third-party/helix-aac/) contains the
RealNetworks Helix fixed-point HE-AAC decoder (2005), compiled into
`libreecho-radiod` to decode the AAC-LC and HE-AAC v1 audio in radio streams.
It retains the upstream RealNetworks copyright and license header in every
applicable source file and is distributed under the **RealNetworks Public
Source License v1.0 (RPSL-1.0)**. The applicable license text ships with the
source as `third-party/helix-aac/LICENSE-RPSL.txt` (in-tree).

Provenance, the pinned upstream commit, the build flags, and the SHA-256 of
every vendored file are recorded in
`third-party/helix-aac/README.libreecho.md`.
The vendored `*.c`, `*.h` and `readme.txt` files are copied byte-for-byte from
`earlephilhower/ESP8266Audio` commit
`10d929ac01436dfe8856e0a06fd9ec35a848c6e2` (path `src/libhelix-aac/`) and are
**unmodified**; the two shim headers under
[`third-party/helix-aac/shim/`](third-party/helix-aac/shim/) are
LibreEcho-authored (MIT).

The RPSL obligations are honoured: upstream license headers stay intact, the
license text is shipped with the source, no file is modified (so no
modification marker applies), and binary distributions must carry a notice that
the source is available. MIT is on the RPSL's compatible-source-license list,
so the static link into the MIT `radiod` is permitted. The Helix decoder is not
MIT-licensed; do not describe it as such. AAC patent licensing is a
distribution-time matter and is noted as an open item in the README only.

## Pinned Ogg Opus decode stack

`libreecho-radiod` can decode Ogg Opus local files via
`src/adapter/radio_opus.c`. Opus is decoded by a pinned, statically linked
stack built **outside this repository** by the image build helper
`tools/mt8163-arm32/ui/build_opus.sh` (in the separate LibreEcho platform
repository), not vendored here:

| Component | Version | License | Upstream |
|---|---|---|---|
| libogg | 1.3.5 | BSD-3-Clause | https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.gz |
| libopus | 1.4 | BSD-3-Clause | https://downloads.xiph.org/releases/opus/opus-1.4.tar.gz |
| libopusfile | 0.12 | BSD-3-Clause | https://github.com/xiph/opusfile/releases/tag/v0.12 |

The build is static and HTTP/TLS-free by construction: libopusfile is compiled
from its four local-file sources only, so `src/http.c` and its libcurl/OpenSSL
dependencies are absent. The image build records the archive and license
SHA-256 values and the exact source URLs in its `opus/SOURCE.lock` and emits
`opus-source.json`/`opus-identity.json` beside the prefix. When this stack is
linked into a binary image, ship the three upstream `COPYING` files (BSD-3
text) with the image's notices; the source offer and redistribution
obligations for the linked binary belong to that image release boundary.

`src/adapter/radio_opus.c` is LibreEcho-authored (MIT). The Opus test fixtures
in `tests/radiod_opus_fixture.h` are generated synthetic tones (no third-party
audio) by `tests/gen_radiod_opus_fixture.py`.

## Build-time and runtime integrations

The following are referenced by the UI/service layer or supplied by the image
build, but are not vendored as source in this repository's current `main` tree:

- **SpeexDSP** — optional host/runtime dependency for AEC and resampling. Use
  the license and notices supplied by the installed or packaged SpeexDSP copy.
- **sherpa-onnx and ONNX Runtime** — optional ARM32 inference build inputs for
  the real STT/TTS/wake-word targets. Their source, binary, model, and license
  obligations belong to the corresponding build/image release boundary.
- **Wyoming services/protocol** — an external integration used by custom and
  Home Assistant speech modes. See the links in
  [`docs/HOME_ASSISTANT_VOICE.md`](docs/HOME_ASSISTANT_VOICE.md).
- **Shairport Sync, FFmpeg, Avahi/D-Bus, wpa_supplicant, BusyBox, musl, and
  other image components** — supplied or built by the separate LibreEcho image
  pipeline when enabled. Their complete notices and source-offer obligations
  must accompany any binary/image distribution; this UI repository does not
  claim to relicense them.

A dependency being mentioned here is not a claim that its binaries or model
weights are redistributed by this repository. For a device image or OTA bundle,
use the image manifest and the build repository's component notices as the
release boundary.

## Hardware, protocol, and trademark boundary

LibreEcho is an independent project and is not affiliated with or endorsed by
Amazon. Hardware names are used only to identify supported hardware. Research,
reverse-engineering, boot, kernel, firmware, and vendor components belong to
their own repositories and license/provenance boundaries.

This notice is project documentation, not legal advice. Before redistributing a
binary, image, model, voice, firmware blob, or extracted vendor component,
verify its exact provenance, license, and accompanying notices independently.
