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
