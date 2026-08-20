# End-to-end UI tests (Playwright)

Drives the LibreEcho Control Centre over HTTP against a **running instance** —
the emulation (see `LibreEcho-Platform` emulation tooling) or a live device by
IP. Read-only by design: it navigates and asserts every page renders, and never
saves settings or touches reboot / factory-reset / network.

## Setup
```bash
cd tests/e2e
npm install
npx playwright install chromium
```

## Run
```bash
BASE_URL=http://<host>:8080 LE_USER=<user> LE_PASS=<password> npx playwright test
```
`BASE_URL` defaults to `http://localhost:8080`; `LE_PASS` is required. See `.env.example`.
Add `RUN_HW_TESTS=1` to also run the opt-in LED/wake self-tests (they light the ring).

## Coverage
- `nav.spec.ts` — all pages render (title, content loaded, no error view, no JS errors)
- `csp.spec.ts` — no CSP inline-style violations (regression for the strict `style-src`)
- `interactions.spec.ts` — read-only affordances (panel expansion, Logs)
- `hardware.spec.ts` — opt-in LED/wake self-tests

Auth: the device rate-limits login, so a single API login runs once in
`auth.setup.ts` and the session (a `sessionStorage` bearer token) is injected per
page by `fixtures/auth.ts`.
