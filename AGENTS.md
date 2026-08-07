# LibreEcho Web — Agent Guidelines

LibreEcho Web is the web administration interface for LibreEcho (Amazon Echo Gen 2-class device). It is a single C99 HTTP daemon (bounded `poll()` event loop, no threads/processes/containers) serving static files and `/api/v1/` from one process, plus a vanilla-JS browser frontend.

Read `docs/ARCHITECTURE.md` (system design), `docs/API.md` (HTTP API reference), and `README.md` before making non-trivial changes.

## Build and test

- C99 only (`-std=c99`), POSIX 200809L. No new dependencies: no language runtime, package manager, database, or container.
- Build: `make` — full suite: `make test` (clean build + `tests/run_tests.sh`).
- Workflow parity: CI also runs `node --check web/js/app.js` and `jq -e . web/openapi.json`; keep both valid when touching them.
- Every API or behavioral change needs a test in `tests/` wired into `tests/run_tests.sh`.

## Code Review Rules

### Bounded resources (hard limits are the design)

- All buffers and rings are fixed-size by design (16 clients, 8 KiB headers, 16 KiB API bodies, 12 Wi-Fi scan results, 128 logs, 64 events). Do not introduce unbounded allocations, dynamic resizing, or limits that can be raised by client input.
- All string/byte copies into fixed buffers must be length-checked (e.g. `snprintf` + length clamp, `constant_equal`-style patterns). Flag any `strcpy`/`sprintf`/`memcpy` on client-controlled data.
- New API request/response fields must be validated against the same limits as existing ones; reject with a 4xx JSON error, never truncate silently.

### Security-critical paths

- Secret comparisons (auth tokens, CSRF tokens, bearer sessions) must be constant-time (see `constant_equal` in `src/api.c`). Flag timing-leaky `strcmp`/`memcmp` on secrets.
- State-changing requests must require `X-LibreEcho-CSRF`; device power actions additionally require `X-LibreEcho-Confirm`. Do not weaken or bypass `security()` in `src/api.c`.
- Origin checking (`allowed_origin`) and same-origin LAN login logic are intentional — changes here need a matching test case in `tests/test_auth.sh` (including the negative case, e.g. evil origin rejected).
- Config store writes must stay atomic: temp file + fsync + rename + backup, mode 0600.
- Static file serving must keep rejecting `..` and backslashes.

### API contract

- API changes must be reflected in `web/openapi.json` and `docs/API.md` in the same PR.
- Error responses follow `{"ok":false,"data":null,"error":{"code":...,"message":...}}`; success follows `{"ok":true,"data":...}`. Do not invent new envelope shapes.
- New endpoints must declare their method handling explicitly (`GET`/`HEAD` vs mutating) and get an entry in `tests/test_api.sh` or the matching contract test.

### Style

- Match the existing dense, single-line statement style in `src/`; do not reformat unrelated code.
- Frontend stays dependency-free vanilla JS (`web/js/app.js`); no frameworks, bundlers, or build steps.
- Keep mechanical checks (formatting, lint) out of review feedback — CI owns those.
