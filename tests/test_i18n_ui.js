/* Interface languages: the real web/js/i18n*.js files run under a tiny DOM.
   Covers the English fallback, the complete German catalog, browser-language
   detection on first run, the persisted choice, and data-i18n markup in
   index.html matching the English catalog. */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = path.join(__dirname, '..', 'web');
const read = f => fs.readFileSync(path.join(root, f), 'utf8');
let failures = 0;
function check(ok, message) { if (ok) console.log('  ok: ' + message); else { failures++; console.error('  FAIL: ' + message); } }

function element(attrs, text) {
    const el = { textContent: text || '', attrs: Object.assign({}, attrs) };
    el.getAttribute = k => (k in el.attrs ? el.attrs[k] : null);
    el.setAttribute = (k, v) => { el.attrs[k] = String(v); };
    return el;
}

function harness({ saved, languages, navLanguage, elements = [] }) {
    const store = {};
    if (saved !== undefined) store['libreecho-lang'] = saved;
    const html = { lang: '' };
    const ctx = {
        localStorage: {
            getItem: k => (k in store ? store[k] : null),
            setItem: (k, v) => { store[k] = String(v); },
            removeItem: k => { delete store[k]; }
        },
        navigator: { language: navLanguage, languages: languages || [navLanguage] },
        document: {
            readyState: 'complete',
            documentElement: html,
            querySelectorAll: sel => elements.filter(e => (sel === '[data-i18n]' ? 'data-i18n' in e.attrs : 'data-i18n-aria' in e.attrs)),
            addEventListener: () => {}
        }
    };
    ctx.window = ctx;
    vm.createContext(ctx);
    for (const f of ['i18n.js', 'i18n-en.js', 'i18n-de.js']) vm.runInContext(read('js/' + f), ctx, { filename: f });
    return { ctx, store, html };
}

(async () => {
    console.log('interface languages');

    let h = harness({ navLanguage: 'fr-FR', languages: ['fr-FR'] });
    check(h.ctx.LeI18n.current() === 'en', 'an unsupported browser language falls back to English');
    check(h.ctx.LeI18n.t('nav.Network') === 'Network', 'English catalog serves nav labels');
    check(h.ctx.LeI18n.t('no.such.key') === 'no.such.key', 'an unknown key is returned as-is rather than throwing');

    h = harness({ navLanguage: 'de-DE', languages: ['de-DE', 'en-US'] });
    check(h.ctx.LeI18n.current() === 'de', 'first run follows a German browser language');
    check(h.ctx.LeI18n.t('nav.Network') === 'Netzwerk', 'German catalog is used when selected by the browser');
    check(h.html.lang === 'de', 'document language follows the selection');
    check(h.ctx.LeI18n.saved() === '', 'nothing is saved until the owner picks a language');

    h = harness({ saved: 'en', navLanguage: 'de-DE', languages: ['de-DE'] });
    check(h.ctx.LeI18n.current() === 'en', 'a saved choice wins over the browser language');

    h = harness({ navLanguage: 'en-GB', languages: ['en-GB'] });
    h.ctx.LeI18n.set('de');
    check(h.store['libreecho-lang'] === 'de', 'setting German persists the choice');
    check(h.ctx.LeI18n.t('status.uptime') === 'Betriebszeit', 'the saved German choice is used at once');
    h.ctx.LeI18n.set('fr');
    check(h.store['libreecho-lang'] === 'de', 'an unregistered language is not stored');
    h.ctx.LeI18n.set('');
    check(!('libreecho-lang' in h.store), 'an empty choice clears the saved language (follow browser)');

    const complete = harness({ navLanguage: 'en' });
    const keysOf = file => [...read(file).matchAll(/^ '([^']+)':/gm)].map(m => m[1]);
    const enCatalog = keysOf('js/i18n-en.js');
    const deCatalog = keysOf('js/i18n-de.js');
    check(enCatalog.length > 40, 'English catalog has the control-centre strings (' + enCatalog.length + ')');
    const missing = enCatalog.filter(k => !deCatalog.includes(k));
    check(missing.length === 0, 'German catalog translates every English key' + (missing.length ? ': missing ' + missing.join(', ') : ''));
    check(complete.ctx.LeI18n.languages().map(l => l.code).join(',') === 'en,de', 'both languages are registered, English first');

    const index = read('index.html');
    const scripts = [...index.matchAll(/<script\s+src="\/js\/([^"?]+)/g)].map(m => m[1]);
    const i18nAt = scripts.indexOf('i18n.js');
    check(i18nAt >= 0 && i18nAt < scripts.indexOf('app.js') && scripts.indexOf('i18n-en.js') < scripts.indexOf('app.js') && scripts.indexOf('i18n-de.js') < scripts.indexOf('app.js'),
        'index.html loads the helper and both catalogs before app.js');
    const used = [...index.matchAll(/data-i18n(?:-aria)?="([^"]+)"/g)].map(m => m[1]);
    const unknown = used.filter(k => !enCatalog.includes(k));
    check(used.length > 0 && unknown.length === 0, 'every data-i18n key in index.html exists in the English catalog' + (unknown.length ? ': ' + unknown.join(', ') : ''));

    const app = read('js/app.js');
    check(/function tr\(key,fallback\)/.test(app) && /LeI18n\.languages\(\)/.test(app), 'app.js uses the helper for nav labels and the language picker');

    const els = [
        element({ 'data-i18n': 'header.reboot' }, 'Reboot'),
        element({ 'data-i18n-aria': 'header.menu_aria', 'aria-label': 'Toggle navigation' }, '')
    ];
    h = harness({ saved: 'de', navLanguage: 'en', elements: els });
    check(els[0].textContent === 'Neu starten', 'apply() translates data-i18n text in the selected language');
    check(els[1].getAttribute('aria-label') === 'Navigation umschalten', 'apply() translates data-i18n-aria labels');

    if (failures) { console.error(failures + ' check(s) failed'); process.exit(1); }
    console.log('interface languages: ok');
})();
