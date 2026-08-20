import { test, expect } from '../fixtures/auth';

// Hardware-actuating probes. These are safe and reversible (LED self-test, wake
// self-test) but they DO light the ring / trigger the listening animation, so
// they are OFF by default — the device's top was running hot and the LEDs were
// turned off. Enable explicitly with RUN_HW_TESTS=1.

const HW = process.env.RUN_HW_TESTS === '1';

test.describe('hardware self-tests (opt-in)', () => {
  test.skip(!HW, 'hardware probes disabled — set RUN_HW_TESTS=1 to enable (they light the LEDs)');

  test('Wake Word: self-test raises a toast', async ({ page }) => {
    await page.goto('/');
    await page.locator('#nav .nav-item[data-page="Wake Word"]').click();
    const btn = page.locator('#wake-test');
    if ((await btn.count()) === 0) {
      test.skip(true, 'wake adapter not present in this image');
    }
    await btn.click();
    await expect(page.locator('#toast')).toHaveClass(/show/);
  });

  test('LED & Buttons: LED self-test raises a toast', async ({ page }) => {
    await page.goto('/');
    await page.locator('#nav .nav-item[data-page="LED & Buttons"]').click();
    const btn = page.locator('#led-test');
    if ((await btn.count()) === 0) {
      test.skip(true, 'LED test control not present');
    }
    await btn.click();
    await expect(page.locator('#toast')).toHaveClass(/show/);
  });
});
