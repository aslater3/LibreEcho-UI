import { test as setup, expect } from '@playwright/test';
import fs from 'fs';
import path from 'path';

// Logs in ONCE via the device API and persists the session for all other tests.
// The device auth model is: GET /api/v1/config -> csrf_token, then
// POST /api/v1/auth/login {username,password} with the X-LibreEcho-CSRF header,
// which returns a bearer token the SPA keeps in sessionStorage.
//
// The device rate-limits repeated logins (HTTP 429), so this runs exactly once
// and retries gently (spaced) only to survive the flaky link, never to brute-force.

const AUTH_DIR = path.join(__dirname, '..', '.auth');
const TOKEN_FILE = path.join(AUTH_DIR, 'token.json');
const STORAGE_FILE = path.join(AUTH_DIR, 'storageState.json');

const BASE = process.env.BASE_URL || 'http://localhost:8080';
const USER = process.env.LE_USER || 'lpick';
const PASS = process.env.LE_PASS;

setup('authenticate', async ({ request }) => {
  setup.setTimeout(120_000);
  if (!PASS) {
    throw new Error(
      'LE_PASS env var is required (device login password for user "' + USER + '"). ' +
      'Example: LE_PASS=... npx playwright test',
    );
  }
  fs.mkdirSync(AUTH_DIR, { recursive: true, mode: 0o700 });
  fs.chmodSync(AUTH_DIR, 0o700);

  // Reuse a still-valid session if one was captured recently. The device
  // rate-limits logins, so we avoid a fresh login on every run: probe an
  // authenticated endpoint with the saved token and skip login if it works.
  if (fs.existsSync(TOKEN_FILE) && fs.existsSync(STORAGE_FILE)) {
    try {
      const saved = JSON.parse(fs.readFileSync(TOKEN_FILE, 'utf8'));
      const probe = await request.get(`${BASE}/api/v1/status`, {
        headers: { Authorization: `Bearer ${saved.token}` },
        timeout: 10_000,
      });
      if (probe.ok()) {
        fs.chmodSync(TOKEN_FILE, 0o600);
        fs.chmodSync(STORAGE_FILE, 0o600);
        return; // existing session still valid
      }
    } catch {
      /* fall through to a fresh login */
    }
  }

  let token: string | undefined;
  let username: string | undefined;
  let lastErr: unknown;

  for (let attempt = 1; attempt <= 4 && !token; attempt++) {
    try {
      const cfgRes = await request.get(`${BASE}/api/v1/config`, { timeout: 15_000 });
      if (!cfgRes.ok()) throw new Error(`GET /config -> HTTP ${cfgRes.status()}`);
      const csrf = (await cfgRes.json())?.data?.csrf_token;
      if (!csrf) throw new Error('no csrf_token in /config response');

      const loginRes = await request.post(`${BASE}/api/v1/auth/login`, {
        headers: { 'Content-Type': 'application/json', 'X-LibreEcho-CSRF': csrf },
        data: { username: USER, password: PASS },
        timeout: 15_000,
      });
      const body = await loginRes.json();
      if (loginRes.status() === 429) {
        throw new Error('rate-limited (429) — wait a minute and re-run');
      }
      if (!loginRes.ok() || !body?.ok) {
        throw new Error(`login failed: ${JSON.stringify(body?.error ?? body)}`);
      }
      token = body.data.token;
      username = body.data.username;
    } catch (e) {
      lastErr = e;
      if (attempt < 4) await new Promise((r) => setTimeout(r, 4_000 * attempt));
    }
  }

  expect(token, `could not authenticate after retries: ${lastErr}`).toBeTruthy();

  fs.writeFileSync(TOKEN_FILE, JSON.stringify({ token, username }), { mode: 0o600 });
  fs.chmodSync(TOKEN_FILE, 0o600);
  // Persist cookies from the login so the browser context is authenticated too.
  await request.storageState({ path: STORAGE_FILE });
  fs.chmodSync(STORAGE_FILE, 0o600);
});
