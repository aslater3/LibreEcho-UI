/* Appearance (#188): web/js/theme.js applies the device's saved theme over the
   per-browser hint, never throws when the device cannot be reached, and leaves
   the classic look untouched by default. The real file runs under a tiny DOM. */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const source = fs.readFileSync(path.join(__dirname, '..', 'web', 'js', 'theme.js'), 'utf8');
let failures = 0;
function check(ok, message) { if (ok) console.log('  ok: ' + message); else { failures++; console.error('  FAIL: ' + message); } }

function run(store, configBody, fetchFails) {
    const attrs = {};
    const classes = new Set();
    const meta = { content: '#081622', setAttribute(k, v) { this[k] = v; } };
    const button = { textContent: '☼' };
    const listeners = [];
    const ctx = {
        localStorage: { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } },
        document: {
            documentElement: { setAttribute: (k, v) => { attrs[k] = v; }, removeAttribute: k => { delete attrs[k]; } },
            body: { classList: { toggle: (c, on) => { on ? classes.add(c) : classes.delete(c); } } },
            querySelector: s => (s === 'meta[name="theme-color"]' ? meta : null),
            getElementById: id => (id === 'theme' ? button : null),
            addEventListener: (e, f) => listeners.push(f)
        },
        window: { getComputedStyle: () => ({ getPropertyValue: () => (attrs['data-ui-theme'] === 'setup' ? ' #090c12' : ' #081622') }) },
        fetch: () => (fetchFails ? Promise.reject(new Error('offline')) : Promise.resolve({ json: async () => configBody }))
    };
    ctx.window.fetch = ctx.fetch;
    vm.createContext(ctx);
    vm.runInContext(source, ctx, { filename: 'theme.js' });
    return new Promise(resolve => setTimeout(() => resolve({ attrs, classes, meta, button, store, ctx }), 20));
}

(async () => {
    let r = await run({}, { ok: true, data: { ui_theme: 'classic', color_mode: '' } });
    check(!('data-ui-theme' in r.attrs), 'classic by default: no theme attribute');
    check(!r.classes.has('light'), 'an unset device colour mode leaves the browser choice alone');
    check(!('libreecho-theme' in r.store), 'an unset device colour mode does not overwrite the browser hint');

    r = await run({ 'libreecho-ui-theme': 'classic', 'libreecho-theme': 'dark' }, { ok: true, data: { ui_theme: 'setup', color_mode: 'light' } });
    check(r.attrs['data-ui-theme'] === 'setup', 'the device theme wins over the browser hint');
    check(r.classes.has('light') && r.button.textContent === '☾', 'the device colour mode is applied and the header button follows');
    check(r.store['libreecho-ui-theme'] === 'setup' && r.store['libreecho-theme'] === 'light', 'the browser hint is rewritten to the device choice');

    r = await run({ 'libreecho-ui-theme': 'setup' }, null, true);
    check(r.attrs['data-ui-theme'] === 'setup', 'offline: the browser hint applies immediately without throwing');

    r = await run({}, { ok: true, data: { ui_theme: 'neon', color_mode: 'purple' } });
    check(!('data-ui-theme' in r.attrs) && !r.classes.has('light'), 'unknown values from the device are ignored');

    r = await run({}, { ok: true, data: { ui_theme: 'setup', color_mode: 'dark' } });
    r.ctx.window.leApplyAppearance({ ui_theme: 'classic' });
    check(!('data-ui-theme' in r.attrs), 'switching back to classic removes the theme attribute');

    if (failures) { console.error(`theme UI: ${failures} check(s) failed`); process.exit(1); }
    console.log('theme UI: ok');
})();
