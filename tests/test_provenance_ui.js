/* Contract and interaction checks for the About/System provenance presentation. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function classes() {
    const values = new Set();
    return {
        add: value => values.add(value),
        remove: value => values.delete(value),
        toggle: (value, force) => force === undefined ?
            (values.has(value) ? values.delete(value) : values.add(value)) :
            (force ? values.add(value) : values.delete(value)),
        contains: value => values.has(value)
    };
}
function element(id = '') {
    return {
        id, innerHTML: '', textContent: '', value: '', disabled: false, hidden: false,
        dataset: {}, classList: classes(), style: { setProperty() {} }, onclick: null,
        addEventListener() {}, appendChild(child) { child.parentNode = this; },
        removeChild(child) { child.parentNode = null; }, querySelector() { return null; },
        querySelectorAll() { return []; }, setAttribute() {}, select() {}
    };
}

const elements = new Map();
const content = element('content');
const nav = element('nav');
const body = element('body');
const toast = element('toast');
let copyAllowed = true;
let copiedText = '';
const timers = [];
const buttons = [];
const document = {
    body,
    activeElement: null,
    querySelector(selector) {
        if (selector === '#content') return content;
        if (selector === '#nav') return nav;
        if (selector === '#toast') return toast;
        if (selector.startsWith('#')) {
            const id = selector.slice(1);
            if (!elements.has(id)) elements.set(id, element(id));
            return elements.get(id);
        }
        return element(selector);
    },
    querySelectorAll() { return []; },
    createElement: tag => element(tag),
    execCommand(command) {
        assert.strictEqual(command, 'copy');
        copiedText = body._lastChild ? body._lastChild.value : '';
        return copyAllowed;
    },
    addEventListener() {}
};
body.appendChild = child => { child.parentNode = body; body._lastChild = child; };
body.removeChild = child => { if (body._lastChild === child) body._lastChild = null; child.parentNode = null; };
content.querySelectorAll = selector => selector === '.copy-provenance' ? buttons : [];
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
    fetch: () => Promise.reject(new Error('startup fetch intentionally unavailable')),
    URL: { createObjectURL: () => 'blob:unused', revokeObjectURL() {} },
    setTimeout: callback => { timers.push(callback); return timers.length; },
    clearTimeout() {}
};
vm.createContext(context);
const APP_SOURCE = fs.readFileSync('web/js/app.js', 'utf8');
vm.runInContext(APP_SOURCE, context, { filename: 'app.js' });
assert(APP_SOURCE.includes('provenancePanel(provenance)'), 'System uses the provenance presenter');
assert(APP_SOURCE.includes('provenancePanel(p)'), 'About uses the provenance presenter');
const provenancePanel = vm.runInContext('provenancePanel', context);
const bindProvenanceCopy = vm.runInContext('bindProvenanceCopy', context);

const hashes = {
    source: '0123456789012345678901234567890123456789012345678901234567890123',
    effective: 'abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd',
    capsule: '1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef',
    candidate: '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff',
    running: 'fedcbafedcbafedcbafedcbafedcbafedcbafedcbafedcbafedcbafedcbafedc'
};
const components = ['airplay2', 'tts', 'wakeword', 'stt', 'assistant'].map((feature_id, index) => ({
    feature_id,
    release: 'radar-puffin-v0.13.11',
    source_commit: hashes.source,
    effective_payload_sha256: hashes.effective,
    runtime_capsule_sha256: index === 0 ? null : hashes.capsule,
    candidate_kind: index === 0 ? 'unavailable' : 'runtime',
    candidate_payload_sha256: index === 0 ? 'unavailable' : hashes.candidate,
    candidate_status: index === 0 ? 'unavailable' : 'present',
    running_daemon_sha256: hashes.running,
    running_daemon_status: index === 0 ? 'not-running' : 'running',
    effective: index === 0 ? 'missing' : 'present',
    activation: index === 0 ? 'unavailable' : 'reboot',
    last_transaction_result: 'installed'
}));
const html = provenancePanel({
    os_version: 'LibreEcho OS 0.13.11', source_commit: hashes.source,
    source_digest: hashes.effective, transaction_state: 'none',
    last_transaction_result: 'installed', components
});
assert.strictEqual((html.match(/<tr>/g) || []).length, 6, 'header plus five component rows');
assert.strictEqual((html.match(/<details/g) || []).length, 6, 'five rows plus build identity details');
assert.strictEqual((html.match(/<summary/g) || []).length, 6, 'every native details control has a summary');
assert(html.includes('class="data-table provenance-table"'));
assert(html.includes('radar-puffin-v0.13.11'));
assert(html.includes('Installed · Running · Candidate runtime present · Restart required · Installed'));
assert(html.includes('Payload missing · Not running · No restart pending · Installed'));
assert(html.includes('runtime capsule hash'));
assert(html.includes('Candidate (runtime) SHA-256'));
assert(html.includes('Candidate status'));
assert(html.includes('Copy source commit'));
assert(html.includes('Copy build source digest'));
assert(html.includes('Unavailable'), 'null and missing identity use an honest friendly fallback');
const firstTechnicalDetails = html.indexOf('<details class="provenance-details">');
assert(firstTechnicalDetails > 0);
assert(!html.slice(0, firstTechnicalDetails).includes(hashes.source), 'full source commit is not inline by default');
assert(!html.slice(0, firstTechnicalDetails).includes(hashes.effective), 'full payload hash is not inline by default');

function copyButton(label, value) {
    const button = element();
    button.textContent = 'Copy';
    button.dataset.copyLabel = label;
    button.dataset.copyValue = value;
    buttons.push(button);
    return button;
}
const success = copyButton('source commit', hashes.source);
bindProvenanceCopy();
(async () => {
    await success.onclick();
    assert.strictEqual(success.textContent, 'Copied');
    assert.strictEqual(copiedText, hashes.source);
    copyAllowed = false;
    const failure = copyButton('source digest', hashes.effective);
    bindProvenanceCopy();
    await failure.onclick();
    assert.strictEqual(failure.textContent, 'Copy failed');
    assert(toast.textContent.startsWith('Copy failed:'), 'copy failure is reported instead of claiming success');
    console.log('provenance UI details, native controls, and copy outcomes: ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
