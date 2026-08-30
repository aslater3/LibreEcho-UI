'use strict';

const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const baseURL = process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083';

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
  assert.equal(
    await page.locator('#content').getByText('Unable to load this section', { exact: false }).count(),
    0,
    `${name} rendered the generic error view`
  );
}

async function checkPwa(context, page) {
  assert.equal(await page.locator('link[rel="manifest"]').getAttribute('href'), '/manifest.webmanifest');
  assert.equal(await page.locator('meta[name="theme-color"]').getAttribute('content'), '#081622');
  assert.equal(await page.locator('link[rel="apple-touch-icon"]').getAttribute('href'), '/assets/apple-touch-icon.png');

  const manifestResponse = await context.request.get(`${baseURL}/manifest.webmanifest`);
  assert.equal(manifestResponse.status(), 200);
  assert.match(manifestResponse.headers()['content-type'] || '', /^application\/manifest\+json\b/i);

  const manifest = await manifestResponse.json();
  assert.equal(manifest.name, 'LibreEcho Control Centre');
  assert.equal(manifest.short_name, 'LibreEcho');
  assert.equal(manifest.start_url, '/');
  assert.equal(manifest.display, 'standalone');
  assert.ok(Array.isArray(manifest.icons) && manifest.icons.length >= 3, 'manifest should expose application icons');

  for (const icon of manifest.icons) {
    const response = await context.request.get(`${baseURL}${icon.src}`);
    assert.equal(response.status(), 200, `${icon.src} should be served`);
    assert.match(response.headers()['content-type'] || '', /^image\/png\b/i, `${icon.src} should be PNG`);
  }

  const appleIcon = await context.request.get(`${baseURL}/assets/apple-touch-icon.png`);
  assert.equal(appleIcon.status(), 200);
  assert.match(appleIcon.headers()['content-type'] || '', /^image\/png\b/i);
}

async function checkAudioMutation(page) {
  await selectPage(page, 'Audio');

  const slider = page.locator('#content input[type="range"]').first();
  await slider.waitFor({ state: 'visible', timeout: 5000 });
  const label = await slider.evaluate(element => element.closest('label')?.textContent || '');
  assert.match(label, /volume/i, 'first Audio slider should be the volume control');

  await slider.evaluate(element => {
    element.value = '63';
    element.dispatchEvent(new Event('input', { bubbles: true }));
    element.dispatchEvent(new Event('change', { bubbles: true }));
  });

  const save = page.locator('#content button.save-changes').first();
  await save.waitFor({ state: 'visible', timeout: 5000 });
  assert.equal(await save.isDisabled(), false, 'changing volume should enable Save changes');
  await save.click();

  await page.locator('#toast.show').waitFor({ state: 'visible', timeout: 5000 });
  await selectPage(page, 'Device');
  await selectPage(page, 'Audio');

  const persisted = page.locator('#content input[type="range"]').first();
  await persisted.waitFor({ state: 'visible', timeout: 5000 });
  assert.equal(await persisted.inputValue(), '63', 'saved volume should survive navigation and re-render');
}

/*
 * Simulation is off by default, so the sweep above never reaches it. Turn it on
 * and render the history table from a device-shaped row: agentd records timings
 * only -- no phrase, no wake result -- and a renderer that assumes the fields a
 * locally-run simulation leaves behind throws on the first device row, which
 * takes the whole page down.
 */
async function checkSimulationHistory(context, page) {
  const config = await context.request.get(`${baseURL}/api/v1/config`);
  const csrf = (await config.json()).data.csrf_token;
  const response = await context.request.put(`${baseURL}/api/v1/system/features`, {
    headers: { 'X-LibreEcho-CSRF': csrf },
    data: { simulation: true }
  });
  assert.ok(response.ok(), `enabling the simulation feature should succeed (${response.status()})`);

  await page.evaluate(() => {
    localStorage.removeItem('libreecho-simulation-history');
    localStorage.setItem('libreecho-simulation-device-history', JSON.stringify([
      { at: Date.now(), source: 'device', follow_up: false,
        audio_ms: 1870, processing_ms: 2024, queue_to_first_audio_ms: 2313 }
    ]));
  });
  await page.goto('/', { waitUntil: 'domcontentloaded' });
  await waitForPage(page, 'Overview');
  await selectPage(page, 'Simulation');

  const rows = await page.locator('#sim-history table.sim-table tbody tr').count();
  assert.equal(rows, 1, 'a cached device turn should render as one history row');
  const text = await page.locator('#sim-history').innerText();
  assert.match(text, /spoken turn/, 'a device row should say the phrase was not recorded');

  /* Leave the feature as it was found. It is persisted server-side, and the
     radio suite asserts the menu hides Simulation when it is off. */
  await context.request.put(`${baseURL}/api/v1/system/features`, {
    headers: { 'X-LibreEcho-CSRF': csrf },
    data: { simulation: false }
  });
  await page.evaluate(() => localStorage.removeItem('libreecho-simulation-device-history'));
}

async function desktopSuite(browser) {
  const context = await browser.newContext({ baseURL });
  const page = await context.newPage();
  const failures = captureBrowserFailures(page);

  const response = await page.goto('/', { waitUntil: 'domcontentloaded' });
  assert.ok(response && response.ok(), 'dashboard document should load successfully');
  await waitForPage(page, 'Overview');
  await page.waitForFunction(() => document.querySelector('#backend-badge')?.textContent.includes('mock'), null, { timeout: 7000 });
  assert.doesNotMatch(await page.locator('#device-online').innerText(), /Connecting/i);

  await checkPwa(context, page);

  const destinations = [
    'Device', 'Users', 'Audio', 'Baby Monitor', 'Wake Word', 'LED & Buttons',
    'Network', 'Bluetooth', 'Privacy', 'Integrations', 'System', 'Logs', 'About'
  ];
  for (const destination of destinations) await selectPage(page, destination);

  await checkSimulationHistory(context, page);

  await checkAudioMutation(page);
  assert.deepEqual(failures, [], `browser failures:\n${failures.join('\n')}`);

  await context.close();
}

async function mobileSuite(browser) {
  const context = await browser.newContext({
    baseURL,
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true
  });
  const page = await context.newPage();
  const failures = captureBrowserFailures(page);

  const response = await page.goto('/', { waitUntil: 'domcontentloaded' });
  assert.ok(response && response.ok(), 'mobile dashboard document should load successfully');
  await waitForPage(page, 'Overview');

  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  assert.ok(overflow <= 1, `mobile layout overflows viewport by ${overflow}px`);

  const menu = page.locator('#menu');
  await menu.waitFor({ state: 'visible', timeout: 5000 });
  await menu.click();
  await page.waitForFunction(() => {
    const sidebar = document.querySelector('.sidebar');
    if (!sidebar) return false;
    const box = sidebar.getBoundingClientRect();
    return box.right > 0 && box.left < window.innerWidth;
  }, null, { timeout: 3000 });

  await selectPage(page, 'Network');
  const networkOverflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  assert.ok(networkOverflow <= 1, `mobile Network page overflows viewport by ${networkOverflow}px`);
  assert.deepEqual(failures, [], `mobile browser failures:\n${failures.join('\n')}`);

  await context.close();
}

(async () => {
  const browser = await chromium.launch({ headless: true });
  try {
    await setupReadinessSuite(browser);
    await desktopSuite(browser);
    await mobileSuite(browser);
  } finally {
    await browser.close();
  }
})().catch(error => {
  console.error(error.stack || error);
  process.exit(1);
});
