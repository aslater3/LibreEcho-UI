/*
 * Behavioural cover for browser code that is worth running rather than grepping:
 * the Simulation page's radio barge-in pair, and the System page's update-size
 * row.
 *
 * web/js/app.js is a browser script with no exports, so it is loaded whole
 * into this process with a minimal DOM shim: top-level function declarations
 * land on globalThis, which is enough to drive simRadioStart and simRadioStop
 * against a fake device. That is deliberately the real page code rather than a
 * copy -- a copy would keep passing after the page stopped matching it.
 *
 * What this proves: the pair's control flow, its verdicts and its cleanup.
 * What it cannot prove: anything about the device. The fake answers instantly
 * and always agrees with itself; a real radiod does neither.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

/* ---- DOM shim -------------------------------------------------------- */
function stub() {
    const target = function () { return stub(); };
    return new Proxy(target, {
        get(_, k) {
            if (k === 'then') return undefined;              /* not thenable */
            if (k === Symbol.iterator) return function* () {};
            if (k === Symbol.toPrimitive) return () => '';
            return stub();
        },
        set() { return true; },
        apply() { return stub(); },
        has() { return true; }
    });
}
function storage() {
    const m = new Map();
    return {
        getItem: k => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => m.set(k, String(v)),
        removeItem: k => m.delete(k),
        clear: () => m.clear()
    };
}
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.document = {
    querySelector: () => stub(),
    querySelectorAll: () => [],
    createElement: () => stub(),
    addEventListener: () => {},
    body: stub(),
    title: ''
};
globalThis.window = { addEventListener: () => {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
globalThis.confirm = () => false;
globalThis.prompt = () => '';
globalThis.URL.createObjectURL = () => 'blob:fake';
globalThis.URL.revokeObjectURL = () => {};
/* The page fires api('/config') at load; let it fail, it is caught there. */
globalThis.fetch = () => Promise.reject(new Error('no device in this harness'));

vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const state = vm.runInThisContext('state');
const SIM_PHRASES = vm.runInThisContext('SIM_PHRASES');

/* ---- virtual clock ---------------------------------------------------- */
/* Every sleep in the page is `new Promise(r=>setTimeout(r,ms))`. Advancing a
   counter instead of waiting keeps the suite fast; it also means the durations
   asserted below are ordering facts, not measurements. */
let clock = 0;
globalThis.setTimeout = (fn, d) => { clock += Number(d) || 0; queueMicrotask(fn); return 0; };
globalThis.performance = { now: () => clock };

/* ---- fake device ------------------------------------------------------ */
function device(opt) {
    const d = {
        supported: opt.supported !== false,
        stations: opt.stations !== undefined ? opt.stations
            : [{ word: 'groove', name: 'Groove Salad', url: 'http://example/g', enabled: true }],
        playing: !!opt.playing,
        amplifier_on: !!opt.playing,
        voiceStops: !!opt.voiceStops,        /* does the spoken stop reach radiod? */
        simulateFails: !!opt.simulateFails,
        wake: 0,
        spoken: false,
        pending: -1,
        calls: []
    };
    d.api = async (path, o) => {
        const method = (o && o.method) || 'GET';
        d.calls.push(method + ' ' + path + (o && o.body ? ' ' + o.body : ''));
        /* The turn's log lines only exist once the phrase has been sent, so
           simRun's before/after mark actually selects something. */
        if (path === '/logs')
            return { entries: d.spoken ? [
                { boot_seconds: 10, level: 'info', message: 'Simulated audio queued' },
                { boot_seconds: 12, level: 'info', message: 'audio_ms=1800 processing_ms=2900 total_ms=4700 text_chars=31' },
                { boot_seconds: 13, level: 'info', message: 'ttsd streaming first PCM chunk' }] : [] };
        if (path === '/wake-word') return { detected_count: d.wake };
        if (path === '/audio/simulate') {
            if (d.simulateFails) throw new Error('The device is already playing a phrase');
            d.wake += 1; d.spoken = true;
            if (d.voiceStops && d.playing) d.pending = 2;   /* stops a couple of polls later */
            return {};
        }
        if (path === '/audio') return { amplifier_on: d.amplifier_on };
        if (path === '/integrations/radio') {
            if (d.pending > 0) d.pending -= 1;
            else if (d.pending === 0) { d.pending = -1; d.playing = false; d.amplifier_on = false; }
            return { max_stations: 32, playback_supported: d.supported,
                     playing: d.playing, playing_url: d.playing ? 'http://example/g' : '',
                     stations: d.stations };
        }
        if (path === '/integrations/radio/play') {
            if (!d.supported) throw new Error('Stream playback is not available on this image');
            d.playing = true; d.amplifier_on = true; return {};
        }
        if (path === '/integrations/radio/stop') { d.playing = false; d.amplifier_on = false; return {}; }
        throw new Error('unexpected path ' + path);
    };
    return d;
}
function install(d) {
    globalThis.api = d.api;
    globalThis.localStorage.clear();
    state.simRadio = undefined;
    return d;
}

/* ---- assertions ------------------------------------------------------- */
let failures = 0;
function check(name, cond, detail) {
    if (cond) { console.log('  ok   ' + name); return; }
    failures += 1;
    console.log('  FAIL ' + name + (detail === undefined ? '' : ' — ' + JSON.stringify(detail)));
}
const history = () => JSON.parse(globalThis.localStorage.getItem('libreecho-simulation-history') || '[]');
const posted = (d, path) => d.calls.some(c => c.startsWith('POST ' + path));

async function main() {
    console.log('web ui behaviour harness');
    console.log('-- simulation radio barge-in pair --');

    /* The two halves are an ordered pair or they are nothing. */
    const labels = SIM_PHRASES.map(p => p[0]);
    const i = labels.indexOf('Play the radio');
    check('start half is present', i >= 0);
    check('stop half follows it immediately', labels[i + 1] === 'Stop the radio while it plays', labels.slice(i));
    check('the pair runs last', i + 2 === SIM_PHRASES.length, labels);
    check('start half says "Alexa, play the radio"', SIM_PHRASES[i][1] === 'Alexa, play the radio');
    check('stop half says "Alexa, stop"', SIM_PHRASES[i + 1][1] === 'Alexa, stop');
    check('start half carries its own runner', SIM_PHRASES[i][2] === globalThis.simRadioStart);
    check('stop half carries its own runner', SIM_PHRASES[i + 1][2] === globalThis.simRadioStop);

    /* 1. Start: the spoken phrase does not reach radiod, so the API does. */
    let d = install(device({}));
    let e = await globalThis.simRadioStart('Alexa, play the radio', 12000);
    check('start leaves the stream playing', e.radio_playing === true, e);
    check('start records who started it', e.radio_started_by === 'api', e.radio_started_by);
    check('start names the default station', e.radio_station === 'groove', e.radio_station);
    check('start asked radiod for that word', posted(d, '/integrations/radio/play') &&
          d.calls.some(c => c.includes('"word":"groove"')), d.calls);
    check('start still records the turn metrics', e.wake === true && e.processing_ms === 2900, e);
    check('start wrote exactly one history row', history().length === 1, history().length);

    /* 1b. A stream someone left playing is not credited to the voice turn. */
    d = install(device({ playing: true }));
    e = await globalThis.simRadioStart('Alexa, play the radio', 12000);
    check('pre-existing playback is not credited to voice',
          e.radio_started_by === 'already playing', e.radio_started_by);
    check('pre-existing playback asks radiod for nothing',
          !posted(d, '/integrations/radio/play'), d.calls);

    /* 2. Stop, on a device where the voice path does reach radiod. */
    d = install(device({ playing: true, voiceStops: true }));
    e = await globalThis.simRadioStop('Alexa, stop', 12000);
    check('voice stop is recorded as a pass', e.voice_stopped === true, e);
    check('voice stop attributes the stop to voice', e.radio_stopped_by === 'voice', e.radio_stopped_by);
    check('voice stop times the stop', Number.isFinite(e.radio_stop_ms) && e.radio_stop_ms >= 0, e.radio_stop_ms);
    check('voice stop times the amplifier too', Number.isFinite(e.amplifier_off_ms), e.amplifier_off_ms);
    check('voice stop consulted the amplifier', d.calls.includes('GET /audio'), d.calls.slice(0, 6));
    check('voice stop leaves nothing playing', e.radio_playing_after === false && d.playing === false, e);
    check('voice stop still ran the cleanup', posted(d, '/integrations/radio/stop'), d.calls);
    check('voice stop wrote exactly one history row', history().length === 1, history().length);

    /* 3. Stop, on this image: agentd has no radio path, so nothing stops. */
    d = install(device({ playing: true, voiceStops: false }));
    e = await globalThis.simRadioStop('Alexa, stop', 12000);
    check('unheard stop is recorded as a failure', e.voice_stopped === false, e);
    check('unheard stop says the stream kept playing',
          e.radio_status === 'still playing after the turn', e.radio_status);
    check('unheard stop reports no timing', e.radio_stop_ms === null, e.radio_stop_ms);
    check('unheard stop is cleaned up anyway', d.playing === false && posted(d, '/integrations/radio/stop'), d.calls);
    check('unheard stop attributes the stop to cleanup', e.radio_stopped_by === 'API cleanup', e.radio_stopped_by);

    /* 4. Stop with nothing playing is not a pass, and not barge-in either. */
    d = install(device({ playing: false }));
    e = await globalThis.simRadioStop('Alexa, stop', 12000);
    check('stop with no stream is skipped, not passed', e.voice_stopped === null, e);
    check('stop with no stream says so', /skipped/.test(e.radio_status), e.radio_status);

    /* 5. An image with no stream player is reported, not silently passed. */
    d = install(device({ supported: false }));
    e = await globalThis.simRadioStart('Alexa, play the radio', 12000);
    check('unsupported playback is reported', e.radio_status === 'no stream player on this image', e.radio_status);
    check('unsupported playback is not a pass', e.radio_playing === false, e);
    check('unsupported playback asks radiod for nothing', !posted(d, '/integrations/radio/play'), d.calls);

    /* 6. A configured-but-empty list is reported rather than crashing. */
    d = install(device({ stations: [] }));
    e = await globalThis.simRadioStart('Alexa, play the radio', 12000);
    check('empty station list is reported', e.radio_status === 'no station configured', e.radio_status);

    /* 7. The cleanup is in a finally: a turn that throws still leaves it off. */
    d = install(device({ playing: true, simulateFails: true }));
    let threw = false;
    try { await globalThis.simRadioStop('Alexa, stop', 12000); } catch (_) { threw = true; }
    check('a failing turn still propagates', threw);
    check('a failing turn still stops the radio', d.playing === false && posted(d, '/integrations/radio/stop'), d.calls);

    /* ---- Radio playback must use the persisted complete row -------------- */
    const oldRadioQuerySelector = document.querySelector;
    const oldRadioQuerySelectorAll = document.querySelectorAll;
    const oldRadioApi = globalThis.api;
    const radioHandlers = {};
    const radioCount = { textContent: '' };
    const radioNow = { textContent: '', set hidden(_) {} };
    const radioStop = { hidden: true };
    const radioAdd = {};
    const radioSave = { disabled: true };
    const radioToast = {
        textContent: '',
        classList: { toggle() {}, add() {}, remove() {} }
    };
    const radioInputs = {
        '.radio-word': { value: 'groove' },
        '.radio-name': { value: 'Groove Salad' },
        '.radio-url': { value: 'http://example/changed' },
        '.radio-enabled': { checked: true },
        '.radio-row-error': { textContent: '', hidden: true }
    };
    const radioRow = {
        classList: { toggle() {} },
        querySelector: selector => radioInputs[selector]
    };
    const radioPlay = {
        closest: selector => selector === '.radio-play' ? radioPlay
            : selector === '[data-radio-row]' ? radioRow : null
    };
    const radioList = {
        querySelectorAll: selector => selector === '[data-radio-row]' ? [radioRow] : [],
        addEventListener: (type, handler) => { radioHandlers[type] = handler; }
    };
    document.querySelector = selector => ({
        '#radio-list': radioList, '#save-radio': radioSave,
        '#radio-count': radioCount, '#radio-now': radioNow,
        '#radio-stop': radioStop, '#radio-add': radioAdd,
        '#toast': radioToast
    }[selector] || stub());
    document.querySelectorAll = () => [];
    let radioPlayed = false;
    globalThis.api = async () => { radioPlayed = true; return {}; };
    globalThis.bindRadio({
        stations: [{ word: 'groove', name: 'Groove Salad',
            url: 'http://example/original', enabled: true }],
        max_stations: 32, playback_supported: true
    });
    await radioHandlers.click({ target: radioPlay });
    check('edited radio row cannot play persisted station',
          radioPlayed === false && /Save the station/.test(radioToast.textContent),
          { played: radioPlayed, toast: radioToast.textContent });
    document.querySelector = oldRadioQuerySelector;
    document.querySelectorAll = oldRadioQuerySelectorAll;
    globalThis.api = oldRadioApi;

    /* ---- Action sound preview must preserve unsaved form state ---- */
    const oldQuerySelector = document.querySelector;
    const oldQuerySelectorAll = document.querySelectorAll;
    const oldApi = globalThis.api;
    const soundBox = {
        checked: true,
        dataset: { sound: 'action-1' },
        addEventListener() {}
    };
    const soundTry = { dataset: { try: 'action-2' } };
    const count = { textContent: '' };
    const toast = {
        textContent: '',
        classList: { toggle() {}, add() {}, remove() {} }
    };
    let previewRequest;
    document.querySelector = selector => {
        if (selector === '#ab-count') return count;
        if (selector === '#toast') return toast;
        return stub();
    };
    document.querySelectorAll = selector => {
        if (selector === '#content input[data-sound]') return [soundBox];
        if (selector === '#content .sound-try') return [soundTry];
        return [];
    };
    globalThis.api = async (path, options) => {
        previewRequest = { path, options };
        return { playing: true };
    };
    globalThis.bindActionSounds();
    check('sound preview handler is bound', typeof soundTry.onclick === 'function',
          typeof soundTry.onclick);
    check('sound preview starts idle', state.busy === false, state.busy);
    let prevented = false;
    let stopped = false;
    await soundTry.onclick({
        preventDefault() { prevented = true; },
        stopPropagation() { stopped = true; }
    });
    check('sound preview invokes the API', !!previewRequest, {
        request: previewRequest,
        toast: toast.textContent,
        busy: state.busy
    });
    check('sound preview sends the selected sample',
          previewRequest && previewRequest.path === '/audio/sample' &&
          previewRequest.options.method === 'POST' &&
          JSON.parse(previewRequest.options.body).name === 'action-2', previewRequest);
    check('sound preview prevents label default handling', prevented && stopped);
    check('sound preview preserves an unsaved checkbox', soundBox.checked === true,
          soundBox.checked);
    check('sound preview leaves the page busy state clear', state.busy === false, state.busy);
    document.querySelector = oldQuerySelector;
    document.querySelectorAll = oldQuerySelectorAll;
    globalThis.api = oldApi;

    /* ---- System update: loaded image size against the daemon's cap ---- */
    const cap = { max_upload_bytes: 33554432, max_upload_ceiling_bytes: 33554432 };
    check('the cap is shown before a file is chosen',
          globalThis.updateSizeText(null, cap) === 'No file selected of 32.0 MiB limit',
          globalThis.updateSizeText(null, cap));
    check('a selected file is sized against the cap',
          globalThis.updateSizeText({ size: 19293798 }, cap) === '18.4 MiB of 32.0 MiB limit',
          globalThis.updateSizeText({ size: 19293798 }, cap));
    check('the cap is read from the daemon, not hard-coded',
          globalThis.updateSizeText({ size: 1048576 }, { max_upload_bytes: 67108864 })
            === '1.0 MiB of 64.0 MiB limit',
          globalThis.updateSizeText({ size: 1048576 }, { max_upload_bytes: 67108864 }));
    check('a daemon that reports no cap still shows the size',
          globalThis.updateSizeText({ size: 1048576 }, {}) === '1.0 MiB',
          globalThis.updateSizeText({ size: 1048576 }, {}));
    check('32 MiB is binary, not 33.6 MB', globalThis.mib(33554432) === '32.0 MiB',
          globalThis.mib(33554432));
    /* A device short of staging space reports a limit below the ceiling; the row
       has to say which of the two is doing the limiting. */
    const tight = { max_upload_bytes: 4194304, max_upload_ceiling_bytes: 33554432 };
    check('a space-limited device says so',
          globalThis.updateSizeText(null, tight) ===
            'No file selected of 4.0 MiB limit — free space on the device, under the 32.0 MiB ceiling',
          globalThis.updateSizeText(null, tight));
    check('an unconstrained device does not editorialise',
          globalThis.updateSizeText(null, cap).indexOf('ceiling') === -1,
          globalThis.updateSizeText(null, cap));

    console.log(failures ? 'web ui behaviour: ' + failures + ' FAILED' : 'web ui behaviour: ok');
    process.exit(failures ? 1 : 0);
}
main().catch(e => { console.error(e); process.exit(1); });
