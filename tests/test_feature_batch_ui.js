/* Behavioral coverage for the 0.14 feature-batch browser controls.
 *
 * The real web/js/app.js is driven under a small DOM shim with a recording
 * api() so every control is exercised through its actual on-page handler, not
 * by grepping source text: LED idle/sleep + output/music diagnostics, nursery
 * sounds, USB Opus capability gating, the recent-voice ring (collection,
 * detail, clear, stale/generation ownership), the Simulation latency alias and
 * the network recovery panel (opt-in auto fallback, owner-prepared secret,
 * stop). web/js/setup.js is driven in its own context for the configured-device
 * captive recovery flow: sign-in then a Wi-Fi-only page, never account
 * creation. Escaped output is asserted too.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '..');
const failures = [];
function check(ok, message) {
    if (ok) { console.log('  ok: ' + message); return true; }
    failures.push(message);
    console.error('  FAIL: ' + message);
    return false;
}
function checkEqual(actual, expected, message) {
    return check(actual === expected, `${message} (expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)})`);
}
function has(haystack, needle, message) {
    return check(String(haystack).includes(needle), message);
}

/* ---------------------------------------------------------------- DOM shim */

function makeClassList() {
    const set = new Set();
    return {
        add: c => set.add(c), remove: c => set.delete(c),
        toggle: (c, on) => { const want = on === undefined ? !set.has(c) : !!on; want ? set.add(c) : set.delete(c); return want; },
        contains: c => set.has(c), _set: set
    };
}
function makeElement(id) {
    const el = {
        id, innerHTML: '', textContent: '', value: '', checked: false, disabled: false, hidden: false,
        type: '', title: '', required: false, isConnected: true,
        classList: makeClassList(), style: { setProperty() {} }, dataset: {},
        onclick: null, oninput: null, onchange: null, onsubmit: null, ontoggle: null,
        addEventListener() {}, appendChild() {}, remove() {}, focus() {}, scrollIntoView() {},
        setAttribute() {}, removeAttribute() {}, getAttribute() { return null; },
        querySelector: () => null, querySelectorAll: () => [],
        closest: () => null
    };
    el.parentElement = { querySelector: () => el };
    return el;
}

const elements = new Map();
const content = makeElement('content');
const nav = makeElement('nav');
const body = makeElement('body');
globalThis.document = {
    querySelector(s) {
        if (s === '#content') return content;
        if (s === '#nav') return nav;
        if (s === 'body') return body;
        if (s.startsWith('#')) {
            const id = s.slice(1);
            if (!elements.has(id)) elements.set(id, makeElement(id));
            return elements.get(id);
        }
        return makeElement(s);
    },
    querySelectorAll: () => [],
    createElement: tag => makeElement(tag),
    addEventListener() {}, body, activeElement: null
};
function $app(id) { return document.querySelector(id); }

globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device' };
globalThis.history = { pushState() {}, replaceState() {} };
function storage() {
    const m = new Map();
    return { getItem: k => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)),
        removeItem: k => m.delete(k), clear: () => m.clear(), _map: m };
}
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => true;
globalThis.prompt = () => 'secret';
globalThis.Blob = function (parts) { this.parts = parts; };
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };
/* Leave the dashboard startup fetch pending so no startup error view can
   overwrite the page under test; every request goes through api() below. */
globalThis.fetch = () => new Promise(() => {});

function runApp() {
    vm.runInThisContext('state', { filename: 'app.js' });
}
vm.runInThisContext(fs.readFileSync(path.join(ROOT, 'web/js/app.js'), 'utf8'), { filename: 'app.js' });
/* index.html loads these after app.js; load the real browser files so the page
   lifecycle handlers under test are the shipped ones. */
for (const file of ['web/js/bluetooth.js', 'web/js/integrations-ui.js', 'web/js/privacy-ui.js']) {
    vm.runInThisContext(fs.readFileSync(path.join(ROOT, file), 'utf8'), { filename: file });
}
const state = vm.runInThisContext('state');
const ledPage = vm.runInThisContext('ledPage');
const audioPage = vm.runInThisContext('audioPage');
const networkPage = vm.runInThisContext('networkPage');
const systemPage = vm.runInThisContext('systemPage');
const bindVoiceHistory = vm.runInThisContext('bindVoiceHistory');
const voiceHistoryLoad = vm.runInThisContext('voiceHistoryLoad');
const voiceHistoryDetail = vm.runInThisContext('voiceHistoryDetail');
const voiceHistoryClear = vm.runInThisContext('voiceHistoryClear');
const voiceTurnTime = vm.runInThisContext('voiceTurnTime');
const simHistoryLoad = vm.runInThisContext('simHistoryLoad');
const recoveryPanel = vm.runInThisContext('recoveryPanel');

/* ------------------------------------------------------------- api recorder */

const apiCalls = [];
let routes = {};
function defaultRoute(path, method) {
    if (path === '/led') return LED;
    if (path === '/buttons') return BUTTONS;
    if (path === '/audio') return AUDIO;
    if (path === '/network') return NETWORK;
    if (path === '/status') return { device_state: 'online', uptime_seconds: 1 };
    if (path === '/device') return { name: 'LibreEcho', hostname: 'libreecho', os_version: '0.14.0' };
    if (path === '/system/features') return {};
    if (path === '/system/update') return { supported: false };
    if (path === '/assistant/latency') return { turns: [], history_generation: 1 };
    if (path === '/assistant/history') return method === 'DELETE' ? {} : { turns: [], history_generation: 1 };
    if (path.startsWith('/storage/usb')) return USB;
    if (path === '/wake-word') return {};
    if (path === '/voice-pipeline') return {};
    return {};
}
globalThis.api = async (path, opt = {}) => {
    const method = (opt && opt.method) || 'GET';
    apiCalls.push({ path, method, body: opt && opt.body ? JSON.parse(opt.body) : null });
    if (routes[path]) {
        const r = routes[path];
        if (typeof r === 'function') return r(method, opt);
        return r;
    }
    return defaultRoute(path, method);
};
function callsFor(path, method) { return apiCalls.filter(c => c.path === path && (!method || c.method === method)); }
function lastBody(path, method) { const c = callsFor(path, method); return c.length ? c[c.length - 1].body : null; }

/* --------------------------------------------------------------- fixtures */

const LED = {
    idle_mode: 'indicator',
    sleep_light: { mode: 'pulse', active: true, brightness: 8, period_ms: 8000, timer_minutes: 120, remaining_ms: 600000, restore_on_boot: true },
    output: { effective_brightness: 12, frame_load: 900, max_load: 1000, limited: true, slew_limited: false },
    music: { active: true, grammar: 'pulse', effect: 'kaleidoscope', session: 7 },
    colour: { r: 10, g: 20, b: 30 }, brightness: 50, visualizer_enabled: true,
    pixels: Array.from({ length: 12 }, () => ({ r: 5, g: 6, b: 7 })),
    night: { enabled: false, active: false, start_minute: 1320, end_minute: 420 },
    profiles: {}
};
const BUTTONS = { short_press: 'Start listening', long_press: 'Open pairing mode', hardware_mute: true, action: 'sound', action_sounds: '', action_brightness: 70, mute_brightness: 60, tones: true };
const AUDIO = {
    volume: 40, notification_volume: 50, microphone_gain: 60, microphone_muted: false, startup_sound: true,
    amplifier_on: true, output_available: true, tts_voice: 'southern-female', tts_voices: [],
    noise: { active: false, source: 'brown', bed: 'none', tempo: 60, fade_seconds: 0, level: 40, remaining_seconds: -1 }
};
const NETWORK = {
    state: 'connected', connectivity: 'healthy', ssid: 'HomeNet', signal: 80, hostname: 'libreecho', dhcp: true,
    ip: '192.168.1.10', gateway: '192.168.1.1', dns: '192.168.1.1', internet: true, ssh: false,
    api_lan: false, api_lan_effective: false, api_lan_forced: false, gateway_reachable: true,
    recovery: {
        mode: 'off', trigger: 'none', available: true, ssid: 'LibreEcho-Setup-1234', reason: '', error: '',
        secret_available: false, led_owner: null, enabled: false, net_configured: true, auto_enabled: false,
        auto_timeout_ms: 120000, auto_pending: false, auto_countdown_ms: 0, rate_count: 0, children: 0,
        psk_path: '/data/libreecho/config/recovery-psk'
    }
};
const USB = {
    present: true, mounted: true, device: 'sda', partition: 'sda1', filesystem: 'vfat',
    size_bytes: 1000, used_bytes: 100, free_bytes: 900, rel_path: '', playable_formats: ['mp3'],
    entries: [{ name: 'song.mp3', directory: false, size_bytes: 10 }, { name: 'clip.opus', directory: false, size_bytes: 20 }]
};

function resetCalls() { apiCalls.length = 0; }
function resetDom() { elements.clear(); content.innerHTML = ''; }
async function settle(ms = 3) { await new Promise(r => setTimeout(r, ms)); }
function clearTimers() { clearTimeout(state.timer); clearTimeout(state.lampTimer); }

/* ------------------------------------------------------------------ cases */

async function caseLedControls() {
    resetDom(); resetCalls();
    state.page = 'LED & Buttons'; state.renderGeneration++;
    await ledPage();
    clearTimers();
    const html = content.innerHTML;
    has(html, 'Idle behaviour', 'LED page renders the idle-behaviour control');
    has(html, 'Sleep light', 'LED page renders the sleep-light control');
    has(html, 'Output limiting', 'LED page renders the output-limiting diagnostics');
    has(html, 'Music visualiser', 'LED page renders the music diagnostics');
    has(html, 'kaleidoscope', 'music diagnostics show the reported effect');
    check(!html.includes('effective_brightness'), 'output diagnostics render values, not raw field names');
    has(html, 'Frame limited', 'output limiting labels the frame-limited verdict');
    check(/<option value="indicator" selected>/.test(html), 'idle mode renders the current value selected');
    check(/<option value="pulse" selected>/.test(html), 'sleep mode renders the current value selected');

    $app('#led-idle-mode').value = 'always';
    await $app('#save-led-idle').onclick();
    clearTimers();
    checkEqual(JSON.stringify(lastBody('/led/idle', 'PUT')), JSON.stringify({ mode: 'always' }), 'saving idle behaviour PUTs the mode');

    $app('#led-sleep-mode').value = 'solid';
    $app('#led-sleep-brightness').value = '4';
    $app('#led-sleep-period').value = '5000';
    $app('#led-sleep-timer').value = '60';
    $app('#led-sleep-restore').checked = true;
    await $app('#save-led-sleep').onclick();
    clearTimers();
    checkEqual(JSON.stringify(lastBody('/led/sleep', 'PUT')),
        JSON.stringify({ mode: 'solid', brightness: 4, period_ms: 5000, timer_minutes: 60, restore_on_boot: true }),
        'saving the sleep light PUTs mode/brightness/period/timer/restore');

    /* Out-of-range brightness is rejected client-side, not silently clamped. */
    resetCalls();
    $app('#led-sleep-brightness').value = '25';
    await $app('#save-led-sleep').onclick();
    clearTimers();
    checkEqual(callsFor('/led/sleep', 'PUT').length, 0, 'an out-of-range sleep brightness is rejected without a request');
}

async function caseLedDiagnosticsEscaped() {
    resetDom(); resetCalls();
    routes['/led'] = Object.assign({}, LED, { music: { active: true, grammar: '<img src=x onerror=1>', effect: 'ok', session: 3 } });
    state.page = 'LED & Buttons'; state.renderGeneration++;
    await ledPage(); clearTimers();
    routes['/led'] = null;
    const html = content.innerHTML;
    has(html, '&lt;img src=x onerror=1&gt;', 'music grammar text is escaped in the diagnostics');
    check(!html.includes('<img src=x'), 'music grammar cannot inject markup');
}

async function caseNurserySounds() {
    resetDom(); resetCalls();
    state.page = 'Audio'; state.renderGeneration++;
    await audioPage(); clearTimers();
    const html = content.innerHTML;
    has(html, 'Nursery sounds', 'Audio page renders the nursery-sounds panel');
    has(html, 'Heartbeat tempo', 'nursery panel offers the heartbeat tempo field');
    has(html, 'id="noise-bed"', 'nursery panel offers a noise bed selector');
    has(html, 'id="noise-fade"', 'nursery panel offers a fade field');
    has(html, 'normal audio output at the master volume', 'nursery copy states the normal endpoint/volume behaviour');
    has(html, '<option value="heartbeat"', 'heartbeat source is selectable');

    $app('#noise-colour').value = 'heartbeat';
    $app('#noise-bed').value = 'brown';
    $app('#noise-level').value = '30';
    $app('#noise-minutes').value = '60';
    $app('#noise-tempo').value = '72';
    $app('#noise-fade').value = '120';
    await $app('#noise-start').onclick();
    clearTimers();
    const body = lastBody('/audio/noise', 'POST');
    checkEqual(JSON.stringify(body),
        JSON.stringify({ colour: 'heartbeat', source: 'heartbeat', bed: 'brown', level: 30, minutes: 60, tempo: 72, fade_seconds: 120 }),
        'starting nursery sounds POSTs source/bed/tempo/fade/timer/level');

    /* A fade longer than the timer is rejected before any request. */
    resetCalls();
    $app('#noise-minutes').value = '15';
    $app('#noise-fade').value = '3000';
    await $app('#noise-start').onclick();
    clearTimers();
    checkEqual(callsFor('/audio/noise', 'POST').length, 0, 'a fade longer than the timer is rejected client-side');

    checkEqual(typeof $app('#noise-stop').onclick, 'function', 'the Stop control is bound to a handler');
    checkEqual(state.busy, false, 'the page is not busy before Stop');
    await $app('#noise-stop').onclick();
    clearTimers();
    checkEqual(callsFor('/audio/noise', 'DELETE').length, 1, 'Stop DELETEs the nursery sounds endpoint');
}

async function caseUsbCapabilityGate() {
    const OTA = { supported: true, current_slot: 'a', inactive_slot: 'b', state: 'idle', progress: 0, channel: 'stable', latest_version: '0.14.0', automatic_updates: false, rollback_available: false, source_reachable: 'true' };
    routes['/system/update'] = OTA;
    /* Opus advertised: .opus gets a Play button, .mp3 too. */
    resetDom(); resetCalls();
    routes['/storage/usb'] = Object.assign({}, USB, { playable_formats: ['mp3', 'opus'] });
    state.data.status = { simulated: true };
    state.page = 'System'; state.renderGeneration++;
    $app('#feature-usb-host').checked = true;
    await systemPage(); clearTimers();
    $app('#feature-usb-host').checked = true;
    state.features = { usb_host: true };
    await systemPage(); clearTimers();
    has($app('#usb-storage').innerHTML, 'clip.opus', 'USB listing shows the opus file');
    check(/clip\.opus[\s\S]{0,80}Play/.test($app('#usb-storage').innerHTML),
        'with Opus advertised, a .opus file offers Play');
    has($app('#usb-storage').innerHTML, 'MP3, OPUS', 'the playable-formats line advertises MP3 and OPUS');

    /* Opus absent: .opus is listed but not playable; .mp3 still plays. */
    resetDom(); resetCalls();
    routes['/storage/usb'] = Object.assign({}, USB, { playable_formats: ['mp3'] });
    state.page = 'System'; state.renderGeneration++;
    $app('#feature-usb-host').checked = true;
    state.features = { usb_host: true };
    await systemPage(); clearTimers();
    const gate = $app('#usb-storage').innerHTML;
    check(/song\.mp3[\s\S]{0,80}Play/.test(gate), 'mp3 still offers Play without an Opus decoder');
    check(!/clip\.opus[\s\S]{0,80}Play/.test(gate), 'without Opus advertised, .opus offers no Play');
    has(gate, 'Opus decode is not available on this image', 'the .opus file explains why it is not playable');
    has(gate, 'MP3', 'the playable-formats line reports MP3');

    /* Missing capability defaults to mp3-only (never optimistically opus). */
    resetDom(); resetCalls();
    routes['/storage/usb'] = Object.assign({}, USB);
    delete routes['/storage/usb'].playable_formats;
    state.page = 'System'; state.renderGeneration++;
    $app('#feature-usb-host').checked = true;
    state.features = { usb_host: true };
    await systemPage(); clearTimers();
    check(!/clip\.opus[\s\S]{0,80}Play/.test($app('#usb-storage').innerHTML),
        'a missing capability is treated as mp3-only, never opus');
}

function voiceTurns() {
    return [
        { id: 6, at_ms: 6000, status: 'complete', preview: 'sixth' },
        { id: 12, at_ms: 12000, status: 'complete', preview: 'twelfth' },
        { id: 3, at_ms: 3000, status: 'complete', preview: 'third' },
        { id: 11, at_ms: 11000, status: 'error', preview: 'eleventh' },
        { id: 1, at_ms: 1000, status: 'complete', preview: 'first' },
        { id: 10, at_ms: 10000, status: 'complete', preview: 'tenth' },
        { id: 2, at_ms: 2000, status: 'complete', preview: 'second' },
        { id: 9, at_ms: 9000, status: 'complete', preview: 'ninth' },
        { id: 4, at_ms: 4000, status: 'complete', preview: 'fourth' },
        { id: 8, at_ms: 8000, status: 'complete', preview: 'eighth' },
        { id: 5, at_ms: 5000, status: 'complete', preview: 'fifth' },
        { id: 7, at_ms: 7000, status: 'complete', preview: 'seventh' }
    ];
}

async function caseRecentVoiceCollection() {
    resetDom(); resetCalls();
    $app('#voice-history').innerHTML = '';
    routes['/assistant/history'] = { history_generation: 4, capacity: 10, turns: voiceTurns() };
    bindVoiceHistory();
    await settle(8);
    const html = $app('#voice-history').innerHTML;
    const order = [...html.matchAll(/class="voice-turn" role="listitem" data-id="(\d+)"/g)].map(m => Number(m[1]));
    checkEqual(order.length, 10, 'the recent-voice ring is capped at 10 rows');
    checkEqual(order[0], 12, 'recent voice is newest-first');
    checkEqual(order[1], 11, 'newest-first ordering continues');
    check(html.indexOf('twelfth') < html.indexOf('sixth'), 'the newest preview precedes older previews');
    check(!/role="listitem" data-id="1"/.test(html), 'older turns beyond the cap are omitted');
    has(html, 'error', 'a failed turn renders its error status');
}

/* Canonical schema. voice_history.c serializes each turn's moment as an ISO
   8601 `timestamp` with a numeric offset (e.g. 2026-09-30T21:00:12+00:00), not
   the pre-0.14 latency `at_ms`. The panel must render that time, order by it,
   and never print "—" for a valid canonical record; a missing or unparseable
   value must still degrade to "—" rather than "Invalid Date"/NaN. */
function canonicalStamp(id) {
    return new Date(Date.UTC(2026, 8, 30, 21, 0, id)).toISOString().replace(/\.000Z$/, '+00:00');
}
function canonicalVoiceTurns() {
    return voiceTurns().map(t => ({
        id: t.id, timestamp: canonicalStamp(t.id), status: t.status,
        transcript_preview: t.preview, response_preview: '',
        transcript_truncated: false, response_truncated: false,
        transcript_length: 0, response_length: 0, stt_ms: 0, assistant_ms: 0, tts_ms: 0, error: null
    }));
}

async function caseRecentVoiceCanonicalTimestamp() {
    resetDom(); resetCalls();
    routes['/assistant/history'] = { history_generation: 7, capacity: 10, count: 12, preview_chars: 64, turns: canonicalVoiceTurns() };
    bindVoiceHistory();
    await settle(8);
    const html = $app('#voice-history').innerHTML;
    const order = [...html.matchAll(/class="voice-turn" role="listitem" data-id="(\d+)"/g)].map(m => Number(m[1]));
    checkEqual(order.length, 10, 'the canonical ring is capped at 10 rows');
    checkEqual(order[0], 12, 'canonical turns are ordered newest-first by their ISO timestamp');
    checkEqual(order[1], 11, 'canonical newest-first ordering continues');
    const times = [...html.matchAll(/<time>([^<]*)<\/time>/g)].map(m => m[1]);
    checkEqual(times.length, 10, 'every canonical row renders a <time>');
    check(times.every(v => v && v !== '—'), 'no canonical row shows a placeholder dash for a valid timestamp');
    checkEqual(times[0], new Date(canonicalStamp(12)).toLocaleTimeString(), 'the newest canonical row renders its ISO timestamp');
    /* transcript_preview (not the legacy `preview`) is the canonical text field. */
    has(html, 'twelfth', 'the canonical transcript_preview is rendered');
    check(!/—/.test(html), 'no placeholder dash leaks into the canonical list');

    /* Safe fallback: missing, empty or unparseable times degrade to "—". */
    checkEqual(voiceTurnTime({}), '—', 'a turn with no time field falls back to a dash');
    checkEqual(voiceTurnTime({ timestamp: '' }), '—', 'an empty timestamp falls back to a dash');
    checkEqual(voiceTurnTime({ timestamp: 'not-a-date' }), '—', 'an unparseable timestamp falls back to a dash');
    checkEqual(voiceTurnTime({ timestamp: '1970-01-01T00:00:00+00:00' }), '—', 'the device epoch fallback stamp still reads as unknown');

    /* Legacy at_ms/at is preserved for the pre-0.14 shape. */
    checkEqual(voiceTurnTime({ at_ms: 9000 }), new Date(9000).toLocaleTimeString(), 'legacy at_ms still renders its time');
    checkEqual(voiceTurnTime({ at: 12000 }), new Date(12000).toLocaleTimeString(), 'legacy at still renders its time');
    checkEqual(voiceTurnTime({ at_ms: 9000, timestamp: canonicalStamp(12) }), new Date(canonicalStamp(12)).toLocaleTimeString(),
        'the canonical timestamp wins over a stale legacy at_ms');
}

async function caseRecentVoiceLegacyOrderingPreserved() {
    resetDom(); resetCalls();
    routes['/assistant/history'] = { history_generation: 4, turns: voiceTurns() };
    bindVoiceHistory();
    await settle(8);
    const html = $app('#voice-history').innerHTML;
    const order = [...html.matchAll(/class="voice-turn" role="listitem" data-id="(\d+)"/g)].map(m => Number(m[1]));
    checkEqual(order[0], 12, 'legacy at_ms fixtures still order newest-first');
    check(!/—/.test(html), 'legacy at_ms fixtures still render a time, not a dash');
}

async function caseRecentVoiceDetailEscaped() {
    resetDom();
    const body = makeElement('detail-body'); body.innerHTML = ''; body.hidden = true;
    const box = makeElement('turn'); box.querySelector = sel => sel === '.voice-detail-body' ? body : null;
    const btn = makeElement('detail'); btn.closest = () => box; btn.dataset = { id: '11' };
    routes['/assistant/history/11'] = { id: 11, transcript: '<b>hello</b>', response: 'hi & bye', status: 'complete' };
    await voiceHistoryDetail('11', btn);
    has(body.innerHTML, '&lt;b&gt;hello&lt;/b&gt;', 'the transcript is escaped when opened');
    has(body.innerHTML, 'hi &amp; bye', 'the response is escaped when opened');
    checkEqual(callsFor('/assistant/history/11').length, 1, 'opening a turn fetches its detail endpoint');
}

async function caseRecentVoiceStaleDetail() {
    resetDom();
    const body = makeElement('detail-body'); body.hidden = true; body.isConnected = true;
    const box = makeElement('turn'); box.querySelector = () => body;
    const btn = makeElement('detail'); btn.closest = () => box; btn.dataset = { id: '5' };
    let release;
    routes['/assistant/history/5'] = () => new Promise(resolve => { release = resolve; });
    const pending = voiceHistoryDetail('5', btn);
    await settle();
    /* Clearing bumps the generation: the in-flight detail must not paint. */
    await voiceHistoryClear();
    release({ id: 5, transcript: 'stale transcript', response: 'stale', status: 'complete' });
    await pending;
    check(!body.innerHTML.includes('stale transcript'), 'a detail response superseded by clear does not overwrite the list');
}

async function caseRecentVoiceLoadRace() {
    resetDom(); resetCalls();
    const box = $app('#voice-history');
    let release;
    routes['/assistant/history'] = () => new Promise(resolve => { release = resolve; });
    const first = voiceHistoryLoad();
    await settle();
    routes['/assistant/history'] = { history_generation: 9, turns: [{ id: 99, at_ms: 9000, status: 'complete', preview: 'fresh' }] };
    const second = voiceHistoryLoad();
    await settle(8);
    check(box.innerHTML.includes('fresh'), 'the newer collection renders');
    release({ history_generation: 8, turns: [{ id: 1, at_ms: 1000, status: 'complete', preview: 'stale-old' }] });
    await first; await second;
    check(!box.innerHTML.includes('stale-old'), 'a stale collection response cannot overwrite the newer list');
}

async function caseSimulationLatencyAlias() {
    resetDom(); resetCalls();
    routes['/assistant/latency'] = { history_generation: 5, turns: [{ at_ms: 1000, first_pcm_ms: 250 }] };
    localStorage.removeItem('libreecho-simulation-device-history');
    await simHistoryLoad();
    checkEqual(callsFor('/assistant/latency').length, 1, 'the Simulation history reads the new latency alias');
    checkEqual(callsFor('/assistant/history', 'GET').length, 0, 'the Simulation history no longer reads the voice collection');
}

async function caseNetworkRecovery() {
    resetDom(); resetCalls();
    state.page = 'Network'; state.renderGeneration++;
    await networkPage(); clearTimers();
    const html = content.innerHTML;
    has(html, 'Recovery access point', 'Network page renders the recovery panel');
    has(html, 'Automatic fallback', 'recovery panel shows the automatic-fallback setting');
    has(html, 'LibreEcho-Setup-1234', 'recovery panel shows the recovery SSID');
    has(html, 'opt-in', 'the automatic fallback is described as opt-in');
    checkEqual($app('#recovery-auto').checked, false, 'automatic fallback defaults off');
    has(html, 'briefly disconnects', 'the panel explains the AP to STA handover disconnect');

    $app('#recovery-enabled').checked = true;
    $app('#recovery-auto').checked = true;
    $app('#recovery-timeout').value = '300';
    await $app('#save-recovery').onclick();
    clearTimers();
    checkEqual(JSON.stringify(lastBody('/network/recovery', 'PUT')),
        JSON.stringify({ enabled: true, auto_enabled: true, timeout_seconds: 300 }),
        'saving recovery PUTs enabled/auto_enabled/timeout_seconds');

    resetCalls();
    $app('#recovery-timeout').value = '5';
    await $app('#save-recovery').onclick();
    clearTimers();
    checkEqual(callsFor('/network/recovery', 'PUT').length, 0, 'an out-of-range recovery timeout is rejected');
}

async function caseRecoverySecret() {
    resetDom(); resetCalls();
    state.page = 'Network'; state.renderGeneration++;
    await networkPage(); clearTimers();
    const before = JSON.stringify([...localStorage._map.entries()]) + JSON.stringify([...sessionStorage._map.entries()]);
    routes['/network/recovery/prepare'] = { ssid: 'LibreEcho-Setup-1234', psk: 'abc<X>def' };
    await $app('#recovery-prepare').onclick();
    const box = $app('#recovery-secret');
    checkEqual(box.hidden, false, 'preparing reveals the secret region');
    has(box.innerHTML, 'LibreEcho-Setup-1234', 'the prepared SSID is shown');
    has(box.innerHTML, 'abc&lt;X&gt;def', 'the recovery password is escaped when shown');
    has(box.innerHTML, 'shown only once', 'the secret warns that it is shown only once');
    checkEqual(callsFor('/network/recovery/prepare', 'POST').length, 1, 'Prepare POSTs the recovery prepare endpoint');
    const after = JSON.stringify([...localStorage._map.entries()]) + JSON.stringify([...sessionStorage._map.entries()]);
    checkEqual(after, before, 'the recovery password is never written to browser storage');
    check(!after.includes('abc<X>def') && !after.includes('abc&lt;X&gt;def'), 'the recovery password never reaches storage');

    await $app('#recovery-stop').onclick();
    clearTimers();
    checkEqual(callsFor('/network/recovery/stop', 'POST').length, 1, 'Stop POSTs the recovery stop endpoint');
}

/* ------------------------------------------- 0.14 save-wiring / mode regressions */

async function caseSaveControlsBindDirty() {
    /* ledBasePanels and recoveryPanel render their Save buttons disabled, so
       bindLedBase/bindRecovery must wire each control through bindDirty or the
       button can never become usable. This is the wiring defect the real
       browser suite catches by clicking; here the wiring itself is asserted. */
    resetDom(); resetCalls();
    const real = vm.runInThisContext('bindDirty');
    const wired = [];
    globalThis.bindDirty = (ids, button) => { wired.push({ ids: [...ids], button }); return real(ids, button); };
    try {
        state.page = 'LED & Buttons'; state.renderGeneration++;
        await ledPage(); clearTimers();
        check(wired.some(w => w.button === '#save-led-idle' && w.ids.includes('#led-idle-mode')),
            'the idle-behaviour Save button is wired through bindDirty');
        check(wired.some(w => w.button === '#save-led-sleep' &&
            ['#led-sleep-mode', '#led-sleep-brightness', '#led-sleep-period', '#led-sleep-timer', '#led-sleep-restore'].every(id => w.ids.includes(id))),
            'the sleep-light Save button is wired through bindDirty for all five controls');

        resetDom(); resetCalls(); wired.length = 0;
        state.page = 'Network'; state.renderGeneration++;
        await networkPage(); clearTimers();
        check(wired.some(w => w.button === '#save-recovery' &&
            ['#recovery-enabled', '#recovery-auto', '#recovery-timeout'].every(id => w.ids.includes(id))),
            'the recovery settings Save button is wired through bindDirty');
    } finally {
        globalThis.bindDirty = real;
    }
}

async function caseRecoveryModeDisplay() {
    const base = Object.assign({}, NETWORK.recovery);
    const htmlFor = mode => recoveryPanel({ recovery: Object.assign({}, base, { mode }) });
    const active = htmlFor('recovery-ap');
    check(/data-recovery-mode="recovery-ap"/.test(active) && /status-dot ok/.test(active),
        'recovery-ap is the only mode shown as an active (ok) access point');
    check(active.includes('Recovery access point is active'), 'recovery-ap shows its serving label');
    for (const mode of ['client', 'armed', 'stopped', 'stopping', 'unavailable']) {
        const html = htmlFor(mode);
        check(/status-dot warn/.test(html) && !/status-dot ok/.test(html),
            `${mode} is not shown as an active recovery access point`);
    }
    const handover = htmlFor('handover');
    check(handover.includes('Handover') && /status-dot warn/.test(handover),
        'handover gets its own label and is not shown as an active access point');
}

/* ---------------------------------------------------------- setup.js (captive) */

function setupSandbox(config, search = '', options = {}) {
    const store = {};
    if (options.token) store['libreecho-token'] = options.token;
    const unauthorized = new Set(options.unauthorized || []);
    const setupCalls = [];
    function classes() { const s = new Set(); return { add: c => s.add(c), remove: c => s.delete(c), toggle: (c, on) => on ? s.add(c) : s.delete(c), contains: c => s.has(c), _set: s }; }
    function el(id) { return { id, innerHTML: '', textContent: '', value: '', checked: false, disabled: false, hidden: id === 'recovery-login' || id === 'recovery-network', type: '', classList: classes(), dataset: {}, style: {}, scrollIntoView() {}, addEventListener() {}, focus() {}, onclick: null, onchange: null, oninput: null }; }
    const map = new Map();
    const step0 = el('step0'); step0.dataset = { step: '0' };
    const step1 = el('step1'); step1.dataset = { step: '1' };
    const pages = [step0, step1, el('recovery-login'), el('recovery-network')];
    pages.forEach(p => map.set(p.id, p));
    const document = {
        querySelector(s) { const id = s.slice(1); if (!map.has(id)) map.set(id, el(id)); return map.get(id); },
        querySelectorAll(s) {
            if (s === '.setup-page') return pages;
            if (s === '.wifi-option') return [];
            return [];
        },
        createElement: el, body: el('body'), addEventListener() {}
    };
    const sessionStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); }, removeItem: k => { delete store[k]; } };
    const sandbox = {
        document, sessionStorage, location: { replaced: [], replace(u) { this.replaced.push(u); }, href: '/', search, hash: '' }, history: {},
        console, setTimeout: (fn) => setTimeout(fn, 0), clearTimeout, Promise, JSON, Math, Number, String, Array, Object, Date, RegExp, Error, isNaN,
        confirm: () => true,
        fetch: async (url, opts = {}) => {
            const p = String(url).replace('/api/v1', '');
            setupCalls.push({ path: p, method: opts.method || 'GET', body: opts.body ? JSON.parse(opts.body) : null });
            if (unauthorized.has(p)) return { ok: false, status: 401, json: async () => ({ ok: false, data: null, error: { code: 'auth_required', message: 'Authentication is required' } }) };
            let data = {};
            if (p === '/config') data = Object.assign({ csrf_token: 'csrf-x', os_version: '0.14.0' }, config);
            else if (p === '/network/wifi/scan') data = { networks: [{ ssid: 'HomeNet', security: 'wpa2', signal: 80, capabilities: 'WPA2' }] };
            else if (p === '/auth') data = { authenticated: true, username: 'owner' };
            else if (p === '/auth/login') data = { token: 'tok-recovery', username: 'owner' };
            return { ok: true, status: 200, json: async () => ({ ok: true, data }) };
        }
    };
    sandbox.window = sandbox; sandbox.globalThis = sandbox;
    vm.createContext(sandbox);
    return { sandbox, setupCalls, map, store };
}
async function runSetup(config, search, options) {
    const ctx = setupSandbox(config, search || '', options || {});
    vm.runInContext(fs.readFileSync(path.join(ROOT, 'web/js/setup.js'), 'utf8'), ctx.sandbox, { filename: 'setup.js' });
    await settle(8);
    return ctx;
}

async function caseCaptiveRecoveryConfigured() {
    const ctx = await runSetup({ bootstrap_required: false, authentication: 'users' }, '?recovery=1');
    const setupCalls = ctx.setupCalls;
    const q = sel => ctx.sandbox.document.querySelector(sel);
    checkEqual(q('#recovery-login').hidden, false, 'a configured device shows the recovery sign-in page');
    checkEqual(q('#recovery-network').hidden, true, 'the Wi-Fi page is hidden until signed in');
    checkEqual(q('#setup-actions').hidden, true, 'the wizard footer is hidden in recovery');
    checkEqual(setupCalls.filter(c => c.path === '/auth/bootstrap').length, 0, 'the recovery path never calls account creation');
    checkEqual(q('#step0').classList.contains('active'), false, 'the one-time account step is never activated');
    checkEqual(setupCalls.filter(c => c.path === '/setup').length, 0, 'the recovery path does not run the setup wizard');

    /* Sign in, then the Wi-Fi-only page appears. */
    q('#recovery-username').value = 'owner';
    q('#recovery-password').value = 'correct horse';
    await q('#recovery-signin').onclick();
    checkEqual(setupCalls.filter(c => c.path === '/auth/login').length, 1, 'sign-in POSTs to the login endpoint');
    checkEqual(q('#recovery-login').hidden, true, 'the sign-in page is hidden after signing in');
    checkEqual(q('#recovery-network').hidden, false, 'the Wi-Fi-only recovery page is shown after signing in');
    checkEqual(setupCalls.filter(c => c.path === '/auth/bootstrap').length, 0, 'no account is created on sign-in');

    q('#recovery-ssid').value = 'HomeNet';
    q('#recovery-security').value = 'wpa2';
    q('#recovery-wifi-password').value = 'super-secret';
    await q('#recovery-connect').onclick();
    const connect = setupCalls.filter(c => c.path === '/network/wifi/connect');
    checkEqual(connect.length, 1, 'recovery connect POSTs the Wi-Fi credentials');
    checkEqual(JSON.stringify(connect[0].body), JSON.stringify({ ssid: 'HomeNet', password: 'super-secret', security: 'wpa2' }), 'the connect body carries the chosen network');
    has(q('#recovery-status').textContent, 'disconnect', 'the page warns the phone will disconnect during the handover');
}

async function caseFirstBootWizardUnchanged() {
    const ctx = await runSetup({ bootstrap_required: true, authentication: 'bootstrap-required' });
    const q = sel => ctx.sandbox.document.querySelector(sel);
    checkEqual(q('#step0').classList.contains('active'), true, 'an unconfigured device still starts the first-boot wizard');
    checkEqual(q('#recovery-login').hidden, true, 'the recovery sign-in is not shown on first boot');
    checkEqual(ctx.setupCalls.filter(c => c.path === '/auth/login').length, 0, 'first boot does not try to sign in');
}

async function caseConfiguredPlainVisitRedirectsToLogin() {
    const ctx = await runSetup({ bootstrap_required: false, authentication: 'users' });
    const q = sel => ctx.sandbox.document.querySelector(sel);
    checkEqual(ctx.sandbox.location.replaced.includes('/login'), true, 'a plain configured visit still routes to the login page');
    checkEqual(q('#recovery-login').hidden, true, 'the recovery sign-in is not shown without the recovery hint');
    checkEqual(ctx.setupCalls.filter(c => c.path === '/auth/bootstrap').length, 0, 'a configured device never recreates the account');
    checkEqual(ctx.setupCalls.filter(c => c.path === '/setup').length, 0, 'a configured device is not sent through the setup wizard');
}

/* Regression (Codex review on 5660cc2): the captive recovery flow trusted any
 * sessionStorage token by truthiness, so an expired/revoked session skipped
 * sign-in and the owner landed on a Wi-Fi page whose requests all 401'd. The
 * stored token is now validated against /api/v1/auth, and any 401 from the
 * recovery scan or connect returns to sign-in with the token cleared. */

async function caseRecoveryExpiredSessionShowsSignin() {
    const ctx = await runSetup({ bootstrap_required: false, authentication: 'users' }, '?recovery=1',
        { token: 'expired-token', unauthorized: ['/auth'] });
    const q = sel => ctx.sandbox.document.querySelector(sel);
    checkEqual(ctx.setupCalls.filter(c => c.path === '/auth').length, 1, 'the stored recovery token is validated against the auth endpoint');
    checkEqual(q('#recovery-login').hidden, false, 'an expired recovery session shows the sign-in page');
    checkEqual(q('#recovery-network').hidden, true, 'the Wi-Fi page is hidden for an expired session');
    check(!('libreecho-token' in ctx.store), 'the expired token is cleared from sessionStorage');
    checkEqual(ctx.setupCalls.filter(c => c.path === '/network/wifi/scan').length, 0, 'the Wi-Fi page is not reached with an expired session');
    checkEqual(ctx.setupCalls.filter(c => c.path === '/auth/bootstrap').length, 0, 'no account is created for an expired session');
}

async function caseRecoveryScanUnauthorizedReturnsToSignin() {
    const ctx = await runSetup({ bootstrap_required: false, authentication: 'users' }, '?recovery=1',
        { token: 'live-token', unauthorized: ['/network/wifi/scan'] });
    const q = sel => ctx.sandbox.document.querySelector(sel);
    checkEqual(ctx.setupCalls.filter(c => c.path === '/auth').length, 1, 'a live token is validated before the Wi-Fi page');
    checkEqual(ctx.setupCalls.filter(c => c.path === '/network/wifi/scan').length, 1, 'the recovery scan is attempted for a live session');
    checkEqual(q('#recovery-login').hidden, false, 'a 401 from the recovery scan returns to sign-in');
    checkEqual(q('#recovery-network').hidden, true, 'the Wi-Fi page is hidden after a scan 401');
    check(!('libreecho-token' in ctx.store), 'the rejected token is cleared after a scan 401');
}

async function caseRecoveryConnectUnauthorizedReturnsToSignin() {
    const ctx = await runSetup({ bootstrap_required: false, authentication: 'users' }, '?recovery=1',
        { token: 'live-token', unauthorized: ['/network/wifi/connect'] });
    const q = sel => ctx.sandbox.document.querySelector(sel);
    checkEqual(q('#recovery-network').hidden, false, 'a live session reaches the Wi-Fi page');
    q('#recovery-ssid').value = 'HomeNet';
    q('#recovery-security').value = 'wpa2';
    q('#recovery-wifi-password').value = 'super-secret';
    await q('#recovery-connect').onclick();
    checkEqual(ctx.setupCalls.filter(c => c.path === '/network/wifi/connect').length, 1, 'the connect is attempted for a live session');
    checkEqual(q('#recovery-login').hidden, false, 'a 401 from the recovery connect returns to sign-in');
    check(!('libreecho-token' in ctx.store), 'the rejected token is cleared after a connect 401');
}

/* ------------------------------------------------------------------- main */

async function main() {
    const watchdog = setTimeout(() => { console.error('feature batch UI: timed out'); process.exit(1); }, 30000);
    const cases = [
        ['LED idle/sleep controls and diagnostics', caseLedControls],
        ['LED diagnostics escape untrusted text', caseLedDiagnosticsEscaped],
        ['nursery sounds source/bed/tempo/fade/timer', caseNurserySounds],
        ['USB .opus gated by advertised capability', caseUsbCapabilityGate],
        ['recent voice newest-first cap of 10', caseRecentVoiceCollection],
        ['recent voice canonical ISO timestamp rendering', caseRecentVoiceCanonicalTimestamp],
        ['recent voice legacy at_ms ordering preserved', caseRecentVoiceLegacyOrderingPreserved],
        ['recent voice detail fetch and escaping', caseRecentVoiceDetailEscaped],
        ['recent voice stale detail after clear', caseRecentVoiceStaleDetail],
        ['recent voice stale collection race', caseRecentVoiceLoadRace],
        ['Simulation history uses the latency alias', caseSimulationLatencyAlias],
        ['network recovery save/validate', caseNetworkRecovery],
        ['network recovery owner-prepared secret', caseRecoverySecret],
        ['LED and recovery Save controls are wired through bindDirty', caseSaveControlsBindDirty],
        ['recovery panel reflects actual networkd modes', caseRecoveryModeDisplay],
        ['configured device captive recovery', caseCaptiveRecoveryConfigured],
        ['configured device plain visit redirects to login', caseConfiguredPlainVisitRedirectsToLogin],
        ['recovery expired session returns to sign-in', caseRecoveryExpiredSessionShowsSignin],
        ['recovery scan 401 returns to sign-in', caseRecoveryScanUnauthorizedReturnsToSignin],
        ['recovery connect 401 returns to sign-in', caseRecoveryConnectUnauthorizedReturnsToSignin],
        ['first-boot wizard unchanged', caseFirstBootWizardUnchanged]
    ];
    for (const [name, run] of cases) {
        console.log('case: ' + name);
        try { await run(); } catch (error) {
            failures.push(`${name}: ${error && error.stack ? error.stack : error}`);
            console.error('  FAIL: ' + name + ' threw: ' + (error && error.message ? error.message : error));
        }
    }
    clearTimeout(watchdog);
    clearTimers();
    if (failures.length) {
        console.error(`feature batch UI: ${failures.length} check(s) failed`);
        process.exit(1);
    }
    console.log('feature batch UI: ok');
    process.exit(0);
}
main();
