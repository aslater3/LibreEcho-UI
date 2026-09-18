/*
 * Browser coverage for the ChatGPT account gate on the Integrations page
 * (UI #272), run against the real served assets in Chromium.
 *
 * The voice-assistant daemon is simulated through route interception so the
 * suite is deterministic on any host: the subject is the browser's own
 * behaviour. A disabled switch must not be turnable, the account panels must
 * stay open while a device code is showing (the auth poll re-renders the page
 * every few seconds), and the panel status must read differently for a missing
 * GPT-Live service, a signed-out account, an authenticated-but-off provider and
 * an enabled one.
 */
'use strict';

const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const baseURL = process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083';
const envelope = data => ({ ok: true, data, error: null });
const failure = message => ({ ok: false, data: null, error: { code: 7, message } });

function captureBrowserFailures(page) {
  const failures = [];
  page.on('pageerror', error => failures.push(`pageerror: ${error.message}`));
  page.on('console', message => {
    if (message.type() !== 'error') return;
    const text = message.text();
    if (/^Failed to load resource: the server responded with a status of \d+/.test(text)) return;
    failures.push(`console.error: ${text}`);
  });
  page.on('response', response => {
    const type = response.request().resourceType();
    if (response.status() >= 400 && ['document', 'script', 'stylesheet', 'image'].includes(type)) {
      failures.push(`${response.status()} ${type}: ${response.url()}`);
    }
  });
  return failures;
}

async function waitForPage(page, title) {
  await page.waitForFunction(expected => {
    const heading = document.querySelector('#page-title');
    const content = document.querySelector('#content');
    return heading && heading.textContent.trim() === expected &&
      content && content.textContent.trim().length > 0 &&
      !content.querySelector('.loading');
  }, title, { timeout: 7000 });
}

async function selectPage(page, name) {
  const item = page.locator('#nav').getByText(name, { exact: true });
  await item.waitFor({ state: 'visible', timeout: 5000 });
  await item.click();
  await waitForPage(page, name);
}

/*
 * A stand-in daemon: it answers the handful of endpoints this page reads and
 * keeps the state the panel is supposed to reflect, so a click that is allowed
 * to reach the API has a visible effect.
 */
function fakeDaemon() {
  return {
    liveSupported: true,
    calls: [],
    assistant: {
      ready: true, provider: 'openai-codex', provider_name: 'ChatGPT',
      subscription_auth: true, enabled: false, authenticated: false,
      auth_state: 'signed_out', user_code: '', verification_url: '', auth_error: '',
      model: 'gpt-5.4', prompt: 'Reply briefly.', clock_format: '12',
      base_url: '', api_key_configured: false,
      wake_connected: true, audio_connected: true, recognizing: false,
      completed_transcripts: 0, last_stt_audio_ms: 0, last_stt_processing_ms: 0,
      last_speech_end_to_first_pcm_ms: 0, latency_target_ms: 3000,
      home_location: 'Fixture', latitude: '51', longitude: '0', weather_provider: 'off'
    },
    live: {
      enabled: false, mode: 'inactive', transport: 'realtime', last_event: 'idle',
      session: { state: 'idle', last_end: 'none', sessions_completed: 0, delegations: 0 },
      transport_metrics: { transport: 'websocket', session_ready: false }
    }
  };
}

async function intercept(page, fake) {
  await page.route('**/api/v1/**', async route => {
    const request = route.request();
    const path = new URL(request.url()).pathname.replace('/api/v1', '');
    const method = request.method();
    let body = null;
    try { body = request.postDataJSON(); } catch (_) { body = null; }
    const json = data => route.fulfill({
      contentType: 'application/json', body: JSON.stringify(envelope(data))
    });

    if (path !== '/status' && path !== '/config' && path !== '/auth') {
      fake.calls.push({ path, method, body });
    }

    if (path === '/integrations' && method === 'GET') {
      return json({ items: [
        { id: 'home-assistant', name: 'Home Assistant', enabled: false },
        { id: 'spotify', name: 'Spotify', enabled: false, installed: false }
      ] });
    }
    if (path === '/assistant') {
      if (method === 'GET') return json(fake.assistant);
      Object.assign(fake.assistant, body || {});
      return json(fake.assistant);
    }
    if (path === '/assistant/auth/start' && method === 'POST') {
      fake.assistant.auth_state = 'waiting';
      fake.assistant.authenticated = false;
      fake.assistant.user_code = 'WXYZ-4242';
      fake.assistant.verification_url = 'https://chatgpt.com/device';
      return json(fake.assistant);
    }
    if (path === '/assistant/auth/poll' && method === 'POST') return json(fake.assistant);
    if (path === '/assistant/logout' && method === 'POST') {
      fake.assistant.authenticated = false;
      fake.assistant.auth_state = 'signed_out';
      fake.assistant.user_code = '';
      fake.assistant.verification_url = '';
      return json(fake.assistant);
    }
    if (path === '/live') {
      if (method === 'PUT') {
        fake.live.enabled = Boolean(body && body.enabled);
        return json({ enabled: fake.live.enabled });
      }
      if (!fake.liveSupported) {
        return route.fulfill({ status: 503, contentType: 'application/json',
          body: JSON.stringify(failure('GPT-Live is not installed in this image')) });
      }
      return json(fake.live);
    }
    if (path === '/voice-pipeline') return json({ mode: 'local', stt: {}, tts: {} });
    if (path === '/privacy') return json({});
    return route.continue();
  });
}

function panel(page, child) {
  return page.locator('details.assistant-provider').filter({ has: page.locator(child) });
}
async function statusOf(page, child) {
  return (await panel(page, child).locator('summary .assistant-state').innerText()).trim();
}
function enableCalls(fake) {
  return fake.calls.filter(call => call.method !== 'GET' && call.body && call.body.enabled === true);
}

(async () => {
  const browser = await chromium.launch({ headless: true });
  const fake = fakeDaemon();
  try {
    const context = await browser.newContext({ baseURL });
    const page = await context.newPage();
    const failures = captureBrowserFailures(page);
    await intercept(page, fake);

    /* --- signed out ----------------------------------------------------- */
    await page.goto('/', { waitUntil: 'domcontentloaded' });
    await waitForPage(page, 'Overview');
    await selectPage(page, 'Integrations');

    assert.equal(await panel(page, '#use-device-provider').evaluate(el => el.open), true,
      'the signed-out device panel must be open so the sign-in control is reachable');
    assert.equal(await statusOf(page, '#use-device-provider'), 'Sign in required');
    assert.equal(await page.locator('#use-device-provider').isDisabled(), true,
      'the device assistant switch must be disabled while signed out');
    assert.equal(await panel(page, '#use-live-provider').evaluate(el => el.open), true,
      'the GPT-Live panel must offer the shared sign-in while signed out');
    assert.equal(await statusOf(page, '#use-live-provider'), 'Sign in required');
    assert.equal(await page.locator('#use-live-provider').isDisabled(), true,
      'the GPT-Live switch must be disabled while the account is signed out');
    assert.equal(await page.locator('#use-local-provider').isDisabled(), false,
      'Local LLM must not be gated on the ChatGPT account');

    /* The visible half of the prerequisite: a real click on a disabled switch
       is a no-op, and the adversarial path (a change event that arrives anyway)
       must not reach the API either. */
    await page.locator('#use-device-provider').click({ force: true });
    await page.evaluate(() => {
      const input = document.querySelector('#use-device-provider');
      input.checked = true;
      input.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await page.evaluate(() => {
      const input = document.querySelector('#use-live-provider');
      input.checked = true;
      input.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await page.waitForTimeout(500);
    assert.deepEqual(enableCalls(fake).map(call => call.path), [],
      'a signed-out account must not produce an enable request from the browser');
    assert.equal(await page.locator('#use-device-provider').isChecked(), false,
      'the device switch must not stay on while the account is signed out');

    /* Sign-in is offered without enabling or selecting the assistant. */
    assert.equal(await page.locator('#assistant-auth-start').isVisible(), true,
      'Connect ChatGPT must be offered on the signed-out device panel');
    assert.equal(await page.locator('#live-auth-start').isVisible(), true,
      'GPT-Live must offer the shared Connect control as well');
    await page.locator('#assistant-auth-start').click();
    await page.waitForFunction(() =>
      Boolean(document.querySelector('.device-code strong')), null, { timeout: 5000 });
    assert.ok(fake.calls.some(call => call.path === '/assistant/auth/start' && call.method === 'POST'),
      'Connect ChatGPT did not start device login');

    /* --- waiting for sign-in, across auth polls -------------------------- */
    assert.equal(await statusOf(page, '#use-device-provider'), 'Waiting for sign-in');
    assert.equal(await page.locator('.device-code strong').innerText(), 'WXYZ-4242');
    assert.equal(await page.locator('.device-code a').getAttribute('href'), 'https://chatgpt.com/device');
    assert.equal(await page.locator('#assistant-auth-poll').isVisible(), true);

    const polls = () => fake.calls.filter(call => call.path === '/assistant/auth/poll').length;
    const deadline = Date.now() + 12000;
    while (polls() < 2 && Date.now() < deadline) await page.waitForTimeout(250);
    assert.ok(polls() >= 2, 'a pending device login must keep being polled');
    assert.equal(await panel(page, '#use-device-provider').evaluate(el => el.open), true,
      'the auth-poll re-render collapsed the panel holding the device code');
    assert.equal(await page.locator('.device-code strong').isVisible(), true,
      'the auth-poll re-render hid the device code');
    assert.equal(await page.locator('.device-code a').isVisible(), true,
      'the auth-poll re-render hid the verification link');

    /* --- signed in, then enabled ---------------------------------------- */
    fake.assistant.authenticated = true;
    fake.assistant.auth_state = 'signed_in';
    fake.assistant.user_code = '';
    fake.assistant.verification_url = '';
    const signedIn = Date.now() + 12000;
    while ((await statusOf(page, '#use-device-provider')) !== 'Disabled' && Date.now() < signedIn) {
      await page.waitForTimeout(250);
    }
    assert.equal(await statusOf(page, '#use-device-provider'), 'Disabled',
      'a signed-in, switched-off assistant must report "Disabled"');
    assert.equal(await page.locator('.device-code').count(), 0,
      'a completed sign-in must clear the device code');
    assert.equal(await page.locator('#use-device-provider').isDisabled(), false,
      'the switch must be usable once the account is signed in');
    assert.equal(await statusOf(page, '#use-live-provider'), 'Disabled');
    assert.equal(await page.locator('#use-live-provider').isDisabled(), false,
      'GPT-Live must be switchable once the shared account is signed in');

    await page.locator('#use-device-provider').check({ force: true });
    const enableDeadline = Date.now() + 5000;
    while (!enableCalls(fake).some(call => call.path === '/assistant') &&
           Date.now() < enableDeadline) {
      await page.waitForTimeout(50);
    }
    const deviceEnable = enableCalls(fake).find(call => call.path === '/assistant');
    assert.ok(deviceEnable, 'enabling the signed-in device assistant did not reach /assistant');
    assert.equal(deviceEnable.body.provider, 'openai-codex');
    assert.equal(await statusOf(page, '#use-device-provider'), 'Enabled');

    /* --- GPT-Live's own service is a separate axis ---------------------- */
    fake.liveSupported = false;
    await selectPage(page, 'Integrations');
    assert.equal(await statusOf(page, '#use-live-provider'), 'Unavailable',
      'a missing GPT-Live service must report "Unavailable"');
    assert.equal(await statusOf(page, '#use-device-provider'), 'Enabled',
      'a missing GPT-Live service must not change the device assistant state');
    assert.equal(await page.locator('#use-live-provider').isDisabled(), true,
      'the GPT-Live switch must be disabled while its service is unavailable');

    assert.deepEqual(failures, [], `browser failures:\n${failures.join('\n')}`);
    await context.close();
  } finally {
    await browser.close();
  }
  console.log('chatgpt account gate browser behaviour: ok');
})().catch(error => {
  console.error(error.stack || error);
  process.exit(1);
});
