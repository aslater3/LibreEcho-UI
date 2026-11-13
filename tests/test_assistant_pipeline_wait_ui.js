/* The assistant PUT must wait for the voice pipeline restart (aslater3/LibreEcho#365). */
'use strict';
const fs = require('fs');
const vm = require('vm');
function classes() { return { add() {}, remove() {}, toggle() {}, contains() { return false; } }; }
function element(id) {
    return { id, innerHTML: '', textContent: '', value: '', disabled: false,
        classList: classes(), style: {}, dataset: {}, onclick: null,
        addEventListener() {}, appendChild() {}, querySelectorAll() { return []; },
        closest() { return null; },
        focus() { document.activeElement = this; } };
}
const elements = new Map();
const content = element('content');
const nav = element('nav');
const body = element('body');
globalThis.document = {
    querySelector(s) {
        if (s === '#content') return content;
        if (s === '#nav') return nav;
        if (s.startsWith('#')) {
            const id = s.slice(1);
            if (!elements.has(id)) elements.set(id, element(id));
            return elements.get(id);
        }
        return element(s);
    },
    querySelectorAll() { return []; },
    createElement: tag => element(tag),
    addEventListener() {}, body,
    activeElement: null
};
globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
function storage() { const m = new Map(); return { getItem:k=>m.get(k)||null, setItem:(k,v)=>m.set(k,String(v)), removeItem:k=>m.delete(k), clear:()=>m.clear() }; }
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => false;
globalThis.prompt = () => '';
// The dashboard's startup fetch is left pending so no startup error view can
// overwrite the page under test; every request this test needs goes through
// api() below.
globalThis.fetch = () => new Promise(() => {});
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };
vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
// index.html loads integrations-ui.js after app.js, so its Integrations page is
// the live one; load the real browser files in the same order here.
vm.runInThisContext(fs.readFileSync('web/js/integrations-ui.js', 'utf8'), { filename: 'integrations-ui.js' });
const state = vm.runInThisContext('state');
globalThis.setTimeout = fn => { if (typeof fn === 'function') fn(); return 1; };  // sleeps return at once
globalThis.clearTimeout = () => {};
const realNow = Date.now;

const log = [];
let pollStates, assistantFailures, assistant;
const toasts = [];
globalThis.toast = (message, error) => toasts.push({ message, error: Boolean(error) });
const restart = (s, error = '') => ({ mode: 'custom', stt: {}, tts: {}, restart: { state: s, error } });
globalThis.api = async (path, opt = {}) => {
    const method = opt.method || 'GET';
    log.push(`${method} ${path}`);
    if (path === '/integrations') return { items: [] };
    if (path === '/live') return { enabled: false };
    if (path === '/privacy') return {};
    if (path === '/assistant' && method === 'GET') return assistant;
    if (path === '/voice-pipeline' && method === 'PUT') return restart('pending');
    if (path === '/voice-pipeline') return restart(pollStates.shift() || 'ready', pollStates.error);
    if (path === '/assistant') {
        if (assistantFailures-- > 0) throw new Error('Voice assistant service is unavailable');
        return {};
    }
    throw new Error(`unexpected API path ${path}`);
};
let armed;
// Rendering the page also reads /voice-pipeline, so poll states are armed only
// after the render, just before the action under test.
function arm() { log.length = 0; toasts.length = 0; pollStates = [...armed]; pollStates.error = armed.error; }
function reset(states, failures = 0) {
    Date.now = realNow;
    armed = states; armed.error = ''; assistantFailures = failures;
    assistant = { provider: 'openai-compatible', enabled: false, base_url: 'http://192.0.2.10:8000/v1',
        model: 'm', prompt: 'p', clock_format: '12' };
}
function eq(actual, expected, what) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(`${what}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}
const writes = () => log.filter(x => x.startsWith('PUT'));
async function toggleOn() {
    state.page = 'Integrations';
    await integrationsPage();
    arm();
    const input = document.querySelector('#use-local-provider');
    input.checked = true;
    await input.onchange();
}

async function main() {
    // 1. pending, pending, ready: the assistant PUT comes only after the last poll.
    reset(['pending', 'pending', 'ready']);
    await toggleOn();
    const seq = log.slice(0, log.indexOf('PUT /assistant') + 1);
    eq(seq.filter(x => x === 'GET /voice-pipeline').length, 3, 'polls before assistant PUT');
    eq(seq.slice(-5), ['PUT /voice-pipeline', 'GET /voice-pipeline', 'GET /voice-pipeline', 'GET /voice-pipeline', 'PUT /assistant'], 'order');
    if (toasts.some(t => t.error)) throw new Error('success path toasted an error');
    eq(state.busy, false, 'busy cleared');

    // 2. A 503 straight after "ready" is retried (daemon socket not up yet), then succeeds.
    reset(['ready'], 2);
    await toggleOn();
    eq(log.filter(x => x === 'PUT /assistant').length, 3, 'assistant retries');
    if (toasts.some(t => t.error)) throw new Error('retry path toasted an error');

    // 3. Restart failed: error shown, assistant never enabled, page re-rendered.
    reset(['pending', 'failed']); armed.error = 'Voice pipeline daemon restart failed';
    await toggleOn();
    if (log.includes('PUT /assistant')) throw new Error('assistant PUT sent after a failed restart');
    eq(toasts.find(t => t.error)?.message, 'Voice pipeline daemon restart failed', 'failure toast');
    if (!log.includes('GET /assistant')) throw new Error('page was not re-rendered after failure');
    eq(state.busy, false, 'busy cleared after failure');

    // 4. Restart never finishes: bounded timeout, clear error, no assistant PUT.
    reset(Array(1000).fill('pending'));
    let t = 0; Date.now = () => (t += 30000);
    await toggleOn();
    Date.now = realNow;
    if (log.includes('PUT /assistant')) throw new Error('assistant PUT sent after a restart timeout');
    if (!/timed out/.test(toasts.find(x => x.error)?.message || '')) throw new Error('timeout not reported');
    if (log.filter(x => x === 'GET /voice-pipeline').length > 6) throw new Error('timeout polling is not bounded');
    eq(state.busy, false, 'busy cleared after timeout');

    // 5. A persistent 503 gives up after a bounded number of tries and reports it.
    reset(['ready'], 1000);
    await toggleOn();
    eq(log.filter(x => x === 'PUT /assistant').length, 10, 'bounded assistant retries');
    eq(toasts.find(x => x.error)?.message, 'Voice assistant service is unavailable', 'persistent 503 toast');

    // 6. Local LLM settings save uses the same wait.
    reset(['pending', 'ready']);
    state.page = 'Integrations';
    assistant.enabled = true;
    await integrationsPage();
    arm();
    document.querySelector('#local-base-url').value = 'http://192.0.2.10:8000/v1';
    document.querySelector('#local-model').value = 'm';
    await document.querySelector('#save-local-assistant').onclick();
    const save = log.slice(0, log.indexOf('PUT /assistant') + 1);
    eq(save.slice(-4), ['PUT /voice-pipeline', 'GET /voice-pipeline', 'GET /voice-pipeline', 'PUT /assistant'], 'local save order');

    // 7. Local LLM save with a failed restart never reaches /assistant.
    reset(['failed']); armed.error = 'Voice pipeline daemon restart failed';
    assistant.enabled = true;
    await integrationsPage();
    arm();
    await document.querySelector('#save-local-assistant').onclick();
    if (log.includes('PUT /assistant')) throw new Error('local save sent /assistant after a failed restart');
    eq(toasts.find(x => x.error)?.message, 'Voice pipeline daemon restart failed', 'local save failure toast');

    console.log('assistant waits for the voice pipeline restart (toggle and Local LLM save): ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
