import { test, expect } from '../fixtures/auth';
import type { Page } from '@playwright/test';

// The 14 control-centre pages, in nav order. Each nav item carries data-page="<Name>"
// and, when active, sets document.title to "<Name> · LibreEcho".
const PAGES = [
  'Overview',
  'Device',
  'Users',
  'Audio',
  'Baby Monitor',
  'Wake Word',
  'LED & Buttons',
  'Network',
  'Bluetooth',
  'Privacy',
  'Integrations',
  'System',
  'Logs',
  'About',
] as const;

function trackErrors(page: Page): string[] {
  const errors: string[] = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  return errors;
}

test.describe('UI navigation (read-only)', () => {
  test('authenticated load shows the app, not the login page', async ({ page }) => {
    const errors = trackErrors(page);
    await page.goto('/');
    await expect(page).not.toHaveURL(/\/login\b/);
    await expect(page.locator('#nav .nav-item')).toHaveCount(PAGES.length);
    expect(errors, `uncaught JS errors: ${errors.join(' | ')}`).toEqual([]);
  });

  for (const name of PAGES) {
    test(`page renders: ${name}`, async ({ page }) => {
      const errors = trackErrors(page);
      await page.goto('/');

      await page.locator(`#nav .nav-item[data-page="${name}"]`).click();

      // Title + header reflect the selected page (the SPA sets both).
      await expect(page).toHaveTitle(`${name} · LibreEcho`);
      await expect(page.locator('#page-title')).toHaveText(name);

      const content = page.locator('#content');
      // Finished loading (spinner gone) and did not fall into the generic error view.
      await expect(content).not.toContainText('Loading device state');
      await expect(content).not.toContainText('Unable to load this section');
      // Something actually rendered.
      await expect(content.locator('.panel').first()).toBeVisible();

      expect(errors, `uncaught JS errors on "${name}": ${errors.join(' | ')}`).toEqual([]);
    });
  }
});
