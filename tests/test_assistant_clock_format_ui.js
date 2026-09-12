/* Behavioral coverage for the spoken clock-format setting on the Integrations page. */
'use strict';
const fs = require('fs');
const vm = require('vm');
function classes() { return { add() {}, remove() {}, toggle() {} }; }
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
const integrationsPage = vm.runInThisContext('integrationsPage');
globalThis.setTimeout = () => 1;
globalThis.clearTimeout = () => {};

const puts = [];
let assistant = {};
globalThis.api = async (path, opt = {}) => {
    if ((opt.method || 'GET') === 'PUT') puts.push({ path, body: JSON.parse(opt.body) });
    if (path === '/integrations') return { items: [{ id: 'rest', name: 'REST API', enabled: true }] };
    if (path === '/assistant') return assistant;
    if (path === '/voice-pipeline') return { mode: 'custom', stt: {}, tts: {} };
    if (path === '/privacy') return {};
    throw new Error(`unexpected API path ${path}`);
};
function lastPutBody(path) { const all = puts.filter(x => x.path === path); return all.length ? all[all.length - 1].body : undefined; }

async function main() {
    state.page = 'Integrations';

    // A selected OpenAI-compatible local assistant renders the spoken time
    // selector, showing the format the daemon reported.
    assistant = { provider: 'openai-compatible', enabled: true,
        base_url: 'http://192.0.2.10:8000/v1', model: 'gemma-3-4b',
        prompt: 'Reply briefly.', clock_format: '24' };
    await integrationsPage();
    if (!content.innerHTML.includes('id="local-clock-format"'))
        throw new Error('local assistant panel did not render the spoken time selector');
    if (!content.innerHTML.includes('<option value="24" selected>'))
        throw new Error('spoken time selector did not select the reported 24-hour format');
    if (content.innerHTML.includes('id="assistant-clock-format"'))
        throw new Error('unselected device assistant rendered its selector');

    // Saving the local assistant sends the chosen format with the rest of the
    // local settings.
    document.querySelector('#local-clock-format').value = '12';
    document.querySelector('#local-base-url').value = 'http://192.0.2.10:8000/v1';
    document.querySelector('#local-model').value = 'gemma-3-4b';
    document.querySelector('#local-prompt').value = 'Reply briefly.';
    await document.querySelector('#save-local-assistant').onclick();
    const local = lastPutBody('/assistant');
    if (!local) throw new Error('local assistant save did not reach /assistant');
    if (local.clock_format !== '12')
        throw new Error(`local assistant save omitted clock_format (got ${JSON.stringify(local.clock_format)})`);
    if (local.provider !== 'openai-compatible' || local.model !== 'gemma-3-4b')
        throw new Error('local assistant save lost its provider settings');

    // With the device assistant selected, both panels render distinct spoken
    // time selectors and the device save keeps sending its own choice.
    assistant = { provider: 'openai-codex', enabled: true, authenticated: true,
        provider_name: 'ChatGPT', model: 'gpt-5.4', prompt: 'Reply briefly.',
        clock_format: '12' };
    await integrationsPage();
    if (!content.innerHTML.includes('id="local-clock-format"') ||
        !content.innerHTML.includes('id="assistant-clock-format"'))
        throw new Error('both assistant panels should render distinct spoken time selectors');
    document.querySelector('#assistant-clock-format').value = '24';
    document.querySelector('#assistant-model').value = 'gpt-5.4';
    document.querySelector('#assistant-prompt').value = 'Reply briefly.';
    await document.querySelector('#save-assistant').onclick();
    const device = lastPutBody('/assistant');
    if (!device || device.clock_format !== '24')
        throw new Error(`device assistant save omitted clock_format (got ${JSON.stringify(device && device.clock_format)})`);
    if (device.provider !== 'openai-codex')
        throw new Error('device assistant save lost its provider');

    console.log('assistant spoken clock format renders and saves for both providers: ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
