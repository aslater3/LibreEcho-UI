/* Bootloader card, repair confirmation dialog and OTA v3 update display. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function element(id = '') {
    const children = new Map();
    const el = {
        id, innerHTML: '', textContent: '', value: '', disabled: false, hidden: false, checked: false,
        dataset: {}, style: { setProperty() {} }, onclick: null, oninput: null, onchange: null,
        classList: { add() {}, remove() {}, toggle() {}, contains: () => false },
        addEventListener() {}, appendChild(child) { child.parentNode = this; }, removeChild() {},
        setAttribute() {}, select() {}, remove() { this.removed = true; },
        showModal() { this.open = true; }, close() { this.open = false; },
        radios: [],
        querySelector(selector) {
            if (!children.has(selector)) children.set(selector, element(selector));
            return children.get(selector);
        },
        querySelectorAll(selector) { return selector.startsWith('input[name=') ? this.radios : []; }
    };
    return el;
}
const elements = new Map();
const content = element('content');
const toast = element('toast');
const dialogs = [];
const document = {
    body: element('body'),
    querySelector(selector) {
        if (selector === '#content') return content;
        if (selector === '#toast') return toast;
        if (selector.startsWith('#')) {
            if (!elements.has(selector)) elements.set(selector, element(selector));
            return elements.get(selector);
        }
        return element(selector);
    },
    querySelectorAll() { return []; },
    createElement(tag) {
        const el = element(tag);
        if (tag === 'dialog') {
            const radio = value => Object.assign(element('radio'), { value });
            el.radios = [radio('radar_puffin'), radio('biscuit')];
            dialogs.push(el);
        }
        return el;
    },
    addEventListener() {}
};
const requests = [];
let reply = { status: 202, body: { ok: true, data: { started: true, repair: 'in-progress' }, error: null } };
const context = {
    console, document,
    window: { isSecureContext: false, addEventListener() {} },
    navigator: {},
    location: { pathname: '/', hash: '', host: 'fixture', replace() {} },
    history: { pushState() {}, replaceState() {} },
    sessionStorage: { getItem() { return null; }, setItem() {}, removeItem() {} },
    localStorage: { getItem() { return null; }, setItem() {} },
    confirm: () => false,
    prompt: () => '',
    fetch: (url, opt = {}) => {
        if (url === '/api/v1/config') return Promise.reject(new Error('startup fetch intentionally unavailable'));
        requests.push({ url, opt });
        if (url === '/api/v1/system/boot-chain/repair')
            return Promise.resolve({ ok: reply.status < 300, status: reply.status, json: async () => reply.body });
        return Promise.resolve({ ok: true, status: 200, json: async () => ({ ok: true, data: { supported: true, repair: 'in-progress', phase: 'writing-recovery', chain: 'legacy-0.13', board: 'radar_puffin' }, error: null }) });
    },
    URL: { createObjectURL: () => 'blob:unused', revokeObjectURL() {} },
    setTimeout: () => 1,
    clearTimeout() {}
};
vm.createContext(context);
const APP_SOURCE = fs.readFileSync('web/js/app.js', 'utf8');
vm.runInContext(APP_SOURCE, context, { filename: 'app.js' });
const bootChainBody = vm.runInContext('bootChainBody', context);
const bootChainReason = vm.runInContext('bootChainReason', context);
const bootRepairDialog = vm.runInContext('bootRepairDialog', context);
const updateTargetNote = vm.runInContext('updateTargetNote', context);
const reasons = vm.runInContext('bootReasonText', context);
assert(APP_SOURCE.includes('${bootChainPanel(bootChain)}'), 'System page renders the Bootloader card');

const legacy = { supported: true, schema: 1, layout: 'legacy-amonet', chain: 'legacy-0.13', board: 'radar_puffin',
    board_confidence: 'confirmed', repair: 'available', reason: '', phase: '', recovery: 'other', twrp_present: false };
let html = bootChainBody(legacy);
assert(html.includes('LibreEcho 0.13 legacy chain'));
assert(html.includes('Echo (2nd Gen) · confirmed by hardware'));
assert(html.includes('Not installed'));
assert(html.includes('id="boot-chain-repair"') && html.includes('Repair bootloader and install TWRP'));

html = bootChainBody({ ...legacy, repair: 'blocked', reason: 'board_unconfirmed', board: 'unknown', board_confidence: 'unconfirmed' });
assert(!html.includes('boot-chain-repair'), 'blocked state never shows the repair button');
assert(html.includes('could not confirm which Echo model'));
assert(html.includes('Unknown · not confirmed'));
html = bootChainBody({ ...legacy, repair: 'failed', reason: 'mystery_token' });
assert(html.includes('The device reported: mystery_token'), 'unknown reasons fall back to the token');
html = bootChainBody({ ...legacy, repair: 'in-progress', phase: 'writing-recovery' });
assert(html.includes('Repair in progress · writing-recovery') && html.includes('Keep the Echo plugged in'));
assert(!html.includes('boot-chain-repair'));
html = bootChainBody({ ...legacy, chain: 'kaeru-radar-v1', repair: 'not-needed', recovery: 'twrp-radar', twrp_present: true });
assert(html.includes('Kaeru community chain (amonet-radar v1)') && html.includes('>Installed<'));
assert(bootChainBody({ supported: false }).includes('Not supported'));
assert(bootChainBody({ supported: true, error: 'Boot-chain status could not be read', reason: 'emmc_readonly' }).includes('read-only'));
assert(bootChainBody({ ...legacy, board: 'radar_puffin"><script>' }).includes('&quot;&gt;&lt;script&gt;'), 'values are escaped');
for (const token of ['board_unconfirmed', 'board_mismatch', 'layout_not_legacy', 'bootloader_unrecognised',
    'slot_unconfirmed', 'low_space', 'download_failed', 'package_mismatch', 'write_failed_restored',
    'write_failed_unrestored', 'emmc_readonly'])
    assert(reasons[token] && bootChainReason(token) === reasons[token], `reason ${token} has plain English`);
assert(/restored\. The device is safe/.test(reasons.write_failed_restored));
assert(/Do not unplug/.test(reasons.write_failed_unrestored) && /USB/.test(reasons.write_failed_unrestored));

// OTA v3 (0.14) display, without changing v2 rendering.
assert.strictEqual(updateTargetNote({ latest_ota_format: '', latest_target_release: '' }), '');
assert.strictEqual(updateTargetNote({ latest_ota_format: 'v2', pending_ota_format: '' }), '');
assert.strictEqual(updateTargetNote({ latest_ota_format: 'v3', latest_target_release: 'radar-puffin-v0.14.0', immutable_target_v3: true }),
    'radar-puffin-v0.14.0 · LibreEcho 0.14 immutable target (OTA v3)');
assert(updateTargetNote({ pending_ota_format: 'v3', pending_target_release: 'radar-puffin-v0.14.1', latest_ota_format: 'v3', latest_target_release: 'x' })
    .startsWith('radar-puffin-v0.14.1'), 'a pending v3 install is shown in preference to the offer');
assert(updateTargetNote({ latest_ota_format: 'v3', latest_target_release: 'r', immutable_target_v3: false }).includes('cannot install'));

(async () => {
    const dlg = bootRepairDialog(legacy);
    assert(dlg.open, 'dialog is modal');
    for (const phrase of ['Keep the Echo plugged in', 'about a minute', 'restarts by itself', 'Do not unplug',
        'Which Echo is this?', 'Echo (2nd Gen) — tall cylinder speaker, 2017', 'Echo Dot (2nd Gen) — small puck, 2016',
        'Detected by the device: Echo (2nd Gen)', 'Type REPAIR-BOOTLOADER to confirm'])
        assert(dlg.innerHTML.includes(phrase), `dialog says: ${phrase}`);
    assert(!dlg.innerHTML.includes('checked'), 'no board is preselected');
    const start = dlg.querySelector('#boot-repair-start'), typed = dlg.querySelector('#boot-repair-confirm');
    const mismatch = dlg.querySelector('#boot-repair-mismatch');
    const [radar, biscuit] = dlg.radios;
    typed.value = 'REPAIR-BOOTLOADER'; typed.oninput();
    assert(start.disabled, 'typed confirmation alone is not enough');
    biscuit.checked = true; biscuit.onchange();
    assert(start.disabled && !mismatch.hidden && /identity disagrees/.test(mismatch.textContent), 'mismatching board disables repair');
    biscuit.checked = false; radar.checked = true; radar.onchange();
    assert(mismatch.hidden);
    typed.value = 'repair-bootloader'; typed.oninput();
    assert(start.disabled, 'confirmation is exact');
    typed.value = 'REPAIR-BOOTLOADER'; typed.oninput();
    assert(!start.disabled);
    vm.runInContext("state.csrf='csrf-token'", context);
    await start.onclick();
    const sent = requests.find(r => r.url === '/api/v1/system/boot-chain/repair');
    assert(sent && sent.opt.method === 'POST');
    assert.deepStrictEqual(JSON.parse(sent.opt.body), { confirm: 'REPAIR-BOOTLOADER', board: 'radar_puffin' });
    assert.strictEqual(sent.opt.headers['X-LibreEcho-Confirm'], 'confirm-device-action');
    assert.strictEqual(sent.opt.headers['X-LibreEcho-CSRF'], 'csrf-token');
    assert(dlg.removed, 'dialog closes after the repair starts');

    reply = { status: 409, body: { ok: false, data: null, error: { code: 'busy', reason: 'board_mismatch', message: 'Bootloader repair was refused' } } };
    const refused = bootRepairDialog(legacy);
    refused.radios[0].checked = true; refused.radios[0].onchange();
    refused.querySelector('#boot-repair-confirm').value = 'REPAIR-BOOTLOADER';
    refused.querySelector('#boot-repair-confirm').oninput();
    await refused.querySelector('#boot-repair-start').onclick();
    const error = refused.querySelector('#boot-repair-error');
    assert(!error.hidden && error.textContent === reasons.board_mismatch, 'refusal reason is shown in plain English');
    assert(!refused.removed);
    console.log('boot-chain UI card, confirmation dialog and OTA v3 display: ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
