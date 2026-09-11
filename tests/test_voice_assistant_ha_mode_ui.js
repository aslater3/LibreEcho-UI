/* Behavioral coverage for the mode-aware Voice Assistants card. */
'use strict';
const fs = require('fs');
const vm = require('vm');
function classes() { return { add() {}, remove() {}, toggle() {} }; }
function element(id) {
    return { id, innerHTML: '', textContent: '', value: '', disabled: false,
        classList: classes(), style: {}, dataset: {}, onclick: null,
        addEventListener() {}, appendChild() {}, querySelectorAll() { return []; },
        focus() {} };
}
const elements = new Map();
const content = element('content');
const body = element('body');
globalThis.document = {
    querySelector(s) {
        if (s === '#content') return content;
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
function storage() { const m = new Map(); return { getItem: k => m.get(k) || null, setItem: (k, v) => m.set(k, String(v)), removeItem: k => m.delete(k), clear: () => m.clear() }; }
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => false;
globalThis.prompt = () => '';
// Hold the app's startup fetches open so its boot never settles; each case
// below drives the page through its own stubbed api() instead.
globalThis.fetch = () => new Promise(() => {});
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };
globalThis.setTimeout = () => 0;
globalThis.clearTimeout = () => {};
vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const integrationsPage = vm.runInThisContext('integrationsPage');

const integrations = enabled => ({ items: [
    { id: 'home-assistant', name: 'Home Assistant', enabled },
    { id: 'mqtt', name: 'MQTT', enabled: true }
] });
const assistantStatus = { authenticated: false, enabled: false, provider_name: 'ChatGPT',
    model: 'gpt-5-codex', prompt: 'Be brief.', wake_connected: false,
    audio_connected: false, completed_transcripts: 0, latency_target_ms: 3000,
    latency_violations: 0 };
const assistantDown = () => { throw new Error('Voice assistant service is unavailable'); };

async function main() {
    // Home Assistant mode: the local assistant is stopped by design; the page
    // must explain the hand-off instead of surfacing the raw unsupported notice.
    globalThis.api = async path => {
        if (path === '/integrations') return integrations(true);
        if (path === '/assistant') return assistantDown();
        throw new Error(`unexpected API path ${path}`);
    };
    await integrationsPage();
    if (!content.innerHTML.includes('Managed by Home Assistant'))
        throw new Error('Home Assistant mode did not render the managed-by notice');
    if (!content.innerHTML.includes('Wyoming'))
        throw new Error('Home Assistant mode did not explain the Wyoming hand-off');
    if (content.innerHTML.includes('Voice assistant service is unavailable'))
        throw new Error('Home Assistant mode still surfaced the raw unsupported notice');
    if (content.innerHTML.includes('assistant-auth-start') || content.innerHTML.includes('assistant-enabled'))
        throw new Error('Home Assistant mode still rendered the local assistant controls');
    if (!content.innerHTML.includes('int-home-assistant'))
        throw new Error('Home Assistant mode removed the integration controls');

    // Local mode: unchanged rendering.
    globalThis.api = async path => {
        if (path === '/integrations') return integrations(false);
        if (path === '/assistant') return assistantStatus;
        throw new Error(`unexpected API path ${path}`);
    };
    await integrationsPage();
    if (content.innerHTML.includes('Managed by Home Assistant'))
        throw new Error('local mode rendered the Home Assistant notice');
    if (!content.innerHTML.includes('Connect ChatGPT'))
        throw new Error('local mode did not render the assistant controls');
    if (!content.innerHTML.includes('Local LLM'))
        throw new Error('local mode did not render the local LLM panel');

    // Local mode with the assistant service down keeps the existing notice.
    globalThis.api = async path => {
        if (path === '/integrations') return integrations(false);
        if (path === '/assistant') return assistantDown();
        throw new Error(`unexpected API path ${path}`);
    };
    await integrationsPage();
    if (!content.innerHTML.includes('Not supported') ||
        !content.innerHTML.includes('Voice assistant service is unavailable'))
        throw new Error('unavailable local assistant no longer renders the unsupported notice');

    console.log('voice assistant mode-aware card: ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
