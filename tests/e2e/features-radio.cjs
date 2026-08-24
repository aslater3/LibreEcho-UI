'use strict';
/*
 * Browser verification for the System > Features toggle, the feature-gated
 * Simulation nav entry, and the Integrations > Internet radio panel.
 *
 * The preview mock backend does not serve /system/features or
 * /integrations/radio, so the first pass checks the graceful-degradation path
 * against the real 404/405 and the rest supplies responses with page.route().
 */
const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const baseURL = process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083';
const checks = [];
function pass(name) { checks.push(name); console.log('  ok  ' + name); }

function captureBrowserFailures(page) {
  const failures = [];
  page.on('pageerror', e => failures.push(`pageerror: ${e.message}`));
  page.on('console', m => {
    if (m.type() !== 'error') return;
    const text = m.text();
    if (/^Failed to load resource: the server responded with a status of \d+/.test(text)) return;
    failures.push(`console.error: ${text}`);
  });
  return failures;
}

async function waitForPage(page, title) {
  await page.waitForFunction(expected => {
    const h = document.querySelector('#page-title'), c = document.querySelector('#content');
    return h && h.textContent.trim() === expected && c && c.textContent.trim().length > 0 && !c.querySelector('.loading');
  }, title, { timeout: 8000 });
}

// The real checkbox is visually hidden (.toggle-input is opacity:0 /
// pointer-events:none); the label is what a person clicks.
async function flip(locator) {
  await locator.evaluate(el => el.closest('label').click());
}

async function navNames(page) {
  return page.$$eval('#nav .nav-item', els => els.map(e => e.textContent.trim()));
}

/* The radio panel is a <details>; everything the assertions below look at
   lives inside the disclosure, and a re-render closes it again. */
async function openRadioPanel(page) {
  const panel = page.locator('.radio-stations');
  await panel.waitFor({ timeout: 5000 });
  if (!(await panel.evaluate(el => el.open))) await panel.locator('summary').click();
  return panel;
}

async function main() {
  const browser = await chromium.launch();
  const context = await browser.newContext({ baseURL });
  const page = await context.newPage();
  const failures = captureBrowserFailures(page);

  /*
   * Pass 1 covers the older-image case: an interface newer than the firmware
   * it is talking to. It used to rely on the endpoints genuinely not existing,
   * which stopped being true once the device grew them -- the assertions then
   * described nothing and the suite failed. Refuse them explicitly instead, so
   * the case survives the endpoints being implemented.
   */
  await page.route('**/api/v1/system/features', route =>
    route.fulfill({ status: 404, contentType: 'application/json',
      body: JSON.stringify({ ok: false, data: null,
        error: { code: 'not_found', message: 'Not supported' } }) }));
  await page.route('**/api/v1/integrations/radio', route =>
    route.fulfill({ status: 404, contentType: 'application/json',
      body: JSON.stringify({ ok: false, data: null,
        error: { code: 'not_found', message: 'Not supported' } }) }));

  await page.goto('/');
  await waitForPage(page, 'Overview');
  assert.ok(!(await navNames(page)).includes('Simulation'),
    'Simulation must be hidden when the feature endpoint is unavailable');
  pass('endpoint unavailable: Simulation nav entry hidden, page still loads');

  await page.locator('#nav').getByText('System', { exact: true }).click();
  await waitForPage(page, 'System');
  const featuresPanel = page.locator('.setting-panel', { has: page.getByRole('heading', { name: 'Features' }) });
  await featuresPanel.waitFor({ timeout: 5000 });
  assert.match(await featuresPanel.innerText(), /Not supported/);
  assert.equal(await page.locator('#feature-simulation').count(), 0);
  pass('endpoint unavailable: Features panel explains rather than throwing');

  await page.locator('#nav').getByText('Integrations', { exact: true }).click();
  await waitForPage(page, 'Integrations');
  const radio = await openRadioPanel(page);
  assert.match(await radio.innerText(), /Not supported/);
  pass('endpoint unavailable: radio panel explains rather than throwing');

  await page.goto('/simulation');
  await waitForPage(page, 'Overview');
  assert.equal(new URL(page.url()).pathname, '/overview', 'the unreachable route is replaced');
  pass('direct /simulation route falls back to Overview and rewrites the URL');

  await page.unroute('**/api/v1/system/features');
  await page.unroute('**/api/v1/integrations/radio');

  // ---- Pass 2: supply the two endpoints -----------------------------------
  const backend = { simulation: false, radio: { max_stations: 32, playback_supported: false, stations: [] } };
  const puts = { features: [], radio: [] };
  let radioPutStatus = 200, radioPutError = null;

  await page.route('**/api/v1/system/features', async route => {
    const req = route.request();
    if (req.method() === 'PUT') {
      const body = JSON.parse(req.postData());
      puts.features.push(body);
      backend.simulation = !!body.simulation;
    }
    await route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, data: { simulation: backend.simulation }, error: null }) });
  });
  await page.route('**/api/v1/integrations/radio', async route => {
    const req = route.request();
    if (req.method() === 'PUT') {
      const body = JSON.parse(req.postData());
      puts.radio.push(body);
      if (radioPutStatus !== 200) {
        await route.fulfill({ status: radioPutStatus, contentType: 'application/json',
          body: JSON.stringify({ ok: false, data: null, error: { code: 'invalid_request', message: radioPutError } }) });
        return;
      }
      backend.radio.stations = [];
      for (let i = 0; i < body.station_count; i++) {
        backend.radio.stations.push({ word: body['station_' + i + '_word'], name: body['station_' + i + '_name'] || body['station_' + i + '_word'], url: body['station_' + i + '_url'], enabled: body['station_' + i + '_enabled'] });
      }
    }
    await route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, data: backend.radio, error: null }) });
  });

  await page.goto('/system');
  await waitForPage(page, 'System');
  assert.ok(!(await navNames(page)).includes('Simulation'), 'off by default keeps the entry hidden');
  assert.equal(await page.locator('#feature-simulation').isChecked(), false);
  assert.equal(await page.locator('#save-features').isDisabled(), true);
  pass('features off: toggle unchecked, save disabled, no Simulation entry');

  await flip(page.locator('#feature-simulation'));
  assert.equal(await page.locator('#save-features').isDisabled(), false);
  pass('features: save enables when dirty');

  await page.locator('#save-features').click();
  await page.waitForFunction(() => [...document.querySelectorAll('#nav .nav-item')].some(e => e.textContent.trim() === 'Simulation'), null, { timeout: 8000 });
  /* The panel saves every feature it shows, so HTTPS rides along unchanged. */
  assert.deepEqual(puts.features, [{ simulation: true, https: false }]);
  pass('features on: PUT sent, Simulation entry appears without a reload');

  await page.locator('#nav').getByText('Simulation', { exact: true }).click();
  await waitForPage(page, 'Simulation');
  pass('Simulation page reachable while the feature is on');

  await page.goto('/simulation');
  await waitForPage(page, 'Simulation');
  assert.equal(new URL(page.url()).pathname, '/simulation');
  pass('direct /simulation route works while the feature is on');

  await page.goto('/system');
  await waitForPage(page, 'System');
  assert.equal(await page.locator('#feature-simulation').isChecked(), true);
  await flip(page.locator('#feature-simulation'));
  await page.locator('#save-features').click();
  await page.waitForFunction(() => ![...document.querySelectorAll('#nav .nav-item')].some(e => e.textContent.trim() === 'Simulation'), null, { timeout: 8000 });
  assert.deepEqual(puts.features[1], { simulation: false, https: false });
  pass('features off again: Simulation entry disappears without a reload');

  // ---- Radio panel --------------------------------------------------------
  await page.goto('/integrations');
  await waitForPage(page, 'Integrations');
  const panelText = await (await openRadioPanel(page)).innerText();
  assert.match(panelText, /cannot be played yet/);
  assert.match(panelText, /Example station/);
  assert.equal(await page.locator('.radio-row').count(), 1);
  assert.equal(await page.locator('.radio-row .radio-word').first().inputValue(), 'groove');
  assert.equal(await page.locator('.radio-row .radio-name').first().inputValue(), 'Groove Salad');
  assert.equal(await page.locator('.radio-row .radio-url').first().inputValue(), 'http://ice1.somafm.com/groovesalad-128-mp3');
  assert.equal(await page.locator('#radio-count').innerText(), '1 of 32 stations');
  assert.equal(await page.locator('#save-radio').isDisabled(), false, 'the unsaved example counts as dirty');
  pass('radio: empty list seeds the Groove Salad example and says playback is unavailable');

  await page.locator('#radio-add').click();
  assert.equal(await page.locator('.radio-row').count(), 2);
  assert.equal(await page.locator('#radio-count').innerText(), '2 of 32 stations');
  assert.match(await page.locator('.radio-row').nth(1).locator('.radio-row-error').innerText(), /needs a word/);
  pass('radio: add row, count updates, empty row reports a missing word');

  await page.locator('.radio-row').nth(1).locator('.radio-word').fill('News Radio!');
  assert.match(await page.locator('.radio-row').nth(1).locator('.radio-row-error').innerText(), /lowercase letters, digits, spaces and hyphens/);
  await page.locator('.radio-row').nth(1).locator('.radio-word').fill('groove');
  assert.match(await page.locator('.radio-row').nth(1).locator('.radio-row-error').innerText(), /Another station already uses/);
  await page.locator('.radio-row').nth(1).locator('.radio-word').fill('news');
  await page.locator('.radio-row').nth(1).locator('.radio-url').fill('ftp://example.invalid/stream');
  assert.match(await page.locator('.radio-row').nth(1).locator('.radio-row-error').innerText(), /must start with http:\/\/ or https:\/\//);
  pass('radio: word charset, duplicate word and URL scheme all report before saving');

  // an invalid row must not be sent at all
  await page.locator('#save-radio').click();
  await page.waitForSelector('#toast.show', { timeout: 4000 });
  assert.match(await page.locator('#toast').innerText(), /must start with http:\/\/ or https:\/\//);
  assert.equal(puts.radio.length, 0, 'nothing is PUT while a row is invalid');
  pass('radio: save with an invalid row toasts the rule and sends nothing');

  await page.locator('.radio-row').nth(1).locator('.radio-url').fill('http://example.invalid/news.mp3');
  await flip(page.locator('.radio-row').nth(1).locator('.radio-enabled'));  // -> disabled
  assert.equal(await page.locator('.radio-row .radio-row-error:visible').count(), 0);

  // server rejects: its message, not a generic failure
  radioPutStatus = 400; radioPutError = 'Each station needs a unique word of lowercase letters, digits, spaces or hyphens, and an http:// or https:// URL';
  await page.locator('#save-radio').click();
  await page.waitForFunction(() => document.querySelector('#toast').textContent.includes('unique word'), null, { timeout: 5000 });
  assert.equal(puts.radio.length, 1);
  assert.deepEqual(puts.radio[0], {
    station_count: 2,
    station_0_word: 'groove', station_0_name: 'Groove Salad',
    station_0_url: 'http://ice1.somafm.com/groovesalad-128-mp3', station_0_enabled: true,
    station_1_word: 'news', station_1_name: '',
    station_1_url: 'http://example.invalid/news.mp3', station_1_enabled: false
  });
  pass('radio: PUT body is flat and numbered; a 400 shows the server message');
  assert.equal(await page.locator('.radio-row').count(), 2, 'a rejected save keeps the edits on screen');
  assert.equal(await page.locator('#save-radio').isDisabled(), false, 'save stays usable after a rejected save');
  pass('radio: rejected save keeps the rows and re-enables the button');

  radioPutStatus = 200;
  await page.locator('#save-radio').click();
  await page.waitForFunction(() => document.querySelector('#toast').textContent.includes('2 stations saved'), null, { timeout: 5000 });
  await page.locator('.radio-stations').waitFor({ timeout: 5000 });
  await page.waitForFunction(() => document.querySelectorAll('.radio-row').length === 2, null, { timeout: 5000 });
  const savedText = await page.locator('.radio-stations').innerText();
  assert.doesNotMatch(savedText, /Example station/, 'the example notice goes away once stations exist');
  assert.equal(await page.locator('#save-radio').isDisabled(), true, 'a freshly loaded list is not dirty');
  assert.equal(await page.locator('.radio-row').nth(1).locator('.radio-name').inputValue(), 'news', 'a blank name falls back to the word');
  assert.equal(await page.locator('.radio-row').nth(1).locator('.radio-enabled').isChecked(), false);
  pass('radio: successful save reloads the server list, clean and not dirty');

  await openRadioPanel(page);   /* the reload re-rendered the panel, closing it */
  await page.locator('.radio-row').first().locator('.radio-name').fill('Groove Salad HD');
  assert.equal(await page.locator('#save-radio').isDisabled(), false);
  await page.locator('.radio-row').nth(1).locator('.radio-remove').click();
  assert.equal(await page.locator('.radio-row').count(), 1);
  assert.equal(await page.locator('#radio-count').innerText(), '1 of 32 stations');
  pass('radio: editing marks dirty, remove drops the row and updates the count');

  // the radio panel survives an unrelated re-render of the Integrations page
  await page.locator('#save-radio').click();
  await page.waitForFunction(() => document.querySelector('#toast').textContent.includes('1 station saved'), null, { timeout: 5000 });
  await page.waitForFunction(() => document.querySelectorAll('.radio-row').length === 1, null, { timeout: 5000 });
  assert.equal(await page.locator('.radio-stations').count(), 1, 'exactly one radio panel after a re-render');
  pass('radio: panel is present exactly once after integrationsPage re-renders');


  // ---- Device hardware / audio capability card ---------------------------
  const device = {
    name: 'Kitchen LibreEcho', hostname: 'libreecho-dev',
    model: 'Amazon Echo (2nd generation)', serial: 'DEV-MOCK-4C454348',
    os_version: 'LibreEcho OS 0.13.7', kernel: '6.1.178+',
    hardware_revision: 'MediaTek MT8163', backend: 'mock',
    audio: {
      capture: { rate_hz: 16000, raw_channels: 9, microphones: 7, transport_channels: 2,
        format: 'pcm_s24_3le', valid_bits: 16,
        beamforming: 'measured delay-and-sum on logical mics 0 and 3',
        high_pass_hz: 80, digital_gain: '4.0x',
        response: 'flat within 0.5 dB, 200 Hz to 7 kHz',
        noise_floor_dbfs: -65.3, thd_n_percent_max: 0.2, clipping_from_input_amplitude: 16000 },
      output: { rate_hz: 48000, channels: 2, format: 'pcm_s16_le',
        mixer_volume_range: '0-175, unity at 127',
        buses: ['media', 'system', 'announcement', 'alarm'] },
      streaming: { decoders: [], available: ['airplay2', 'bluetooth-a2dp'],
        note: 'no compressed-audio decoder on this image' }
    }
  };
  let deviceBody = device;
  await page.route('**/api/v1/device', r => r.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ ok: true, data: deviceBody, error: null }) }));

  await page.goto('/device');
  await waitForPage(page, 'Device');
  const hw = page.locator('.hardware-card');
  await hw.waitFor({ timeout: 5000 });
  const hwText = await hw.innerText();
  for (const expected of ['MediaTek MT8163', '6.1.178+', 'DEV-MOCK-4C454348', 'Amazon Echo (2nd generation)',
                          '16 kHz', '9 raw channels', '7 microphones', '2 transport',
                          'pcm_s24_3le', '16 valid bits', '80 Hz', '4.0x', '-65.3 dBFS', 'up to 0.2 %',
                          '48 kHz', 'pcm_s16_le', '0-175, unity at 127', 'media, system, announcement, alarm',
                          'AirPlay 2, Bluetooth A2DP', 'None on this image']) {
    assert.ok(hwText.includes(expected), `hardware card is missing ${JSON.stringify(expected)}`);
  }
  assert.match(hwText, /no compressed-audio decoder, so the device cannot play a stream URL by itself/);
  assert.match(hwText, /already decoded, over AirPlay 2 and Bluetooth A2DP/);
  assert.doesNotMatch(hwText, /undefined|\[object/);
  pass('device: hardware and audio capability card renders every reported field');

  // an older daemon with no audio object at all, and a partial one
  deviceBody = { name: 'Kitchen LibreEcho', hostname: 'libreecho-dev', model: 'Amazon Echo (2nd generation)',
    serial: 'DEV-MOCK', os_version: 'LibreEcho OS', kernel: '6.1.178+', hardware_revision: 'MediaTek MT8163', backend: 'mock' };
  await page.goto('/device');
  await waitForPage(page, 'Device');
  assert.match(await page.locator('.hardware-card').innerText(), /does not report audio capability/);
  assert.doesNotMatch(await page.locator('.hardware-card').innerText(), /undefined/);
  pass('device: no audio object degrades to a plain explanation');

  deviceBody = { name: 'x', hostname: 'y', model: 'Amazon Echo (2nd generation)', serial: 'S', os_version: 'o',
    kernel: '6.1.178+', hardware_revision: 'MediaTek MT8163', backend: 'mock',
    audio: { output: { rate_hz: 48000, buses: ['media'] } } };
  await page.goto('/device');
  await waitForPage(page, 'Device');
  const partial = await page.locator('.hardware-card').innerText();
  assert.match(partial, /48 kHz/);
  assert.doesNotMatch(partial, /Capture/);
  assert.doesNotMatch(partial, /undefined|NaN/);
  pass('device: a partial audio object renders only the fields that are present');
  await page.unroute('**/api/v1/device');

  // ---- Home location card -------------------------------------------------
  await page.route('**/api/v1/assistant', async route => {
    if (route.request().method() !== 'GET') return route.fallback();
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true, data: {
      enabled: false, provider: 'openai-codex', provider_name: 'ChatGPT', model: 'gpt-5.4', prompt: 'Reply briefly.',
      authenticated: false, auth_state: 'idle', weather_provider: 'open-meteo',
      home_location: 'Austin, Texas', latitude: '30.2672', longitude: '-97.7431' }, error: null }) });
  });
  await page.goto('/integrations');
  await waitForPage(page, 'Integrations');
  const home = page.locator('.weather-provider');
  await home.waitFor({ timeout: 5000 });
  assert.equal(await home.count(), 1, 'exactly one home-location card');
  assert.match(await home.locator('summary h3').innerText(), /Home location & weather/);
  /* Collapsed by default since "stop expanding panels": Home location, Local
     LLM, the voice assistant and Internet radio all opened themselves on load
     and the page arrived half-expanded. */
  assert.equal(await home.evaluate(el => el.open), false, 'the card starts collapsed');
  await home.locator('summary').click();
  assert.match(await home.innerText(), /Home address or place/);
  assert.match(await home.innerText(), /weather, local time and, in future, directions/);
  assert.equal(await page.locator('#wx-location').inputValue(), 'Austin, Texas');
  assert.equal(await page.locator('#wx-lat').inputValue(), '30.2672');
  assert.equal(await page.locator('#wx-lon').inputValue(), '-97.7431');
  assert.equal(await page.locator('#save-wx').isDisabled(), true);
  await page.locator('#wx-location').fill('Round Rock, Texas');
  assert.equal(await page.locator('#save-wx').isDisabled(), false);
  assert.deepEqual(
    await page.$$eval('.integration-grid > *', els => els.map(e => (e.querySelector('h3') || {}).textContent)),
    ['Voice Assistants', 'Home location & weather', 'Home Assistant', 'MQTT', 'Local REST API',
     'Bluetooth audio', 'AirPlay 2', 'Internet radio'],
    'the home location card sits directly under Voice Assistants, not buried at the bottom'
  );
  pass('home location: retitled, expanded, labelled as an address, prefilled and dirty-tracked');


  // ---- Simulation page: the response-time goal -----------------------------
  // Deliberately mixed: one run under the 1 s goal on stt+llm, the rest over,
  // so both markings are exercised and the median is not the same as the best.
  const simRuns = [
    { at: '2026-08-22T20:09:00.000Z', text: 'Alexa, what time is it?', wake: true, wake_latency_ms: 1200,
      max_utterance_ms: 6000, audio_ms: 5000, processing_ms: 2800, queue_to_transcript_ms: 8000,
      transcript_to_audio_ms: 1000, queue_to_first_audio_ms: 9000 },
    { at: '2026-08-22T20:08:00.000Z', text: 'Alexa, what is the weather?', wake: true, wake_latency_ms: 1240,
      max_utterance_ms: 6000, audio_ms: 5000, processing_ms: 3100, queue_to_transcript_ms: 8000,
      transcript_to_audio_ms: 1000, queue_to_first_audio_ms: 7000 },
    { at: '2026-08-22T20:07:00.000Z', text: 'Alexa, stop', wake: true, wake_latency_ms: 900,
      max_utterance_ms: 6000, audio_ms: 2000, processing_ms: 800, queue_to_transcript_ms: 2000,
      transcript_to_audio_ms: 1000, queue_to_first_audio_ms: 3000 }
  ];
  backend.simulation = true;
  await page.goto('/');
  await page.evaluate(h => localStorage.setItem('libreecho-simulation-history', JSON.stringify(h)), simRuns);
  await page.goto('/simulation');
  await waitForPage(page, 'Simulation');
  await page.waitForSelector('#sim-history table', { timeout: 8000 });

  const lastRun = await page.locator('.setting-panel', { has: page.getByRole('heading', { name: 'Last run' }) }).innerText();
  assert.match(lastRun, /Goal: under 1 s from the end of speech to the first audio out of the speaker/);
  assert.match(lastRun, /does not report end-of-speech to first audio, so it is not shown/);
  assert.match(lastRun, /stamped in whole seconds/);
  assert.match(lastRun, /upper bound/);
  assert.match(lastRun, /closest honest proxy/);
  pass('simulation: the goal is stated and the recorded value is labelled honestly');

  // stt+llm: 800 under goal (connected), 2800 and 3100 over (error-text)
  const sttCells = page.locator('.sim-table tbody tr td:nth-child(6)');
  assert.deepEqual(await sttCells.allInnerTexts(), ['2.80 s', '3.10 s', '800 ms']);
  assert.deepEqual(await sttCells.evaluateAll(els => els.map(e => e.className)),
    ['error-text', 'error-text', 'connected']);
  // request -> first audio: all three over the goal
  const firstAudioCells = page.locator('.sim-table tbody tr td:nth-child(7)');
  assert.deepEqual(await firstAudioCells.evaluateAll(els => els.map(e => e.className)),
    ['error-text', 'error-text', 'error-text']);
  assert.equal(await page.locator('.sim-table thead th').nth(6).innerText(), 'REQUEST → FIRST AUDIO');
  pass('simulation: both goal columns are marked with connected / error-text, no new colours');

  const summary = page.locator('.sim-summary');
  await summary.waitFor({ timeout: 5000 });
  const summaryText = await summary.innerText();
  // stt+llm: best 800 ms, median of [800, 2800, 3100] = 2800
  assert.match(summaryText, /STT \+ model\s+best 800 ms · median 2\.80 s over 3 runs/);
  // request -> first audio: best 3.00 s, median of [3000, 7000, 9000] = 7000
  assert.match(summaryText, /Request → first audio\s+best 3\.00 s · median 7\.00 s over 3 runs/);
  assert.equal(await summary.locator('span.connected').count(), 1, 'only the sub-second best is green');
  assert.match(await page.locator('#sim-history').innerText(), /largest cost today is speech-to-text plus the model/);
  pass('simulation: best and median summary over the stored runs, marked against the goal');

  await page.evaluate(() => localStorage.removeItem('libreecho-simulation-history'));
  if (failures.length) {
    console.error('browser failures:\n' + failures.join('\n'));
    process.exitCode = 1;
  } else {
    console.log(`\n${checks.length} checks passed, 0 console errors / page errors`);
  }
  await browser.close();
}

main().catch(e => { console.error(e); process.exit(1); });
