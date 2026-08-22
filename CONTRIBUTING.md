# Contributing to LibreEcho UI

Thanks for helping with LibreEcho UI. This repository is the public source for
the web daemon, frontend, service interfaces, and host-verifiable contracts.
Image construction, signing, Wi-Fi inputs, firmware, and device deployment
belong to the separate LibreEcho product/build repositories.

## Before opening a change

1. Read [`README.md`](README.md), [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
   and [`docs/API.md`](docs/API.md).
2. Keep credentials, Wi-Fi configuration, device identifiers, private paths,
   generated images, build manifests, and live logs out of Git.
3. Preserve existing license headers and check
   [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) before changing vendored
   or third-party code.
4. Keep the web daemon's bounded-resource and privilege boundaries intact.

## Development checks

Run the focused checks for the area you change, then run the full suite when the
host dependencies are available:

```sh
python3 tests/test_public_source_safety.py
sh tests/test_source_provenance.sh
node --check web/js/app.js
jq -e . web/openapi.json >/dev/null
make test
```

The complete suite needs the host dependencies listed in `README.md` and the
CI workflow. A host/mock pass does not prove target hardware behavior.

## API and frontend changes

- Update `web/openapi.json` and `docs/API.md` with API changes.
- Add a focused test under `tests/` and wire it into `tests/run_tests.sh`.
- Keep error envelopes and CSRF/Origin requirements consistent.
- Do not claim an adapter works merely because the mock backend works; use
  `not_supported` for unavailable target services.

## Pull requests

Branching, PR, and versioning rules are defined in
[`AGENTS.md`](AGENTS.md) under "Branching, Pull Requests, and Versioning" and
apply to every PR in this repository. In particular:

- Feature branches target the active major release branch; fix branches target
  the active minor release branch; release branches merge into `main` when
  release-ready.
- Every PR declares `Release impact:` and `Release note:` and keeps the
  `VERSION` file consistent with the release it targets.
- Describe the evidence boundary explicitly:
  - source implementation;
  - host build/tests;
  - image packaging, if applicable;
  - target/service readiness;
  - real hardware or external-client acceptance.

Use redacted examples only. Do not include secrets or unredacted device logs.
See [`SECURITY.md`](SECURITY.md) for sensitive reports.
