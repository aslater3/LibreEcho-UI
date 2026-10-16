# 0.14 feature-batch browser verification

Real-browser acceptance for the 0.14 feature batch, driven by `tests/e2e/feature-batch.cjs`
against the shipped page scripts and the production mock web daemon. This is deliberately
different from `tests/test_feature_batch_ui.js`, which evaluates the same source under a Node
DOM shim and calls handlers directly: a shim call cannot see a control the browser renders
disabled, or a handler that was never bound after a full page load.

## How to run

The shared runner starts the mock daemon (and a second credential-protected daemon for the
recovery landing), completes setup, then runs each `tests/e2e/*.cjs` suite.

```sh
# Requires Node.js, curl, and Playwright with Chromium installed.
# Point NODE_PATH at your external Playwright installation if necessary.
export NODE_PATH="${PLAYWRIGHT_NODE_MODULES:?Set the Playwright node_modules directory}"
export LIBREECHO_E2E_SUITES="feature-batch"
sh tests/e2e/run.sh
```

With no browser installed the launcher falls back: bundled Chromium → `channel: "chrome"` →
`$LIBREECHO_CHROME`. A plain `node tests/test_feature_batch_ui.js` runs the narrower shim suite.

## What the browser suite covers

- the real script list loads and executes (`ledPage`, `voiceHistoryPanel`, `recoveryPanel` defined);
- LED idle/sleep save, including the enabled-on-edit transition, a real click, the restore
  toggle, an SPA re-render and a full reload re-binding the handlers;
- nursery sounds source/bed/tempo/fade/timer and client-side fade-vs-timer rejection;
- USB `.opus` Play gating on the advertised decoder capability;
- recent voice list/detail/clear, newest-first 10-row cap, escaping and the stale-detail guard;
- owner-prepared recovery password: shown once, escaped, never persisted, device sends
  `Cache-Control: no-store`;
- the recovery settings Save control becomes usable on edit and PUTs the control states;
- the login-only recovery landing (`/setup.html?recovery=1`): a configured device never
  activates the one-time account step, signs in, and reaches the Wi-Fi-only page.

## Defects found and fixed

Front-end (`web/js/app.js`):

- `bindLedBase()` bound the idle/sleep Save handlers but never called `bindDirty`, so the idle
  and sleep Save buttons — which render disabled — could never become usable. Now each control
  is wired through `bindDirty`.
- `bindRecovery()` had the same omission for the recovery settings Save button.
- `recoveryPanel()` treated every mode except `off`/`unavailable` as an active access point, so
  `client`, `armed`, `stopped` and `stopping` showed a green "serving" dot. The dot is now set
  only for the networkd mode `recovery-ap`; `handover` (AP released, station reassociating) gets
  its own label and a non-active dot, and every mode renders a phase label.

Test harness (`tests/e2e/feature-batch.cjs`):

- `recordRequests()` stored the path with the `/api/v1` prefix while every `calls()`/`lastBody()`
  lookup used the API-relative path, so those assertions silently matched nothing. The prefix is
  now stripped.
- The LED/recovery cases had a force-enable / direct-`onclick` fallback that could pass despite a
  disabled Save button. Both are now genuine click regressions: a disabled control fails the case.
- The restore toggle was `page.check()`-ed on a visually hidden input; a real click on the visible
  switch is used instead.
- The login-only case queried `#step0`, which does not exist in the shipped `setup.html` (the
  account step is `.setup-page[data-step="0"]`), so it could never observe the real state.
- The voice case clicked detail buttons for ids outside the 10-row newest-first cap, matched the
  uppercased `innerText`, and raced the prepare response; fixed to painted ids, `.status.error`,
  and `waitForResponse`.
- The recovery Save case hardcoded `enabled:false` although the mock device reports `enabled:true`;
  it now asserts the PUT against the actual control state.
- The prepare `no-store` header is now checked with a direct device request, since the browser
  request is fulfilled by a mocked route.

Narrow regressions were added to `tests/test_feature_batch_ui.js`: the LED/recovery Save controls
are asserted to be wired through `bindDirty`, and `recoveryPanel()` is asserted to mark only
`recovery-ap` active (client/armed/stopped/stopping/unavailable/handover are not). Both fail on
the pre-fix source and pass on the fix.

## Server-side note

An earlier run of this suite aborted the mock daemon with a glibc buffer-overflow on
`POST /api/v1/audio/noise` (confirmed independently with curl: empty reply, process terminated),
which knocked the server out from under the later cases. That is the server change's domain; a
rebuilt daemon now serves the same request without aborting and the full suite passes. The
nursery sounds case is kept last in the case list so a future server regression there cannot mask
the other cases.
