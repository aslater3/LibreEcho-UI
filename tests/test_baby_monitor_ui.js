/* Behavioral coverage for the Safari/iOS-compatible Baby Monitor playback path.
 *
 * The page is driven through the real web/js/app.js under a small DOM shim with
 * a controllable Web Audio implementation, so the lifecycle Safari enforces is
 * covered behaviourally rather than by source-text assertions: synchronous
 * gesture unlock, suspended/interrupted AudioContext, delayed resume, stream
 * chunk scheduling, bounded look-ahead, abort/restart generation ownership and
 * a missing response body. The diagnostics assertions pin the privacy contract
 * (context state and counters only, never microphone samples).
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '..');

/* ---------------------------------------------------------------- DOM shim */

function element(id) {
    const el = {
        id, innerHTML: '', textContent: '', value: '', checked: false, disabled: false,
        classList: { add() {}, remove() {}, toggle() {} }, style: {}, dataset: {},
        onclick: null, oninput: null, onchange: null,
        addEventListener() {}, appendChild() {}, querySelectorAll() { return []; },
        focus() { document.activeElement = el; }
    };
    el.parentElement = { querySelector: () => element(id + '-output') };
    return el;
}

let elements = new Map();
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
function resetElements() {
    elements = new Map();
    content.innerHTML = '';
    document.activeElement = null;
}
function el(id) { return document.querySelector(id); }

globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
function storage() {
    const m = new Map();
    return { getItem: k => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)),
        removeItem: k => m.delete(k), clear: () => m.clear() };
}
globalThis.localStorage = storage();
globalThis.sessionStorage = storage();
globalThis.confirm = () => false;
globalThis.prompt = () => '';
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };

/* -------------------------------------------------------------- Web Audio */

class FakeGain {
    constructor() { this.gain = { value: 0 }; this.connected = []; }
    connect(target) { this.connected.push(target); }
    disconnect() {}
}
class FakeSource {
    constructor(context) {
        this.context = context; this.buffer = null; this.onended = null;
        this.connected = []; this.startedAt = null; this.stopped = false;
    }
    connect(target) { this.connected.push(target); }
    disconnect() {}
    start(when) { if (this.startedAt !== null) throw new Error('buffer source started twice'); this.startedAt = when; }
    stop() { this.stopped = true; }
    fireEnded() { this.stopped = true; if (this.onended) this.onended(); }
}
function fakeBuffer(channels, length, rate) {
    const data = [];
    for (let c = 0; c < channels; c++) data.push(new Float32Array(length));
    return { numberOfChannels: channels, length, sampleRate: rate, duration: length / rate,
        getChannelData: c => data[c], data };
}
let resumeImpl = null;
let contexts = [];
class FakeAudioContext {
    constructor() {
        this.state = 'suspended';
        this.sampleRate = 16000;
        this.time = 0;
        this.destination = { name: 'destination' };
        this.resumeCalls = 0;
        this.closeCalls = 0;
        this.sources = [];
        this.buffers = [];
        this.gains = [];
        this.onstatechange = null;
        contexts.push(this);
    }
    get currentTime() { return this.time; }
    resume() {
        this.resumeCalls++;
        if (resumeImpl) return resumeImpl(this);
        return new Promise(resolve => setTimeout(() => { this.state = 'running'; resolve(); }, 0));
    }
    close() { this.closeCalls++; this.state = 'closed'; return Promise.resolve(); }
    createGain() { const g = new FakeGain(); this.gains.push(g); return g; }
    createBuffer(channels, length, rate) { const b = fakeBuffer(channels, length, rate); this.buffers.push(b); return b; }
    createBufferSource() { const s = new FakeSource(this); this.sources.push(s); return s; }
    setState(state) { this.state = state; if (this.onstatechange) this.onstatechange(); }
    /* Audio buffer sources created for playback (index 0 is the gesture unlock). */
    get playbackSources() { return this.sources.slice(1); }
}
globalThis.window.AudioContext = FakeAudioContext;

/* ------------------------------------------------------------- fake fetch */

const SOURCE_DATA = {
    available: true, simulated: false,
    sources: [{ id: '0:24', card: 0, device: 24, name: 'Echo array', rate: 16000, channels: 9,
        bits: 24, valid_bits: 16, encoding: 'pcm_s24_3le',
        microphones: [{ channel: 0, name: 'Array microphone 1' }, { channel: 3, name: 'Array microphone 4' }] }],
    calibration: { complete: false, q14: [16384], source: '/proc/idme/miccal' }
};

function jsonResponse(payload, status = 200) {
    return { ok: status < 400, status, headers: { get: () => null }, json: async () => payload };
}
function streamBody() {
    const waiting = [];
    const ready = [];
    let finished = false;
    return {
        push(bytes) { if (waiting.length) waiting.shift()({ value: bytes, done: false }); else ready.push(bytes); },
        finish() { finished = true; while (waiting.length) waiting.shift()({ value: undefined, done: true }); },
        fail(error) { while (waiting.length) waiting.shift()(Promise.reject(error)); },
        getReader() {
            return { read() {
                if (ready.length) return Promise.resolve({ value: ready.shift(), done: false });
                if (finished) return Promise.resolve({ done: true });
                return new Promise(resolve => waiting.push(resolve));
            } };
        }
    };
}

let streamPlan = null;          /* { status, header, body, gate } */
let streamRequests = [];
let deferredStream = null;
globalThis.fetch = (url, options = {}) => {
    const target = String(url);
    if (target.startsWith('/api/v1/baby-monitor/stream')) {
        streamRequests.push({ url: target, options });
        if (deferredStream) {
            const plan = deferredStream;
            return new Promise((resolve, reject) => plan.settle = () => resolve(
                { ok: true, status: 200, headers: { get: k => (k === 'X-LibreEcho-Audio' ? plan.header : null) }, body: plan.body }));
        }
        if (!streamPlan) return Promise.reject(new Error('unexpected stream request'));
        const plan = streamPlan;
        if (plan.gate) return new Promise((resolve, reject) => { plan.settle = () => resolve(plan.response); plan.fail = reject; });
        return Promise.resolve(plan.response);
    }
    if (target.startsWith('/api/v1/baby-monitor')) return Promise.resolve(jsonResponse({ ok: true, data: SOURCE_DATA }));
    return Promise.reject(new Error('startup fetch intentionally unavailable'));
};

/* ------------------------------------------------------------------- app */

vm.runInThisContext(fs.readFileSync(path.join(ROOT, 'web/js/app.js'), 'utf8'), { filename: 'app.js' });
const state = vm.runInThisContext('state');
const babyStream = vm.runInThisContext('babyStream');
const babyMonitorPage = vm.runInThisContext('babyMonitorPage');

/* ------------------------------------------------------------- utilities */

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
async function settle(ms = 5) {
    await new Promise(resolve => setTimeout(resolve, ms));
}
/* Drain a stream handler without deadlocking when a case intentionally stalls. */
async function quiesce(promise, ms = 400) {
    await Promise.race([promise.catch(() => {}), settle(ms)]);
}
function tinyPcm(values) {
    const bytes = new Uint8Array(values.length * 2);
    values.forEach((value, i) => {
        const signed = value < 0 ? value + 0x10000 : value;
        bytes[i * 2] = signed & 0xff;
        bytes[i * 2 + 1] = (signed >> 8) & 0xff;
    });
    return bytes;
}
function packed24(frames) {
    const bytes = new Uint8Array(frames.length * frames[0].length * 3);
    let offset = 0;
    frames.forEach(frame => frame.forEach(value => {
        const signed = value < 0 ? value + 0x1000000 : value;
        bytes[offset++] = signed & 0xff;
        bytes[offset++] = (signed >> 8) & 0xff;
        bytes[offset++] = (signed >> 16) & 0xff;
    }));
    return bytes;
}
function diagnostics() {
    return {
        context: el('#baby-diag-context').textContent,
        http: el('#baby-diag-http').textContent,
        received: el('#baby-diag-received').textContent,
        frames: el('#baby-diag-frames').textContent,
        scheduled: el('#baby-diag-scheduled').textContent,
        playback: el('#baby-diag-playback').textContent,
        error: el('#baby-diag-error').textContent
    };
}
function diagnosticsText() {
    const d = diagnostics();
    return [d.context, d.http, d.received, d.frames, d.scheduled, d.playback, d.error].join(' ');
}
function installStream(header, { status = 200, body = streamBody(), gate = false } = {}) {
    streamPlan = { header, body, gate, response: { ok: status < 400, status, headers: { get: k => (k === 'X-LibreEcho-Audio' ? header : null) }, body: status < 400 ? body : null } };
    return body;
}
function reset() {
    resetElements();
    contexts = [];
    streamRequests = [];
    streamPlan = null;
    deferredStream = null;
    resumeImpl = null;
    state.token = '';
    state.page = 'Baby Monitor';
    state.renderGeneration++;
}
async function mountPage() {
    state.page = 'Baby Monitor';
    state.renderGeneration++;
    await babyMonitorPage();
    return { start: el('#baby-start'), stop: el('#baby-stop') };
}
/* Click Start and keep the (never-resolving-until-stop) handler promise alive. */
function clickStart() {
    const promise = el('#baby-start').onclick();
    promise.catch(() => {});
    return promise;
}

/* ------------------------------------------------------------------ cases */

async function caseGestureUnlock() {
    reset();
    const { start } = await mountPage();
    const body = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0');
    let released = null;
    resumeImpl = context => new Promise(resolve => { released = () => { context.state = 'running'; resolve(); }; });
    const pending = clickStart();

    const context = contexts[0];
    check(!!context, 'Start listening created an AudioContext');
    checkEqual(context.sources.length, 1, 'a silent unlock source is started synchronously inside the user gesture');
    checkEqual(context.playbackSources.length, 0, 'no stream audio is scheduled before the stream arrives');
    if (context.sources[0]) {
        checkEqual(context.sources[0].startedAt, 0, 'the unlock source starts immediately (silent frame at time 0)');
        check(context.buffers[0] && context.buffers[0].length === 1,
            'the unlock buffer is a single silent frame so it cannot mask stream audio');
        check(context.sources[0].connected.length === 1, 'the unlock source is connected to the output gain');
    }
    checkEqual(streamRequests.length, 0, 'the stream request waits for the AudioContext to run');
    checkEqual(el('#baby-status').textContent, 'Connecting…', 'the page reports the connecting state');

    released();
    await settle();
    await settle();
    checkEqual(streamRequests.length, 1, 'the stream request starts once the AudioContext is running');
    check(streamRequests[0].url.includes('source=0%3A24') && streamRequests[0].url.includes('channel=0'),
        'the stream request carries the selected source and channel');
    check(!!(streamRequests[0].options && streamRequests[0].options.signal), 'the stream request is abortable');
    checkEqual(el('#baby-status').textContent, 'Listening', 'the page reports listening once the body is present');
    checkEqual(diagnostics().context, 'running', 'diagnostics expose the running AudioContext state');
    checkEqual(diagnostics().http, 'HTTP 200', 'diagnostics expose the stream HTTP status');
    body.finish();
    await settle();
    await quiesce(pending);
}

async function caseChunkScheduling() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    const body = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0');
    const pending = clickStart();
    await settle();
    const context = contexts[0];

    body.push(tinyPcm([1000, -1000, 12345, -12345]));
    await settle();
    checkEqual(context.playbackSources.length, 1, 'a stream chunk is scheduled as one buffer');
    const first = context.playbackSources[0];
    check(first.buffer && first.buffer.length === 4, 'the first buffer holds the four decoded frames');
    if (first.buffer) {
        const data = first.buffer.getChannelData(0);
        check(Math.abs(data[0] - 1000 / 32768) < 1e-9 && Math.abs(data[1] + 1000 / 32768) < 1e-9,
            'signed 16-bit samples decode with the correct polarity and scale');
        check(Math.abs(data[2] - 12345 / 32768) < 1e-9, 'large positive samples decode correctly');
    }
    checkEqual(first.startedAt, 0.05, 'the first buffer is scheduled at the configured start lead');
    checkEqual(first.connected.length, 1, 'the scheduled buffer is connected to the playback gain');

    /* A chunk boundary splitting a frame must not drop or duplicate audio. */
    body.push(tinyPcm([100, -100, 200, -200]).slice(0, 5));
    await settle();
    checkEqual(context.playbackSources.length, 2, 'a partial frame is carried instead of scheduled');
    check(context.playbackSources[1] && context.playbackSources[1].buffer.length === 2,
        'only whole frames from the split chunk are scheduled');
    body.push(new Uint8Array([...tinyPcm([200, -200]).slice(1), ...tinyPcm([300, -300])]));
    await settle();
    checkEqual(context.playbackSources.length, 3, 'the carried bytes complete a frame with the next chunk');
    check(context.playbackSources[2].buffer.length === 4, 'the rejoined chunk schedules the expected frames');

    const starts = context.playbackSources.map(source => source.startedAt);
    check(starts.every((start, i) => i === 0 || start > starts[i - 1]), 'buffers are scheduled in order without overlap');
    check(Math.abs(starts[1] - (starts[0] + 4 / 16000)) < 1e-9,
        'consecutive buffers are scheduled back to back (continuous playback)');

    checkEqual(diagnostics().frames, '10 frames', 'diagnostics expose the decoded frame count');
    checkEqual(diagnostics().received, '20 bytes', 'diagnostics expose the received byte count');
    /* Displayed to 1 ms, so compare at millisecond resolution. */
    check(Math.abs(Number.parseFloat(diagnostics().scheduled) - 10 / 16000) <= 0.0005,
        'diagnostics expose the scheduled seconds of audio');
    checkEqual(diagnostics().playback, 'playing', 'diagnostics expose that playback started');
    check(!diagnosticsText().includes('12345') && !diagnosticsText().includes('0.3767'),
        'diagnostics never expose microphone sample values');
    checkEqual(babyStream.nodes.size >= 1, true, 'scheduled audio nodes stay referenced until they end');
    const before = babyStream.nodes.size;
    context.playbackSources[0].fireEnded();
    checkEqual(babyStream.nodes.size, before - 1, 'an ended node is released from the retained set');

    body.finish();
    await settle();
    await quiesce(pending);
}

async function caseBoundedLookahead() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    const body = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0');
    const pending = clickStart();
    await settle();
    const context = contexts[0];
    const big = tinyPcm(new Array(3200).fill(600));            /* 0.2 s per chunk */
    for (let i = 0; i < 20; i++) { body.push(big); await settle(0); }
    await settle();
    const ahead = context.playbackSources.map(source => source.startedAt - context.time);
    check(ahead.every(value => value <= 0.750001),
        'scheduled look-ahead is bounded instead of buffering without limit');
    check(babyStream.nextTime - context.time <= 0.750001 + 0.2 + 1e-9,
        'the scheduling cursor is bounded by the look-ahead plus one chunk, not by the whole stream');
    checkEqual(context.playbackSources.length, 20, 'every whole chunk was scheduled');
    /* The chunks queued beyond the resync point must be cancelled, not left to
       play on top of the audio that follows once the cursor is reset. */
    check(context.playbackSources.filter(source => source.stopped).length > 0,
        'the queued backlog beyond the resync point is cancelled instead of left to overlap');
    const live = context.playbackSources.filter(source => !source.stopped);
    const overlaps = live.some((a, i) => live.some((b, j) => j > i
        && a.startedAt < b.startedAt + b.buffer.duration
        && b.startedAt < a.startedAt + a.buffer.duration));
    check(!overlaps, 'queued audio never overlaps another queued buffer after a resync');
    check(babyStream.nodes.size <= 6, 'the retained queue stays bounded by the look-ahead window');

    /* A stalled reader must not leave the cursor in the past and burst-play. */
    context.time = 5;
    body.push(big);
    await settle();
    const last = context.playbackSources[context.playbackSources.length - 1];
    check(Math.abs(last.startedAt - (context.time + 0.02)) < 1e-9,
        'a stalled stream resynchronises to the current time instead of replaying a backlog');
    body.finish();
    await settle();
    await quiesce(pending);
}

async function caseInterruptedContext() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    const body = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0');
    const pending = clickStart();
    await settle();
    const context = contexts[0];
    body.push(tinyPcm([10, 20, 30, 40]));
    await settle();
    const resumesBefore = context.resumeCalls;

    context.setState('interrupted');
    checkEqual(diagnostics().context, 'interrupted', 'diagnostics expose an interrupted AudioContext immediately');
    await settle();
    check(context.resumeCalls > resumesBefore, 'an interrupted AudioContext is resumed again');
    checkEqual(diagnostics().context, 'running', 'diagnostics return to running after the interruption clears');

    body.push(tinyPcm([50, 60, 70, 80]));
    await settle();
    checkEqual(context.playbackSources.length, 2, 'streaming continues after an interruption');
    checkEqual(el('#baby-status').textContent, 'Listening', 'the page still reports listening after an interruption');
    body.finish();
    await settle();
    await quiesce(pending);
}

async function caseStreamEndAndStop() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    const body = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0');
    const pending = clickStart();
    await settle();
    const context = contexts[0];
    body.push(tinyPcm([1, 2, 3, 4]));
    await settle();

    body.finish();
    await settle();
    /* The last chunk is scheduled ahead of the clock, so ending the stream must
       not close the graph on top of it. */
    checkEqual(context.closeCalls, 0, 'a finished stream keeps the graph open for the queued tail');
    checkEqual(diagnostics().playback, 'draining', 'diagnostics report the queued tail draining');
    const tail = context.playbackSources[context.playbackSources.length - 1];
    check(!!tail && !tail.stopped, 'the final scheduled chunk is still pending when the stream ends');
    if (tail) tail.fireEnded();                 /* the queued tail plays out */
    await settle();
    await quiesce(pending);
    checkEqual(el('#baby-status').textContent, 'Stopped', 'a finished stream returns the page to the stopped state');
    checkEqual(diagnostics().playback, 'ended', 'diagnostics distinguish an ended stream from a manual stop');
    check(context.closeCalls >= 1, 'a finished stream releases its AudioContext');
    checkEqual(babyStream.nodes.size, 0, 'a finished stream releases its retained nodes');

    /* Manual stop must abort the HTTP stream and release the audio graph. */
    const nextBody = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0');
    const pendingStop = clickStart();
    await settle();
    const stopContext = contexts[contexts.length - 1];
    nextBody.push(tinyPcm([5, 6, 7, 8]));
    await settle();
    checkEqual(el('#baby-status').textContent, 'Listening', 'a restarted stream reports listening');
    const controller = babyStream.controller;
    el('#baby-stop').onclick();
    check(controller.signal.aborted, 'Stop aborts the in-flight stream request');
    check(stopContext.closeCalls >= 1, 'Stop releases the AudioContext');
    checkEqual(el('#baby-status').textContent, 'Stopped', 'Stop returns the page to the stopped state');
    checkEqual(diagnostics().playback, 'stopped', 'diagnostics record a manual stop');
    const scheduledBefore = stopContext.playbackSources.length;
    nextBody.push(tinyPcm([9, 10, 11, 12]));
    await settle();
    checkEqual(stopContext.playbackSources.length, scheduledBefore, 'a chunk arriving after Stop schedules no audio');
    checkEqual(el('#baby-status').textContent, 'Stopped', 'a late chunk after Stop cannot restart the status');
    await quiesce(pendingStop);
}

async function caseGenerationOwnership() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    /* Stream A never settles: it is superseded while its request is in flight. */
    const bodyA = streamBody();
    deferredStream = { header: 'pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=0', body: bodyA };
    const pendingA = clickStart();
    await settle();
    const contextA = contexts[0];
    checkEqual(streamRequests.length, 1, 'the first stream request is in flight');

    /* Stream B starts normally while A is still pending. */
    const deferredA = deferredStream;
    deferredStream = null;
    const bodyB = installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000;selected-channel=1');
    const pendingB = clickStart();
    await settle();
    const contextB = contexts[contexts.length - 1];
    checkEqual(streamRequests.length, 2, 'a restart issues a new stream request');
    bodyB.push(tinyPcm([1000, 2000, 3000, 4000]));
    await settle();
    checkEqual(el('#baby-status').textContent, 'Listening', 'the restarted stream reports listening');
    checkEqual(contextB.playbackSources.length, 1, 'the newer generation schedules audio');
    checkEqual(diagnostics().playback, 'playing', 'diagnostics describe the newer generation');
    checkEqual(contextA.playbackSources.length, 0, 'the superseded generation scheduled no audio');
    check(contextA.closeCalls >= 1, 'the superseded AudioContext was released');

    /* A's late response and late chunk must not touch the newer generation. */
    deferredA.settle();
    await settle();
    bodyA.push(tinyPcm([7, 7, 7, 7]));
    await settle();
    checkEqual(contextA.playbackSources.length, 0, 'a late response for a superseded generation schedules no audio');
    bodyA.finish();
    await settle();
    await quiesce(pendingA);
    checkEqual(el('#baby-status').textContent, 'Listening', 'a superseded stream cannot stop the newer stream');
    checkEqual(diagnostics().playback, 'playing', 'the newer generation keeps its diagnostics');
    check(!!babyStream.controller && babyStream.controller.signal.aborted === false,
        'the newer stream request is still abortable');
    bodyB.finish();
    await settle();
    await quiesce(pendingB);
    checkEqual(el('#baby-status').textContent, 'Stopped', 'the surviving stream stops cleanly when it ends');
}

async function caseMissingBodyAndHttpFailure() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000', { body: null });
    const pending = clickStart();
    await settle();
    const context = contexts[0];
    checkEqual(context.playbackSources.length, 0, 'a response without a body schedules no audio');
    check(diagnostics().error.includes('no audio body'),
        'a missing response body is exposed in the on-page diagnostics');
    checkEqual(el('#baby-status').textContent, 'Stopped', 'a missing response body leaves the page stopped');
    await quiesce(pending);

    /* A non-2xx response must surface the HTTP status instead of silence. */
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    installStream('', { status: 503 });
    const failed = clickStart();
    await settle();
    checkEqual(contexts[0].playbackSources.length, 0, 'a failing stream schedules no audio');
    check(diagnostics().error.includes('503'), 'an HTTP failure is exposed with its status code');
    checkEqual(el('#baby-status').textContent, 'Stopped', 'an HTTP failure leaves the page stopped');
    await quiesce(failed);
}

async function caseBlockedContext() {
    reset();
    await mountPage();
    /* Safari can leave the context suspended even though resume() resolves. */
    resumeImpl = () => Promise.resolve();
    installStream('pcm_s16_le;channels=1;valid-bits=16;rate=16000');
    const pending = clickStart();
    await settle(1400);
    const context = contexts[0];
    check(context.resumeCalls > 1, 'a context that stays suspended is resumed again (bounded retries)');
    checkEqual(streamRequests.length, 0, 'no stream is requested while the AudioContext never runs');
    checkEqual(el('#baby-status').textContent, 'Stopped', 'a blocked AudioContext leaves the page stopped');
    check(diagnostics().error.toLowerCase().includes('blocked'),
        'a blocked AudioContext is exposed as an actionable on-page error');
    checkEqual(diagnostics().context, 'suspended', 'diagnostics expose the blocked AudioContext state');
    await quiesce(pending);
    await settle();
}

async function casePacked24Contract() {
    reset();
    await mountPage();
    resumeImpl = context => { context.state = 'running'; return Promise.resolve(); };
    const body = installStream('pcm_s24_3le;channels=9;valid-bits=16;rate=16000;selected-channel=3');
    const pending = clickStart();
    await settle();
    const context = contexts[0];
    const lane = () => [0, 0, 0, 0, 0, 0, 0, 0, 0];
    const frames = [lane(), lane(), lane()];
    frames[0][3] = 24576;                 /* +0.75 full scale */
    frames[1][3] = -32768;                /* -1.0 full scale  */
    frames[2][3] = 16384;                 /* +0.5 full scale  */
    frames.forEach((frame, i) => { frame[0] = 20000 + i; });   /* other lanes must be ignored */
    body.push(packed24(frames));
    await settle();
    checkEqual(context.playbackSources.length, 1, 'packed 24-bit frames are scheduled as one buffer');
    const buffer = context.playbackSources[0].buffer;
    check(buffer && buffer.length === 3, 'packed 24-bit frames decode to three samples');
    if (buffer) {
        const data = buffer.getChannelData(0);
        check(Math.abs(data[0] - 0.75) < 1e-6, 'the selected 24-bit lane decodes at full-scale positive');
        check(Math.abs(data[1] + 1) < 1e-6, 'the selected 24-bit lane decodes at full-scale negative');
        check(Math.abs(data[2] - 0.5) < 1e-6, 'the selected 24-bit lane decodes at half scale');
        check(Math.abs(data.reduce((a, b) => a + b, 0) - 0.25) < 1e-6,
            'unselected lanes from the nine-lane array are ignored');
    }
    checkEqual(diagnostics().frames, '3 frames', 'diagnostics count packed 24-bit frames');
    checkEqual(diagnostics().received, '81 bytes', 'diagnostics count packed 24-bit bytes');
    check(!diagnosticsText().includes('24576') && !diagnosticsText().includes('0.75'),
        'packed 24-bit diagnostics never expose sample values');
    body.finish();
    await settle();
    await quiesce(pending);
}

async function main() {
    const watchdog = setTimeout(() => {
        console.error('baby monitor iOS playback lifecycle: timed out waiting for a stream lifecycle');
        process.exit(1);
    }, 30000);
    const cases = [
        ['synchronous gesture unlock and stream request gating', caseGestureUnlock],
        ['stream chunk scheduling and frame carry', caseChunkScheduling],
        ['bounded look-ahead and stalled-reader resynchronisation', caseBoundedLookahead],
        ['interrupted AudioContext recovery', caseInterruptedContext],
        ['stream end, manual stop and post-stop chunks', caseStreamEndAndStop],
        ['abort/restart generation ownership', caseGenerationOwnership],
        ['missing response body and HTTP failure', caseMissingBodyAndHttpFailure],
        ['AudioContext that never leaves suspended', caseBlockedContext],
        ['packed 24-bit audio contract', casePacked24Contract]
    ];
    for (const [name, run] of cases) {
        console.log('case: ' + name);
        try {
            await run();
        } catch (error) {
            failures.push(`${name}: ${error && error.stack ? error.stack : error}`);
            console.error('  FAIL: ' + name + ' threw: ' + (error && error.message ? error.message : error));
        }
    }
    clearTimeout(watchdog);
    if (failures.length) {
        console.error(`baby monitor iOS playback lifecycle: ${failures.length} check(s) failed`);
        process.exitCode = 1;
        return;
    }
    console.log('baby monitor iOS playback lifecycle: ok');
}
main().catch(error => { console.error(error); process.exitCode = 1; });