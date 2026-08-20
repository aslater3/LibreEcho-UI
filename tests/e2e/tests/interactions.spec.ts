import { test, expect } from '../fixtures/auth';

// Read-only interactions: exercise UI affordances that do NOT mutate device
// state or actuate hardware — expanding collapsible panels and asserting content.
//
// Hard-excluded everywhere (never targeted by any test): Save buttons on settings
// forms, and reboot / factory-reset / shutdown / network-apply controls.

test.describe('read-only interactions', () => {
  test('Integrations: assistant provider panels expand', async ({ page }) => {
    await page.goto('/');
    await page.locator('#nav .nav-item[data-page="Integrations"]').click();
    await expect(page.locator('#page-title')).toHaveText('Integrations');

    // The Assistant card renders collapsible <details> panels (Local LLM, ChatGPT).
    const panels = page.locator('#content details.assistant-provider, #content details.integration-section');
    const count = await panels.count();
    expect(count, 'expected at least one collapsible panel on Integrations').toBeGreaterThan(0);

    const first = panels.first();
    await first.locator('summary').click();
    await expect(first).toHaveAttribute('open', '');
  });

  test('Logs page shows entries without a stream error', async ({ page }) => {
    await page.goto('/');
    await page.locator('#nav .nav-item[data-page="Logs"]').click();
    await expect(page.locator('#page-title')).toHaveText('Logs');
    await expect(page.locator('#content')).not.toContainText('Unable to load this section');
  });
});
