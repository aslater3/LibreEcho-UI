/* Stop without the wake word (#405): the On Device assistant setting renders and saves. */
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

    // Off by default: a daemon that reports false (or an older one that
    // reports nothing) renders the switch unchecked.
    assistant = { provider: 'openai-codex', enabled: true, authenticated: true,
        provider_name: 'ChatGPT', model: 'gpt-5.4', prompt: 'Reply briefly.',
        clock_format: '12' };
    await integrationsPage();
    const html = content.innerHTML;
    const match = html.match(/<input class="toggle-input" id="assistant-stop-without-wake" type="checkbox" ([^>]*)>/);
    if (!match) throw new Error('device assistant panel did not render the stop-without-wake switch');
    if (match[1].includes('checked')) throw new Error('stop without the wake word must default to off');
    if (!html.includes('never sent to a speech server'))
        throw new Error('the switch must say the audio stays on the device');
    // The Wyoming (local LLM) panel cannot offer it: its recogniser is remote.
    if (html.includes('id="local-stop-without-wake"'))
        throw new Error('the local LLM panel must not offer stop without the wake word');

    // Turning it on sends the boolean with the rest of the device settings.
    document.querySelector('#assistant-stop-without-wake').checked = true;
    document.querySelector('#assistant-clock-format').value = '12';
    document.querySelector('#assistant-model').value = 'gpt-5.4';
    document.querySelector('#assistant-prompt').value = 'Reply briefly.';
    await document.querySelector('#save-assistant').onclick();
    let body = lastPutBody('/assistant');
    if (!body || body.stop_without_wake !== true)
        throw new Error(`save did not send stop_without_wake:true (got ${JSON.stringify(body)})`);
    if (body.provider !== 'openai-codex' || body.clock_format !== '12')
        throw new Error('save lost the other device assistant settings');

    // A device that has it on renders it checked, and turning it off saves false.
    assistant.stop_without_wake = true;
    await integrationsPage();
    if (!/id="assistant-stop-without-wake" type="checkbox" checked/.test(content.innerHTML))
        throw new Error('an enabled setting must render checked');
    document.querySelector('#assistant-stop-without-wake').checked = false;
    await document.querySelector('#save-assistant').onclick();
    body = lastPutBody('/assistant');
    if (!body || body.stop_without_wake !== false)
        throw new Error(`save did not send stop_without_wake:false (got ${JSON.stringify(body)})`);

    console.log('stop without the wake word renders off by default and saves: ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
