import { test, expect } from '../fixtures/auth';

// Regression for #47: the UI must not trigger CSP inline-style violations.
test('no CSP inline-style violations on Overview + LED pages', async ({ page }) => {
  const violations: string[] = [];
  page.on('console', (m) => {
    if (/Content Security Policy|violates the following/i.test(m.text())) violations.push(m.text());
  });
  await page.goto('/');
  await page.waitForTimeout(2600); // let Overview render + one refresh tick
  await page.locator('#nav .nav-item[data-page="LED & Buttons"]').click().catch(() => {});
  await page.waitForTimeout(1500);
  expect(violations, `CSP violations:\n${violations.join('\n')}`).toEqual([]);
});
