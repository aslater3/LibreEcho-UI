/*
 * Time zone picker (#371): the generated IANA -> POSIX table, the shared
 * helpers, and the Device settings panel rendered against a stubbed API.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

function assert(condition, message) { if (!condition) throw new Error(message); }
function classes() { return { add() {}, remove() {}, toggle() {} }; }
function element(id) {
    return { id, innerHTML: '', textContent: '', value: '', disabled: false, hidden: false, checked: false,
        classList: classes(), style: {}, dataset: {}, onclick: null, onchange: null,
        title: '', addEventListener() {}, appendChild() {}, setAttribute() {},
        querySelectorAll() { return []; }, focus() {} };
}
const elements = new Map();
const content = element('content');
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
    addEventListener() {}, body: element('body'), activeElement: null
};
globalThis.window = globalThis;
globalThis.addEventListener = () => {};
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
function storage() {
    const m = new Map();
    return { getItem: k => m.get(k) || null, setItem: (k, v) => m.set(k, String(v)),
        removeItem: k => m.delete(k), clear: () => m.clear() };
}
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => false;
globalThis.fetch = () => Promise.reject(new Error('startup fetch intentionally unavailable'));

vm.runInThisContext(fs.readFileSync('web/js/timezones.js', 'utf8'), { filename: 'timezones.js' });
vm.runInThisContext(fs.readFileSync('web/js/timezone-ui.js', 'utf8'), { filename: 'timezone-ui.js' });

// The table: unique IANA names, every POSIX string inside the device's filter
// (src/timezone.h, the initramfs), and the zones people actually pick.
const table = window.LE_TIMEZONES;
assert(Array.isArray(table) && table.length > 300, 'the time zone table is missing or truncated');
const names = new Set();
for (const [name, posix] of table) {
    assert(!names.has(name), `duplicate zone ${name}`);
    names.add(name);
    assert(/^[A-Za-z0-9_+\-/]{1,63}$/.test(name), `zone name ${name} would be rejected by the API`);
    assert(/^[A-Za-z0-9+\-:,./]{1,63}$/.test(posix), `${name}: ${posix} would be rejected by the device`);
}
const T = window.LETimezone;
assert(T.posixFor('Europe/London') === 'GMT0BST,M3.5.0/1,M10.5.0', 'Europe/London');
assert(T.posixFor('America/Chicago') === 'CST6CDT,M3.2.0,M11.1.0', 'America/Chicago');
assert(T.posixFor('Asia/Dubai') === 'LST-4', 'Asia/Dubai must use letters, not <+04>');
assert(T.posixFor('UTC') === 'UTC0', 'UTC');
assert(T.posixFor('Nowhere/Else') === '', 'unknown names have no POSIX string');

// A stored zone maps back to the saved name, else the first name with the same rules.
assert(T.nameFor('GMT0BST,M3.5.0/1,M10.5.0', 'Europe/London') === 'Europe/London', 'saved name kept');
assert(T.nameFor('GMT0BST,M3.5.0/1,M10.5.0', 'America/Chicago') !== 'America/Chicago', 'a stale name is not shown');
assert(T.posixFor(T.nameFor('CST6CDT,M3.2.0,M11.1.0', '')) === 'CST6CDT,M3.2.0,M11.1.0', 'zone from HA has a name');
assert(T.nameFor('UTC', '') && T.same(T.posixFor(T.nameFor('UTC', '')), 'UTC'), 'the UTC default is listed');
assert(T.nameFor('XYZ3', '') === '', 'a custom zone has no name');

// Options: grouped, selected, and a custom zone stays visible and escaped.
let html = T.options('Europe/London', 'GMT0BST,M3.5.0/1,M10.5.0');
assert(html.includes('<optgroup label="Europe">') && html.includes('<option value="Europe/London" selected>London</option>'), 'London is selected under Europe');
assert(html.includes('<option value="America/Argentina/Buenos_Aires">Argentina/Buenos Aires</option>'), 'nested names are readable');
html = T.options('', 'X<b>1');
assert(html.startsWith('<option value="" selected>Custom: X&lt;b&gt;1</option>'), 'custom zones are shown escaped');

// Device settings panel.
vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const timezonePanel = vm.runInThisContext('timezonePanel');
const bindTimezone = vm.runInThisContext('bindTimezone');
const timezoneText = vm.runInThisContext('timezoneText');
const puts = [];
vm.runInThisContext('mutate=async(path,data,message)=>{globalThis.__puts.push({path,data,message})}');
globalThis.__puts = puts;

html = timezonePanel({ timezone: 'GMT0BST,M3.5.0/1,M10.5.0', name: 'Europe/London', source: 'manual' });
assert(html.includes('<select id="timezone-name">'), 'the picker is enabled for a manual zone');
assert(html.includes('Use the time zone from Home Assistant'), 'the Home Assistant option is offered');
assert(html.includes('London (GMT0BST,M3.5.0/1,M10.5.0)'), 'the zone in use is named');
html = timezonePanel({ timezone: 'CST6CDT,M3.2.0,M11.1.0', name: '', source: 'home_assistant' });
assert(html.includes('<select id="timezone-name" disabled>'), 'the picker is locked while Home Assistant sets the zone');
assert(html.includes('from Home Assistant'), 'the source is shown');
assert(timezonePanel(null).includes('not available'), 'a missing endpoint is reported, not hidden');
assert(timezoneText('UTC', '', 'manual').startsWith('UTC'), 'the factory default reads as UTC');

(async function main() {
    const tz = { timezone: 'UTC', name: '', source: 'manual' };
    elements.clear();
    bindTimezone(tz);
    const select = document.querySelector('#timezone-name');
    const follow = document.querySelector('#timezone-ha');
    const save = document.querySelector('#save-timezone');
    select.value = 'Asia/Kathmandu';
    select.onchange();
    assert(!save.disabled, 'changing the zone enables Save');
    await save.onclick();
    assert(puts.length === 1 && puts[0].path === '/system/timezone', 'save writes the time zone');
    assert(JSON.stringify(puts[0].data) === JSON.stringify({ timezone: 'LST-5:45', name: 'Asia/Kathmandu', source: 'manual' }),
        `unexpected manual payload ${JSON.stringify(puts[0].data)}`);
    follow.checked = true;
    follow.onchange();
    assert(select.disabled, 'following Home Assistant locks the picker');
    await save.onclick();
    assert(JSON.stringify(puts[1].data) === JSON.stringify({ source: 'home_assistant' }), 'follow mode sends only the source');
    console.log('timezone ui: ok');
})().catch(error => { console.error(error); process.exit(1); });
