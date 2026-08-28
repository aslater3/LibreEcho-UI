/* Behavioral coverage for the live Timers-page refresh loop. */
'use strict';
const fs = require('fs');
const vm = require('vm');
function classes() { return { add() {}, remove() {}, toggle() {} }; }
function element(id) {
    return { id, innerHTML: '', textContent: '', value: '', disabled: false,
        classList: classes(), style: {}, dataset: {}, onclick: null,
        addEventListener() {}, appendChild() {}, querySelectorAll() { return []; } };
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
    addEventListener() {}, body
};
globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
function storage() { const m = new Map(); return { getItem:k=>m.get(k)||null, setItem:(k,v)=>m.set(k,String(v)), removeItem:k=>m.delete(k), clear:()=>m.clear() }; }
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => false;
globalThis.prompt = () => '';
globalThis.fetch = () => Promise.reject(new Error('startup fetch intentionally unavailable'));
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };
vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const state = vm.runInThisContext('state');
let scheduled;
globalThis.setTimeout = (fn) => { scheduled = fn; return 1; };
globalThis.clearTimeout = () => {};
let reads = 0;
globalThis.api = async path => {
    if (path !== '/timers') throw new Error(`unexpected API path ${path}`);
    reads++;
    return { available: true, ringing: 0, missed: 0,
        timers: [{ id: 1, kind: 'countdown', state: 'pending',
            seconds_remaining: reads === 1 ? 10 : 9, label: 'tea' }] };
};
async function main() {
    state.page = 'Timers';
    await globalThis.timersPage();
    if (!content.innerHTML.includes('10s')) throw new Error('initial timer state was not rendered');
    if (typeof scheduled !== 'function') throw new Error('Timers page did not schedule a refresh');
    await scheduled();
    if (reads !== 2) throw new Error(`expected two timer reads, got ${reads}`);
    if (!content.innerHTML.includes('9s')) throw new Error('refreshed timer state was not rendered');
    console.log('timers UI refresh: ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
