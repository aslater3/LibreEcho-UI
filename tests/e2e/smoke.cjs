'use strict';

const assert = require('node:assert/strict');
const { chromium } = require('playwright');

const baseURL = process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083';

function captureBrowserFailures(page) {
  const failures = [];
  page.on('pageerror', error => failures.push(`pageerror: ${error.message}`));
  page.on('console', message => {
    if (message.type() === 'error') failures.push(`console.error: ${message.text()}`);
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
    await desktopSuite(browser);
    await mobileSuite(browser);
  } finally {
    await browser.close();
  }
})().catch(error => {
  console.error(error.stack || error);
  process.exit(1);
});
