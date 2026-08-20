import { defineConfig, devices } from '@playwright/test';

// Target device base URL. Override per run:
//   BASE_URL=http://<device-ip>:8080 npx playwright test
const BASE_URL = process.env.BASE_URL || 'http://localhost:8080';

export default defineConfig({
  testDir: './tests',
  // The device is a single, sometimes-flaky target: never parallelize against it,
  // and retry to ride out transient link drops.
  fullyParallel: false,
  workers: 1,
  retries: 2,
  timeout: 60_000,
  expect: { timeout: 15_000 },
  reporter: [['list'], ['html', { open: 'never' }]],
  use: {
    baseURL: BASE_URL,
    actionTimeout: 15_000,
    navigationTimeout: 30_000,
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
    ignoreHTTPSErrors: true,
  },
  projects: [
    // One login for the whole run — the device rate-limits auth, so we must not
    // log in per test. This project captures the session; everything depends on it.
    { name: 'setup', testMatch: /auth\.setup\.ts/ },
    {
      name: 'chromium',
      use: {
        ...devices['Desktop Chrome'],
        // Tall viewport so all 14 nav items are on-screen (the sidebar doesn't
        // scroll independently; the last items fall below a 720px fold).
        viewport: { width: 1440, height: 1600 },
        // Cookies from the single login; the sessionStorage bearer token is
        // injected per page by the auth fixture (storageState can't carry it).
        storageState: '.auth/storageState.json',
      },
      dependencies: ['setup'],
      testIgnore: /auth\.setup\.ts/,
    },
  ],
});
