'use strict';

/*
 * Real-browser acceptance tests for the 0.14 feature batch.
 *
 * These run the SHIPPED page scripts (web/js/app.js and its siblings) in a real
 * Chromium, against the production mock web backend, and drive every control
 * through its actual on-page event handler. That is deliberately different from
 * tests/test_feature_batch_ui.js, which evaluates the same source under a Node
 * DOM shim and calls handlers directly: a shim call cannot see a control that
 * the browser renders disabled, or a handler that was never bound after a
 * re-render or a full page load.
 *
 * Covered here:
 *   - the page's real script list actually loads and executes (not a shim);
 *   - LED idle/sleep controls save, across the initial load and re-renders;
 *   - nursery sounds source/bed/tempo/fade/timer;
 *   - the USB .opus Play button appears only when the image advertises the
 *     decoder, from the real listing DOM;
 *   - recent voice list/detail/clear, including the generation-scoped stale
 *     guard that must not paint a superseded detail;
 *   - the owner-prepared recovery password: shown once, escaped, never cached
 *     or persisted to browser storage;
 *   - the login-only setup path: a configured owner is routed to sign-in, and
 *     the one-time account step is never reopened.
 *
 * Browser resolution: the CI job installs Playwright with its own browser. On a
 * host without it (like a dev box that only has system Chrome) the launch falls
 * back to channel "chrome" and then an explicit executable.
 */

const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const baseURL = (process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083').replace(/\/+$/, '');
const authURL = (process.env.LIBREECHO_E2E_AUTH_URL || '').replace(/\/+$/, '');
const authUser = process.env.LIBREECHO_E2E_AUTH_USER || 'recovery-owner';
const authPass = process.env.LIBREECHO_E2E_AUTH_PASSWORD || 'recovery-password-123';

const failures = [];
function check(ok, message) {
  if (ok) {
    console.log('  ok: ' + message);
  } else {
    failures.push(message);
    console.error('  FAIL: ' + message);
  }
  return ok;
}
function checkEqual(actual, expected, message) {
  return check(actual === expected, `${message} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

async function launchBrowser() {
  const attempts = [
    ['bundled', {}],
    ['channel-chrome', { channel: 'chrome' }],
    ['explicit-chrome', { executablePath: process.env.LIBREECHO_CHROME || '/usr/bin/google-chrome' }],
  ];
  let lastError = null;
  for (const [name, options] of attempts) {
    try {
      const browser = await chromium.launch(options);
      console.log(`browser: chromium (${name})`);
      return browser;
    } catch (error) {
      lastError = error;
    }
  }
  throw new Error(`no Chromium could be launched: ${lastError && lastError.message}`);
}

function recordRequests(page) {
  const requests = [];
  page.on('request', request => {
    const url = request.url();
    const at = url.indexOf('/api/v1/');
    if (at < 0) return;
    /* Store the API-relative path (/led/idle), matching the path literals the
       checks below pass to calls()/lastBody(). */
    requests.push({ method: request.method(), path: url.slice(at + '/api/v1'.length), body: request.postData() });
  });
  return requests;
}
function calls(requests, path, method) {
  return requests.filter(r => r.path === path && (!method || r.method === method));
}
function lastBody(requests, path, method) {
  const found = calls(requests, path, method);
  return found.length ? found[found.length - 1].body : null;
}
function envelope(data) {
  return JSON.stringify({ ok: true, data, error: null });
}
async function routeJson(page, glob, handler) {
  await page.route(glob, async route => {
    const result = await handler(route);
    if (result === undefined || result === null) return route.continue();
    await route.fulfill({ contentType: 'application/json', body: JSON.stringify({ ok: true, data: result, error: null }) });
  });
}
async function waitForPage(page, title) {
  await page.waitForFunction(expected => {
    const heading = document.querySelector('#page-title');
    const content = document.querySelector('#content');
    return heading && heading.textContent.trim() === expected &&
      content && content.textContent.trim().length > 0 &&
      !content.querySelector('.loading');
  }, title, { timeout: 10000 });
}
function serverGet(path) {
  return `${baseURL}${path}`;
}

/* -------------------------------------------------------------- case: scripts */

async function caseRealPageScriptList(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const failed = [];
  page.on('pageerror', error => failed.push(`pageerror: ${error.message}`));
  page.on('response', response => {
    const type = response.request().resourceType();
    if (response.status() >= 400 && ['document', 'script', 'stylesheet'].includes(type)) {
      failed.push(`${response.status()} ${type}: ${response.url()}`);
    }
  });
  await page.goto(`${baseURL}/`);
  await waitForPage(page, 'Overview');

  const loaded = await page.evaluate(() =>
    Array.from(document.querySelectorAll('script[src]')).map(s => s.getAttribute('src')));
  for (const expected of ['/js/app.js', '/js/bluetooth.js', '/js/integrations-ui.js', '/js/privacy-ui.js']) {
    check(loaded.some(src => src.startsWith(expected)), `page loads the real ${expected}`);
  }

  const indexHtml = await (await context.request.get(`${baseURL}/`)).text();
  const declared = Array.from(indexHtml.matchAll(/<script[^>]+src="([^"]+)"/g)).map(m => m[1]);
  check(declared.length >= 4, `index.html declares the shipped script list (${declared.length} scripts)`);
  for (const src of declared) {
    const resource = await page.evaluate(url => {
      const entry = performance.getEntriesByType('resource').find(r => new URL(r.name).pathname === new URL(url, location.origin).pathname);
      return entry ? { loaded: true, size: entry.transferSize } : { loaded: false };
    }, src);
    check(resource.loaded, `declared script ${src} was fetched by the browser`);
  }

  const globals = await page.evaluate(() => ({
    ledPage: typeof window.ledPage,
    voiceHistoryPanel: typeof window.voiceHistoryPanel,
    recoveryPanel: typeof window.recoveryPanel,
  }));
  checkEqual(globals.ledPage, 'function', 'the real app.js executed (ledPage is defined)');
  checkEqual(globals.voiceHistoryPanel, 'function', 'the real app.js executed (voiceHistoryPanel is defined)');
  checkEqual(globals.recoveryPanel, 'function', 'the real app.js executed (recoveryPanel is defined)');
  check(failed.length === 0, `no page errors or failed script loads (${failed.join('; ')})`);
  await context.close();
}

/* ---------------------------------------------------------- case: LED controls */

async function caseLedSave(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const requests = recordRequests(page);
  await page.goto(`${baseURL}/led-buttons`);
  await waitForPage(page, 'LED & Buttons');

  check(await page.locator('#led-idle-mode').count() === 1, 'LED page renders the idle-behaviour control');
  check(await page.locator('#led-sleep-mode').count() === 1, 'LED page renders the sleep-light control');
  check(await page.locator('#led-sleep-brightness').count() === 1, 'sleep light exposes a brightness ceiling');

  /* Idle behaviour: edit then save through the real button. If the control is
     not made usable by the edit this fails here — there is no force-enable or
     direct handler invocation to hide a disabled Save button. */
  await page.selectOption('#led-idle-mode', 'always');
  if (check(await page.isEnabled('#save-led-idle'), 'editing idle behaviour enables its Save button (real browser click)')) {
    await page.click('#save-led-idle');
    await page.waitForResponse(r => r.url().includes('/api/v1/led/idle') && r.request().method() === 'PUT', { timeout: 5000 });
    checkEqual(lastBody(requests, '/led/idle', 'PUT'), JSON.stringify({ mode: 'always' }), 'saving idle behaviour PUTs the mode');
    const led = await (await context.request.get(serverGet('/api/v1/led'))).json();
    checkEqual(led.data.idle_mode, 'always', 'the device reflects the saved idle mode');
  }

  /* Sleep light. The restore toggle is a styled checkbox (the input itself is
     visually hidden), so a user clicks the visible switch next to it. */
  await page.selectOption('#led-sleep-mode', 'solid');
  await page.fill('#led-sleep-brightness', '4');
  await page.selectOption('#led-sleep-period', '5000');
  await page.selectOption('#led-sleep-timer', '60');
  await page.click('#led-sleep-restore + .switch');
  check(await page.isChecked('#led-sleep-restore'), 'the restore sleep light toggle responds to a real click');
  if (check(await page.isEnabled('#save-led-sleep'), 'editing the sleep light enables its Save button (real browser click)')) {
    await page.click('#save-led-sleep');
    await page.waitForResponse(r => r.url().includes('/api/v1/led/sleep') && r.request().method() === 'PUT', { timeout: 5000 });
    checkEqual(lastBody(requests, '/led/sleep', 'PUT'),
      JSON.stringify({ mode: 'solid', brightness: 4, period_ms: 5000, timer_minutes: 60, restore_on_boot: true }),
      'saving the sleep light PUTs mode/brightness/period/timer/restore');
    const led = await (await context.request.get(serverGet('/api/v1/led'))).json();
    check(led.data.sleep_light && led.data.sleep_light.mode === 'solid' && led.data.sleep_light.brightness === 4,
      'the device reflects the saved sleep light');
  }

  /* Re-render and full reload must re-bind the same real handlers. */
  await page.locator('#nav').getByText('Audio', { exact: true }).click();
  await waitForPage(page, 'Audio');
  await page.locator('#nav').getByText('LED & Buttons', { exact: true }).click();
  await waitForPage(page, 'LED & Buttons');
  check(await page.evaluate(() => typeof document.querySelector('#save-led-sleep').onclick === 'function'),
    'the sleep Save handler is re-bound after an SPA re-render');
  await page.reload();
  await waitForPage(page, 'LED & Buttons');
  check(await page.evaluate(() => typeof document.querySelector('#save-led-idle').onclick === 'function' &&
    typeof document.querySelector('#save-led-sleep').onclick === 'function'),
    'both Save handlers are bound after a full page reload');
  await context.close();
}

/* --------------------------------------------------------- case: nursery sounds */

async function caseNurserySounds(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const requests = recordRequests(page);
  await page.goto(`${baseURL}/audio`);
  await waitForPage(page, 'Audio');

  check(await page.locator('#noise-colour').count() === 1, 'Audio page renders the nursery-sounds panel');
  check((await page.locator('#noise-tempo').getAttribute('min')) === '40' &&
    (await page.locator('#noise-tempo').getAttribute('max')) === '100', 'tempo is bounded 40-100 bpm');

  await page.selectOption('#noise-colour', 'heartbeat');
  await page.selectOption('#noise-bed', 'brown');
  await page.fill('#noise-tempo', '72');
  await page.fill('#noise-level', '30');
  await page.fill('#noise-fade', '120');
  await page.selectOption('#noise-minutes', '60');
  const startDisabled = await page.evaluate(() => document.querySelector('#noise-start').disabled);
  check(!startDisabled, 'the nursery-sounds Start control is clickable');
  await page.click('#noise-start');
  await page.waitForResponse(r => r.url().includes('/api/v1/audio/noise') && r.request().method() === 'POST', { timeout: 5000 });
  checkEqual(lastBody(requests, '/audio/noise', 'POST'),
    JSON.stringify({ colour: 'heartbeat', source: 'heartbeat', bed: 'brown', level: 30, minutes: 60, tempo: 72, fade_seconds: 120 }),
    'starting nursery sounds POSTs source/bed/tempo/fade/timer/level');

  const audio = await (await context.request.get(serverGet('/api/v1/audio'))).json();
  check(audio.data.noise && audio.data.noise.source === 'heartbeat' && audio.data.noise.bed === 'brown' &&
    audio.data.noise.tempo === 72 && audio.data.noise.fade_seconds === 120,
    'the device reflects the nursery source/bed/tempo/fade');

  /* A fade longer than the timer is refused before any request leaves. */
  const before = calls(requests, '/audio/noise', 'POST').length;
  await page.selectOption('#noise-minutes', '15');
  await page.fill('#noise-fade', '3000');
  await page.evaluate(() => document.querySelector('#noise-start').click());
  await page.waitForTimeout(250);
  checkEqual(calls(requests, '/audio/noise', 'POST').length, before, 'a fade longer than the timer is rejected client-side');

  /* Stop uses the same panel and issues a DELETE. */
  await page.evaluate(() => document.querySelector('#noise-stop').click());
  await page.waitForResponse(r => r.url().includes('/api/v1/audio/noise') && r.request().method() === 'DELETE', { timeout: 5000 });
  checkEqual(calls(requests, '/audio/noise', 'DELETE').length, 1, 'Stop DELETEs the nursery-sounds endpoint');
  await context.close();
}

/* -------------------------------------------------------- case: USB capability */

async function caseUsbCapability(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  let formats = ['mp3', 'opus'];
  const requests = recordRequests(page);
  await routeJson(page, '**/api/v1/system/features', () => ({
    simulation: false, https: false, usb_role_supported: true, usb_host: true,
    acoustic_events: false, acoustic_events_available: false,
  }));
  await routeJson(page, '**/api/v1/storage/usb**', () => ({
    present: true, mounted: true, device: 'sda', partition: 'sda1', filesystem: 'vfat',
    size_bytes: 1000, used_bytes: 100, free_bytes: 900, rel_path: '', playable_formats: formats,
    entries: [
      { name: 'song.mp3', directory: false, size_bytes: 10 },
      { name: 'clip.opus', directory: false, size_bytes: 20 },
    ],
  }));
  await page.route('**/api/v1/storage/usb/play', route => route.fulfill({ contentType: 'application/json', body: envelope({ playing: true }) }));

  await page.goto(`${baseURL}/system`);
  await waitForPage(page, 'System');
  await page.waitForSelector('#usb-storage .usb-list', { timeout: 8000 });
  let html = await page.locator('#usb-storage').innerHTML();
  check(/clip\.opus[\s\S]{0,120}usb-play|usb-play[\s\S]{0,120}clip\.opus/.test(html), 'with Opus advertised, a .opus file gets a Play button');
  check(html.includes('MP3, OPUS'), 'the playable-formats line advertises MP3 and OPUS');
  await page.click('#usb-storage .usb-play[data-rel="clip.opus"]');
  await page.waitForResponse(r => r.url().includes('/api/v1/storage/usb/play'), { timeout: 5000 });
  checkEqual(lastBody(requests, '/storage/usb/play', 'POST'), JSON.stringify({ path: 'clip.opus' }),
    'the Play button POSTs the confined relative path');

  /* Re-render with the decoder absent: .opus is listed but not playable. */
  formats = ['mp3'];
  await page.locator('#nav').getByText('Overview', { exact: true }).click();
  await waitForPage(page, 'Overview');
  await page.locator('#nav').getByText('System', { exact: true }).click();
  await waitForPage(page, 'System');
  await page.waitForSelector('#usb-storage .usb-list', { timeout: 8000 });
  html = await page.locator('#usb-storage').innerHTML();
  check(/song\.mp3[\s\S]{0,120}usb-play|usb-play[\s\S]{0,120}song\.mp3/.test(html), 'mp3 still offers Play without an Opus decoder');
  check(!/clip\.opus[\s\S]{0,120}usb-play/.test(html), 'without Opus advertised, .opus offers no Play');
  check(html.includes('Opus decode is not available on this image'), 'the .opus file explains why it is not playable');
  await context.close();
}

/* ------------------------------------------------------------ case: voice history */

function voiceStamp(id) {
  /* Matches the canonical voice_history.c serialization: ISO 8601 with a
     numeric offset, e.g. 2026-09-30T21:00:12+00:00 (no milliseconds). */
  return new Date(Date.UTC(2026, 8, 30, 21, 0, id)).toISOString().replace(/\.000Z$/, '+00:00');
}
function voiceTurns() {
  const row = (id, status, preview) => ({
    id, timestamp: voiceStamp(id), status,
    transcript_preview: preview, response_preview: '',
    transcript_truncated: false, response_truncated: false,
    transcript_length: 0, response_length: 0, stt_ms: 0, assistant_ms: 0, tts_ms: 0, error: null,
  });
  return [
    row(6, 'complete', 'sixth'),
    row(12, 'complete', 'twelfth'),
    row(3, 'complete', 'third'),
    row(11, 'error', 'eleventh'),
    row(1, 'complete', 'first'),
    row(10, 'complete', 'tenth'),
    row(2, 'complete', 'second'),
    row(9, 'complete', 'ninth'),
    row(4, 'complete', 'fourth'),
    row(8, 'complete', 'eighth'),
    row(5, 'complete', 'fifth'),
    row(7, 'complete', 'seventh'),
  ];
}

async function caseVoiceHistory(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const requests = recordRequests(page);
  const STALE = 'STALE-TRANSCRIPT-MARKER';
  await routeJson(page, '**/api/v1/system/features', () => ({
    simulation: true, https: false, usb_role_supported: false, usb_host: false,
    acoustic_events: false, acoustic_events_available: false,
  }));
  await page.route('**/api/v1/assistant/history', async route => {
    if (route.request().method() === 'DELETE') return route.fulfill({ contentType: 'application/json', body: envelope({}) });
    return route.fulfill({ contentType: 'application/json', body: envelope({ history_generation: 4, capacity: 10, count: 12, preview_chars: 64, turns: voiceTurns() }) });
  });
  await page.route('**/api/v1/assistant/history/*', async route => {
    const id = route.request().url().split('/').pop();
    if (id === '3') await new Promise(resolve => setTimeout(resolve, 700));
    /* The ring is newest-first capped at 10, so the rendered ids are
       12,11,...,3; ids 1 and 2 fall outside the cap and are never painted. */
    const transcript = id === '4' ? '<b>hello</b>' : (id === '3' ? STALE : 'transcript ' + id);
    return route.fulfill({ contentType: 'application/json', body: envelope({ id: Number(id), transcript, response: 'reply ' + id, status: 'complete' }) });
  });

  await page.goto(`${baseURL}/simulation`);
  await waitForPage(page, 'Simulation');
  await page.waitForSelector('#voice-history .voice-turn', { timeout: 8000 });

  const order = await page.$$eval('#voice-history .voice-turn', nodes => nodes.map(n => Number(n.dataset.id)));
  checkEqual(order.length, 10, 'the recent-voice ring is capped at 10 rows');
  checkEqual(order[0], 12, 'recent voice is newest-first');
  check(order.includes(3) && !order.includes(2), 'only the newest 10 turns are painted (older turns are dropped)');
  /* Prove the canonical ISO timestamp is actually rendered: a formatter that
     only reads the legacy at_ms/at fields paints — for every canonical row. */
  const times = await page.$$eval('#voice-history .voice-turn time', nodes => nodes.map(n => n.textContent.trim()));
  checkEqual(times.length, 10, 'every canonical row renders a <time>');
  check(times.every(v => v && v !== '—'), 'no canonical row shows the placeholder dash for a valid timestamp');
  const newestTime = await page.locator('#voice-history .voice-turn[data-id="12"] time').innerText();
  const expectedNewest = await page.evaluate(iso => new Date(iso).toLocaleTimeString(), voiceStamp(12));
  checkEqual(newestTime, expectedNewest, 'the newest row shows its canonical ISO timestamp');
  const errorStatus = await page.locator('#voice-history .status.error').first().innerText().catch(() => '');
  check(errorStatus.toLowerCase() === 'error', 'a failed turn renders its error status');

  /* Detail fetch and escaping (id 4 is inside the painted cap). */
  await page.click('#voice-history .voice-turn[data-id="4"] .voice-detail');
  await page.waitForResponse(r => r.url().includes('/api/v1/assistant/history/4'), { timeout: 5000 });
  await page.waitForSelector('#voice-history .voice-turn[data-id="4"] .voice-detail-body dl', { timeout: 5000 });
  const escaped = await page.evaluate(() => {
    const body = document.querySelector('#voice-history .voice-turn[data-id="4"] .voice-detail-body');
    return { text: body.textContent, hasElement: !!body.querySelector('b') };
  });
  check(escaped.text.includes('<b>hello</b>') && !escaped.hasElement, 'the transcript is shown as text, never injected as markup');

  /* Stale guard: opening the delayed id-3 detail, then Clear bumps the
     generation before that detail response lands — it must never paint. */
  await page.click('#voice-history .voice-turn[data-id="3"] .voice-detail');
  await page.click('#voice-clear');
  await page.waitForResponse(r => r.url().includes('/api/v1/assistant/history') && r.request().method() === 'DELETE', { timeout: 5000 });
  await page.waitForTimeout(1200);
  const afterClear = await page.locator('#voice-history').innerText();
  check(afterClear.includes('No voice turns recorded yet'), 'Clear empties the recent-voice list');
  check(!afterClear.includes(STALE), 'a detail response superseded by Clear never overwrites the list (stale guard)');
  check(calls(requests, '/assistant/history', 'GET').length >= 1, 'the list is loaded from the collection endpoint');
  await context.close();
}

/* --------------------------------------------------- case: recovery password */

async function caseRecoverySecret(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  await routeJson(page, '**/api/v1/network/recovery/prepare', () => ({
    ssid: 'LibreEcho-<Setup>', psk: 'abc<X>def',
  }));

  await page.goto(`${baseURL}/network`);
  await waitForPage(page, 'Network');
  const before = await page.evaluate(() => JSON.stringify([Object.entries(localStorage), Object.entries(sessionStorage)]));
  check(await page.evaluate(() => document.querySelector('#recovery-secret').hidden), 'the secret region starts hidden');

  /* Wait for the prepare response itself rather than racing the click. */
  const [prepareResponse] = await Promise.all([
    page.waitForResponse(r => r.url().includes('/api/v1/network/recovery/prepare')),
    page.click('#recovery-prepare'),
  ]);
  await prepareResponse.finished().catch(() => {});
  const secret = await page.evaluate(() => {
    const box = document.querySelector('#recovery-secret');
    return { hidden: box.hidden, text: box.textContent, hasElement: !!box.querySelector('x') };
  });
  check(!secret.hidden, 'the recovery secret is revealed only after the explicit Prepare action');
  check(secret.text.includes('LibreEcho-<Setup>') && !secret.hasElement, 'the prepared SSID is rendered as escaped text');
  check(secret.text.includes('abc<X>def'), 'the recovery password is shown as text, not injected markup');
  check(secret.text.includes('shown only once'), 'the secret is labelled as shown only once');
  /* route.fulfill serves the mocked body above, so the real Cache-Control
     header is verified against the device itself: context.request bypasses
     page routing. */
  const config = await (await context.request.get(serverGet('/api/v1/config'))).json();
  const directPrepare = await context.request.post(serverGet('/api/v1/network/recovery/prepare'), {
    headers: { 'X-LibreEcho-CSRF': config.data.csrf_token, 'Content-Type': 'application/json' },
    data: '{}',
  });
  const header = (await directPrepare.headers())['cache-control'] || '';
  check(/no-store/.test(header), `the device marks the prepare response Cache-Control: no-store (got ${JSON.stringify(header)})`);

  const after = await page.evaluate(() => JSON.stringify([Object.entries(localStorage), Object.entries(sessionStorage)]));
  checkEqual(after, before, 'preparing the secret writes nothing to localStorage or sessionStorage');
  check(!after.includes('abc<X>def') && !after.includes('abc&lt;X&gt;def'), 'the recovery password never reaches browser storage');
  await context.close();
}

/* -------------------------------------- case: recovery settings save (extra) */

async function caseRecoverySave(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const requests = recordRequests(page);
  await page.goto(`${baseURL}/network`);
  await waitForPage(page, 'Network');
  check(await page.locator('#save-recovery').count() === 1, 'the Network page renders the recovery settings Save control');
  const recoveryControls = await page.evaluate(() => ({
    enabled: document.querySelector('#recovery-enabled').checked,
    auto_enabled: document.querySelector('#recovery-auto').checked,
  }));
  await page.fill('#recovery-timeout', '300');
  if (check(await page.isEnabled('#save-recovery'), 'editing the recovery timeout enables its Save button (real browser click)')) {
    await page.click('#save-recovery');
    await page.waitForResponse(r => r.url().includes('/api/v1/network/recovery') && r.request().method() === 'PUT', { timeout: 5000 });
    checkEqual(lastBody(requests, '/network/recovery', 'PUT'),
      JSON.stringify({ enabled: recoveryControls.enabled, auto_enabled: recoveryControls.auto_enabled, timeout_seconds: 300 }),
      'saving recovery PUTs the control states with the timeout');
  }
  await context.close();
}

/* --------------------------------------------------- case: login-only setup */

async function caseLoginOnlySetup(browser) {
  if (!authURL) {
    check(false, 'LIBREECHO_E2E_AUTH_URL is required to verify the login-only setup path');
    return;
  }
  const context = await browser.newContext();
  const page = await context.newPage();
  const requests = recordRequests(page);
  await page.goto(`${authURL}/setup.html?recovery=1`);
  await page.waitForSelector('#recovery-login.active', { timeout: 8000 });

  const state = await page.evaluate(() => {
    /* The shipped setup.html has no #step0 id: the account step is the first
       .setup-page section carrying data-step="0". */
    const wizard = document.querySelector('.setup-page[data-step="0"]');
    const actions = document.querySelector('#setup-actions');
    return {
      wizardFound: !!wizard,
      step0Active: wizard ? wizard.classList.contains('active') : null,
      step0Hidden: wizard ? wizard.hidden : null,
      actionsHidden: actions ? actions.hidden : null,
      bodyText: document.body.innerText,
    };
  });
  check(state.wizardFound, 'the one-time account step exists in the shipped setup page');
  checkEqual(state.step0Active, false, 'a configured device never activates the one-time account step');
  checkEqual(state.step0Hidden, true, 'the one-time account step is hidden on the recovery landing');
  check(state.actionsHidden === true, 'the setup wizard footer is hidden on the recovery landing');
  check(!state.bodyText.includes('Create your local account') && !state.bodyText.includes('Create your first local account'),
    'no account-creation copy is shown to a configured device');
  checkEqual(calls(requests, '/auth/bootstrap', 'POST').length, 0, 'the recovery landing never calls account creation');

  await page.fill('#recovery-username', authUser);
  await page.fill('#recovery-password', authPass);
  await page.click('#recovery-signin');
  await page.waitForSelector('#recovery-network.active', { timeout: 8000 });
  check(await page.evaluate(() => document.querySelector('#recovery-login').hidden), 'the sign-in page is hidden after signing in');
  checkEqual(calls(requests, '/auth/login', 'POST').length, 1, 'sign-in POSTs the login endpoint');
  checkEqual(calls(requests, '/auth/bootstrap', 'POST').length, 0, 'signing in never creates an account');
  check(calls(requests, '/network/wifi/scan', 'GET').length >= 1, 'the signed-in owner reaches the Wi-Fi-only recovery page');

  /* A plain configured visit routes to the login page, never to setup. */
  const second = await context.newPage();
  const secondRequests = recordRequests(second);
  await second.goto(`${authURL}/`);
  await second.waitForSelector('#login-form', { timeout: 8000 });
  check(new URL(second.url()).pathname === '/login', 'a plain configured visit is routed to /login');
  checkEqual(calls(secondRequests, '/auth/bootstrap', 'POST').length, 0, 'the login route never offers account creation');
  const loginText = await second.locator('body').innerText();
  check(!loginText.includes('Create your first local account'), 'the login page shows no bootstrap account form');
  await context.close();
}

/* Regression (Codex review on 5660cc2): a stale sessionStorage token used to
   skip recovery sign-in by truthiness alone. It is now validated against
   /api/v1/auth; an expired session lands on sign-in with the token cleared. */
async function caseRecoveryExpiredSession(browser) {
  if (!authURL) {
    check(false, 'LIBREECHO_E2E_AUTH_URL is required to verify the expired recovery session');
    return;
  }
  const context = await browser.newContext();
  const page = await context.newPage();
  const requests = recordRequests(page);
  await page.goto(`${authURL}/login`);
  await page.evaluate(() => sessionStorage.setItem('libreecho-token', 'expired-e2e-token'));
  await page.goto(`${authURL}/setup.html?recovery=1`);
  await page.waitForSelector('#recovery-login.active', { timeout: 8000 });
  check(calls(requests, '/auth', 'GET').length >= 1, 'an expired recovery token is validated against /auth');
  checkEqual(await page.evaluate(() => sessionStorage.getItem('libreecho-token')), null,
    'the expired recovery token is cleared from sessionStorage');
  checkEqual(calls(requests, '/network/wifi/scan', 'GET').length, 0,
    'the expired session never reaches the Wi-Fi scan');
  check(await page.evaluate(() => document.querySelector('#recovery-network').hidden),
    'the Wi-Fi page stays hidden for the expired session');
  await context.close();
}

/* ------------------------------------------------------------------- main */

async function main() {
  console.log(`feature-batch browser e2e against ${baseURL}`);
  const browser = await launchBrowser();
  const cases = [
    ['real page script list executes', caseRealPageScriptList],
    ['LED idle/sleep controls save', caseLedSave],
    ['USB .opus capability button', caseUsbCapability],
    ['recent voice list/detail/clear stale guard', caseVoiceHistory],
    ['recovery owner prepare no-store / no storage', caseRecoverySecret],
    ['recovery settings save is usable', caseRecoverySave],
    ['login-only setup never reopens bootstrap', caseLoginOnlySetup],
    ['recovery expired session returns to sign-in', caseRecoveryExpiredSession],
    /* Nursery sounds runs last: its POST /audio/noise currently aborts the mock
       daemon (server-side buffer overflow owned by the server change), which
       would otherwise knock the server out from under the cases above. */
    ['nursery sounds source/fade/tempo', caseNurserySounds],
  ];
  for (const [name, run] of cases) {
    console.log('case: ' + name);
    try {
      await run(browser);
    } catch (error) {
      failures.push(`${name}: ${error && error.message ? error.message : error}`);
      console.error(`  FAIL: ${name} threw: ${error && error.stack ? error.stack : error}`);
    }
  }
  await browser.close();
  if (failures.length) {
    console.error(`\nfeature-batch browser e2e: ${failures.length} check(s) failed`);
    for (const failure of failures) console.error(' - ' + failure);
    process.exit(1);
  }
  console.log('\nfeature-batch browser e2e: ok');
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
