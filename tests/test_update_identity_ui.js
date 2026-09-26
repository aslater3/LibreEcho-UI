/*
 * Browser contract coverage for the resolved update identity on the System page.
 *
 * The device resolves the candidate identity for a check (release tag and OTA
 * digest) and the page must name the exact build an available update would
 * install, keep the version-only message when no identity was resolved, and
 * never let a recorded value reach the DOM unescaped. The page is rendered here
 * against a stubbed API, so this covers the rendering contract itself rather
 * than the transport.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

function classes() { return { add() {}, remove() {}, toggle() {} }; }
function element(id) {
    return { id, innerHTML: '', textContent: '', value: '', disabled: false, hidden: false,
        classList: classes(), style: {}, dataset: {}, onclick: null, onchange: null,
        title: '', addEventListener() {}, appendChild() {}, setAttribute() {},
        querySelectorAll() { return []; }, focus() {} };
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
    addEventListener() {}, body, activeElement: null
};
globalThis.window = { addEventListener() {} };
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
globalThis.prompt = () => '';
globalThis.fetch = () => Promise.reject(new Error('startup fetch intentionally unavailable'));
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };

vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const systemPage = vm.runInThisContext('systemPage');

const TAG = 'radar-puffin-build-0123456-0123456789abcdef-fedcba9876543210';
const DIGEST = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

function update(overrides) {
    return Object.assign({
        supported: true, current_slot: 'b', inactive_slot: 'a', state: 'idle', progress: 0,
        pending_reboot: false, pending_version: '', installed_version: 'LibreEcho OS 0.13.9',
        latest_version: '0.14.0', channel: 'dev', source: 'github-releases',
        source_reachable: 'true', check_status: 'update-available', check_error: '',
        last_check_epoch: 1789000000, last_success_epoch: 1789000000,
        automatic_updates: false, rollback_available: false, rollback_version: '',
        allow_unsigned: false, max_upload_bytes: 33554432, max_upload_ceiling_bytes: 33554432
    }, overrides);
}

let ota = update({});
globalThis.api = async path => {
    if (path === '/system') {
        return { timezone: 'UTC', ntp: true, ntp_state: 'synchronized',
            last_sync_epoch: 1789000000, clock_source: 'ntp', rtc_available: true,
            rtc_persisted: true, ntp_servers: 'ntp.example', update_channel: 'dev' };
    }
    if (path === '/device') {
        return { os_version: 'LibreEcho OS 0.13.9', kernel: '6.1.0', hostname: 'fake-echo' };
    }
    if (path === '/system/update') return ota;
    if (path === '/system/features') return { simulation: false, https: false, usb_role_supported: false };
    if (path === '/provenance') return {};
    throw new Error(`unexpected API path ${path}`);
};

function assert(condition, message) { if (!condition) throw new Error(message); }

function updatePanel(html) {
    const start = html.indexOf('System update');
    const end = html.indexOf('Configuration and diagnostics');
    if (start < 0 || end < start) throw new Error('the System update panel was not rendered');
    return html.slice(start, end);
}

async function renderWith(value) {
    ota = value;
    content.innerHTML = '';
    await systemPage();
    return content.innerHTML;
}

(async function main() {
    await new Promise(resolve => setTimeout(resolve, 0));

    // A completed development check names the exact build and digest.
    let html = await renderWith(update({ resolved_release_tag: TAG, ota_sha256: DIGEST }));
    let panel = updatePanel(html);
    assert(panel.includes(`<dt>Release tag</dt><dd class="mono wrap">${TAG}</dd>`),
        'the resolved release tag was not rendered');
    assert(panel.includes(`<dt>OTA SHA-256</dt><dd class="mono wrap">${DIGEST}</dd>`),
        'the resolved OTA digest was not rendered');
    assert(panel.includes(`Version 0.14.0 is available from GitHub Releases: release ${TAG} · SHA-256 ${DIGEST}.`),
        'the update message does not name the build and the digest');

    // An older device, a failed check and a cleared record all leave the
    // version-only message standing rather than inventing an identity.
    for (const absent of [{}, { resolved_release_tag: '', ota_sha256: '' },
                          { resolved_release_tag: '   ', ota_sha256: '\n' }]) {
        html = await renderWith(update(absent));
        panel = updatePanel(html);
        assert(panel.includes('Version 0.14.0 is available from GitHub Releases.'),
            'the version-only update message was lost when no identity was resolved');
        assert(!panel.includes('Release tag') && !panel.includes('OTA SHA-256'),
            'an identity row was rendered without a resolved identity');
        assert(!/undefined|NaN|>null</.test(panel),
            'the panel printed a placeholder instead of staying honest');
    }

    // A state that is not an available update still shows the identity the last
    // completed check resolved, and stays quiet when there is none.
    html = await renderWith(update({ check_status: 'up-to-date', resolved_release_tag: TAG, ota_sha256: DIGEST }));
    panel = updatePanel(html);
    assert(panel.includes('Release tag') && panel.includes(TAG),
        'a resolved identity was hidden for an up-to-date check');
    assert(!panel.includes('is available from GitHub Releases'),
        'an up-to-date check claimed an available update');
    html = await renderWith(update({ check_status: 'error', check_error: 'download_transport' }));
    panel = updatePanel(html);
    assert(!panel.includes('Release tag') && !panel.includes('OTA SHA-256'),
        'a failed check displayed an identity');

    // Recorded values are escaped, so a hostile record cannot inject markup.
    html = await renderWith(update({ resolved_release_tag: '<img src=x onerror=alert(1)>',
        ota_sha256: '"><script>alert(1)</script>' }));
    panel = updatePanel(html);
    assert(!panel.includes('<img') && panel.includes('&lt;img src=x onerror=alert(1)&gt;'),
        'the release tag was not escaped before rendering');
    assert(!panel.includes('<script>') && panel.includes('&quot;&gt;&lt;script&gt;'),
        'the OTA digest was not escaped before rendering');

    console.log('update identity browser contract: ok');
})().catch(error => {
    console.error(`update identity browser contract: FAILED - ${error.message}`);
    process.exit(1);
});
