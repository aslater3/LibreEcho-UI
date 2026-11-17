'use strict';

/*
 * Owner equaliser card in a real Chromium against the mock web backend: the
 * shipped page scripts, real select/slider events and the real API.
 *   - choosing a preset moves the four sliders and applies it on the device;
 *   - moving a slider (keyboard) switches the preset to Custom and applies
 *     after the debounce;
 *   - the speaker boost select applies;
 *   - the German shell shows the translated card;
 *   - no page errors.
 * Leaves the device flat with no boost.
 */

const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const baseURL = (process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083').replace(/\/+$/, '');
const failures = [];
function check(ok, message) {
  if (ok) console.log('  ok: ' + message);
  else { failures.push(message); console.error('  FAIL: ' + message); }
  return ok;
}
function checkEqual(actual, expected, message) {
  return check(JSON.stringify(actual) === JSON.stringify(expected),
    `${message} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}

async function launchBrowser() {
  const attempts = [['bundled', {}], ['channel-chrome', { channel: 'chrome' }],
    ['explicit-chrome', { executablePath: process.env.LIBREECHO_CHROME || '/usr/bin/google-chrome' }]];
  let lastError = null;
  for (const [name, options] of attempts) {
    try { const browser = await chromium.launch(options); console.log(`browser: chromium (${name})`); return browser; }
    catch (error) { lastError = error; }
  }
  throw new Error(`no Chromium could be launched: ${lastError && lastError.message}`);
}

async function waitForAudio(page, title) {
  await page.waitForFunction(expected => {
    const heading = document.querySelector('#page-title');
    return heading && heading.textContent.trim() === expected && document.querySelector('#eq-preset');
  }, title, { timeout: 10000 });
}
async function deviceEq(context) {
  const audio = await (await context.request.get(`${baseURL}/api/v1/audio`)).json();
  return { eq: audio.data.eq, boost_db: audio.data.boost_db };
}
async function sliders(page) {
  return page.evaluate(() => ['bass', 'low_mid', 'high_mid', 'treble'].map(id => Number(document.querySelector('#eq-' + id).value)));
}
async function waitForEq(context, predicate, message) {
  const deadline = Date.now() + 5000;
  let last = null;
  while (Date.now() < deadline) {
    last = await deviceEq(context);
    if (predicate(last)) return check(true, message);
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  return check(false, `${message} (device: ${JSON.stringify(last)})`);
}

async function caseEqualiser(browser) {
  const context = await browser.newContext();
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto(`${baseURL}/audio`);
  await waitForAudio(page, 'Audio');

  check(await page.locator('h3', { hasText: 'Equaliser' }).count() === 1, 'the Equaliser card is shown');
  const options = await page.$$eval('#eq-preset option', list => list.map(o => o.value));
  checkEqual(options, ['flat', 'bass_boost', 'treble_boost', 'vocal', 'loudness', 'late_night', 'warm', 'custom'], 'all presets plus Custom are offered');

  await page.selectOption('#eq-preset', 'vocal');
  checkEqual(await sliders(page), [-2, 1, 4, 1], 'Vocal moves the sliders');
  checkEqual(await page.textContent('#eq-high_mid-out'), '+4 dB', 'the shown dB follows the preset');
  await waitForEq(context, s => s.eq.preset === 'vocal' && s.eq.high_mid === 4, 'the device applies Vocal');

  /* Keyboard: focus the bass slider and step it up by 0.5 dB. */
  await page.focus('#eq-bass');
  await page.keyboard.press('ArrowRight');
  checkEqual(await page.inputValue('#eq-preset'), 'custom', 'moving a slider selects Custom');
  checkEqual(await page.textContent('#eq-bass-out'), '−1.5 dB', 'the slider moves in 0.5 dB steps');
  await waitForEq(context, s => s.eq.preset === 'custom' && s.eq.bass === -1.5 && s.eq.low_mid === 1 && s.eq.high_mid === 4 && s.eq.treble === 1,
    'the device applies the moved slider as Custom');

  await page.selectOption('#eq-boost', '6');
  await waitForEq(context, s => s.boost_db === 6, 'the speaker boost applies');
  await page.selectOption('#eq-boost', '0');
  await waitForEq(context, s => s.boost_db === 0, 'boost off applies');

  await page.selectOption('#eq-preset', 'flat');
  checkEqual(await sliders(page), [0, 0, 0, 0], 'Flat zeroes the sliders');
  await waitForEq(context, s => s.eq.preset === 'flat' && s.eq.bass === 0, 'the device is flat again');

  /* German shell. */
  await page.evaluate(() => localStorage.setItem('libreecho-lang', 'de'));
  await page.reload();
  await waitForAudio(page, 'Audio');
  check(await page.locator('h3', { hasText: 'Equalizer' }).count() === 1, 'German card title');
  check((await page.textContent('.eq-panel')).includes('Untere Mitten'), 'German band names');
  await page.evaluate(() => localStorage.removeItem('libreecho-lang'));

  check(errors.length === 0, `no page errors (${errors.join('; ')})`);
  await context.close();
}

(async () => {
  const browser = await launchBrowser();
  try {
    console.log('equaliser card');
    await caseEqualiser(browser);
  } finally {
    await browser.close();
  }
  assert.equal(failures.length, 0, `equaliser browser checks failed: ${failures.join(' | ')}`);
  console.log('equaliser e2e: ok');
})().catch(error => { console.error(error); process.exit(1); });
