/* Behavioral coverage for the Logs-page kernel-log download control.
 *
 * This loads the real browser file and drives logsPage() with a minimal DOM and
 * fake API. It verifies the rendered control, endpoint, Blob download, and
 * user-visible success/error transitions rather than merely grepping source.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

function classList() { return { add() {}, remove() {}, toggle() {} }; }
function element(id) {
    return {
        id, innerHTML: '', textContent: '', value: '', disabled: false,
        classList: classList(), style: {}, dataset: {}, children: [],
        onclick: null, oninput: null,
        addEventListener() {}, appendChild() {}, click() { if (this.onclick) return this.onclick(); }
    };
}
const elements = new Map();
const content = element('content');
const nav = element('nav');
const body = element('body');
const documentShim = {
    querySelector(selector) {
        if (selector === '#content') return content;
        if (selector === '#nav') return nav;
        if (selector.startsWith('#')) {
            const id = selector.slice(1);
            if (!elements.has(id)) elements.set(id, element(id));
            return elements.get(id);
        }
        return element(selector);
    },
    querySelectorAll() { return []; },
    createElement(tag) { return element(tag); },
    addEventListener() {},
    body
};
function storage() {
    const values = new Map();
    return { getItem: k => values.get(k) || null, setItem: (k, v) => values.set(k, String(v)),
        removeItem: k => values.delete(k), clear: () => values.clear() };
}
globalThis.document = documentShim;
globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => false;
globalThis.prompt = () => '';
globalThis.URL = { createObjectURL: () => 'blob:kernel-log', revokeObjectURL() {} };
globalThis.fetch = () => Promise.reject(new Error('startup fetch intentionally unavailable'));
vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const state = vm.runInThisContext('state');

let failures = 0;
function check(name, condition, detail) {
    if (condition) console.log(`  ok   ${name}`);
    else { failures++; console.log(`  FAIL ${name}${detail === undefined ? '' : ` — ${detail}`}`); }
}
async function main() {
    const calls = [];
    globalThis.api = async (path) => {
        calls.push(path);
        if (path === '/logs') return { entries: [], capacity: 128 };
        if (path === '/diagnostics') return { checks: [] };
        throw new Error(`unexpected API path ${path}`);
    };
    state.page = 'Logs';
    state.token = 'test-token';
    state.data = { status: { simulated: false } };
    await globalThis.logsPage();
    const button = elements.get('kernel-log-download');
    check('Logs page renders the download control', content.innerHTML.includes('kernel-log-download'));
    check('Logs page loads its existing API panels', calls.includes('/logs') && calls.includes('/diagnostics'), calls.join(','));
    check('download handler is bound', typeof button.onclick === 'function');

    let requested;
    let clicked;
    globalThis.fetch = async (url, options) => {
        requested = { url, options };
        return { ok: true, async text() { return 'kernel line 1\nkernel line 2\n'; } };
    };
    globalThis.document.createElement = (tag) => {
        const a = element(tag);
        a.click = () => { clicked = { href: a.href, download: a.download }; };
        return a;
    };
    await button.onclick();
    check('download requests the streaming endpoint', requested && requested.url === '/api/v1/diagnostics/kernel.log', requested && requested.url);
    check('download uses the current bearer token when present', requested && requested.options.headers.Authorization === 'Bearer ' + state.token);
    check('download creates a timestamped kernel-log filename', clicked && /^libreecho-kernel-.*\.log$/.test(clicked.download), clicked && clicked.download);
    check('download uses the streamed text as a Blob URL', clicked && clicked.href === 'blob:kernel-log', clicked && clicked.href);
    if (failures) process.exitCode = 1;
    else console.log('kernel log UI: ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
