import { test as base, expect } from '@playwright/test';
import fs from 'fs';
import path from 'path';

// The SPA reads its bearer token from sessionStorage (`libreecho-token`).
// Playwright's storageState persists cookies + localStorage but NOT sessionStorage,
// so we inject the token captured by auth.setup.ts before every page script runs.

const TOKEN_FILE = path.join(__dirname, '..', '.auth', 'token.json');

type Session = { token: string; username: string };

let session: Session | undefined;
function loadSession(): Session {
  if (!session) {
    if (!fs.existsSync(TOKEN_FILE)) {
      throw new Error('Missing .auth/token.json — the "setup" project must run first.');
    }
    session = JSON.parse(fs.readFileSync(TOKEN_FILE, 'utf8')) as Session;
  }
  return session;
}

export const test = base.extend({
  page: async ({ page }, use) => {
    const s = loadSession();
    await page.addInitScript((sess: Session) => {
      try {
        sessionStorage.setItem('libreecho-token', sess.token);
        sessionStorage.setItem('libreecho-username', sess.username);
      } catch {
        /* sessionStorage unavailable — nothing to do */
      }
    }, s);
    await use(page);
  },
});

export { expect };
