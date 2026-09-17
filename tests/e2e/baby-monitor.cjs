'use strict';
/*
 * Browser verification for the Baby Monitor playback path (#251).
 *
 * The unit suite (tests/test_baby_monitor_ui.js) drives web/js/app.js through a
 * DOM shim with a fake fetch and a fake ReadableStream. This suite runs the same
 * page in a real browser engine, so the shipped code is exercised with a real
 * fetch, a real ReadableStream, a real AbortController, real timers and real DOM
 * events.
 *
 * The preview mock backend cannot stream PCM (its stream endpoint answers 501),
 * so the suite runs one small reverse proxy in front of it: everything is
 * forwarded to the mock server except the two Baby Monitor endpoints, which the
 * proxy answers itself - the stream in real chunks, with the audio contract
 * header. The proxy keeps the page on a single origin, so the server's
 * `connect-src 'self'` policy is exercised as shipped instead of being relaxed
 * for the test.
 *
 * The Web Audio implementation is injected before the page scripts and models
 * the Safari lifecycle the issue describes: the context starts suspended,
 * resume() resolves, and the output is only unlocked when a buffer source was
 * started synchronously inside the user gesture. With that model the pre-fix
 * implementation reported "Listening" over a suspended context and scheduled
 * inaudible sources, which the first scenario pins.
 *
 * Select the engine with LIBREECHO_E2E_BROWSER=chromium|webkit. Chromium is the
 * default; WebKit is the engine iOS Safari uses, so it is the closest host-side
 * stand-in for the reported iPhone failure.
 *
 * No audio hardware is involved: this verifies the playback lifecycle, counters,
 * abort and generation behaviour, not audible output. Real iPhone Safari audio
 * stays a hardware gate.
 */
const assert = require('node:assert/strict');
const http = require('node:http');
const { chromium, webkit } = require('playwright');

const mockURL = process.env.LIBREECHO_E2E_URL || 'http://127.0.0.1:18083';
const engineName = process.env.LIBREECHO_E2E_BROWSER || 'chromium';
const engines = { chromium, webkit };

const checks = [];
function pass(name) { checks.push(name); console.log('  ok  ' + name); }

function captureBrowserFailures(page) {
  const failures = [];
  page.on('pageerror', e => failures.push(`pageerror: ${e.message}`));
  page.on('console', m => {
    if (m.type() !== 'error') return;
    const text = m.text();
    if (/^Failed to load resource: the server responded with a status of \d+/.test(text)) return;
    failures.push(`console.error: ${text}`);
  });
  return failures;
}

async function waitForPage(page, title) {
  await page.waitForFunction(expected => {
    const h = document.querySelector('#page-title'), c = document.querySelector('#content');
    return h && h.textContent.trim() === expected && c && c.textContent.trim().length > 0 && !c.querySelector('.loading');
  }, title, { timeout: 8000 });
}

async function selectPage(page, name) {
  const item = page.locator('#nav').getByText(name, { exact: true });
  await item.waitFor({ state: 'visible', timeout: 5000 });
  await item.click();
  await waitForPage(page, name);
}

/* --------------------------------------------------------------- Web Audio */

/* Injected before the page scripts. Contexts, sources and resume calls are all
   recorded in window.__babyAudio, and a MutationObserver keeps the history of
   the visible state fields so transient transitions are not missed. */
function installAudioShim() {
    const log = [];
    const seen = { status: [], playback: [], context: [] };
    const config = { resumeDelayMs: 0, resumeNeverSettles: false, unlockRequiresGestureSource: true };
    const api = { log, seen, config, contexts: [], gestureOpen: false };
    window.__babyAudio = api;
    function note(name, extra) { log.push(Object.assign({ name }, extra || {})); }

    function sample() {
      const read = (selector, key) => {
        const el = document.querySelector(selector);
        if (!el) return;
        const value = el.textContent;
        const history = seen[key];
        if (history[history.length - 1] !== value) history.push(value);
      };
      read('#baby-status', 'status');
      read('#baby-diag-playback', 'playback');
      read('#baby-diag-context', 'context');
    }
    const observer = new MutationObserver(sample);
    observer.observe(document.documentElement || document, { subtree: true, childList: true, characterData: true });

    /* Safari only opens the output for a source started inside the user gesture.
       The window spans the click task: the synchronous part of the handler runs
       in it, and the flag clears on the next task. */
    document.addEventListener('click', () => {
      api.gestureOpen = true;
      setTimeout(() => { api.gestureOpen = false; }, 0);
    }, true);

    class FakeGain {
      constructor() { this.gain = { value: 0 }; }
      connect() {} disconnect() {}
    }
    class FakeBuffer {
      constructor(channels, length, rate) {
        this.numberOfChannels = channels; this.length = length; this.sampleRate = rate;
        this.duration = length / rate;
        this._data = []; for (let i = 0; i < channels; i++) this._data.push(new Float32Array(length));
      }
      getChannelData(c) { return this._data[c]; }
    }
    class FakeSource {
      constructor(context) { this.context = context; this.buffer = null; this.onended = null; this.startedAt = null; this.stopped = false; this.timer = null; }
      connect() {} disconnect() {}
      start(when) {
        this.startedAt = when == null ? 0 : when;
        if (api.gestureOpen) this.context.unlockArmed = true;
        note('source-start', { inGesture: api.gestureOpen, armed: !!this.context.unlockArmed, state: this.context.state });
        this.context.sources.push(this);
        if (this.buffer) {
          const delay = Math.max(0, (this.startedAt + this.buffer.duration - this.context.currentTime) * 1000);
          this.timer = setTimeout(() => { if (!this.stopped && this.onended) this.onended(); }, delay);
        }
      }
      stop() { this.stopped = true; if (this.timer) clearTimeout(this.timer); }
    }
    class FakeAudioContext {
      constructor() {
        this.state = 'suspended'; this.sampleRate = 16000; this.baseTime = 0; this.t0 = performance.now();
        this.destination = { kind: 'destination' }; this.onstatechange = null;
        this.resumeCalls = 0; this.closeCalls = 0; this.sources = []; this.unlockArmed = false;
        api.contexts.push(this);
        note('context-created', { state: this.state });
      }
      get currentTime() { return this.baseTime + (performance.now() - this.t0) / 1000; }
      resume() {
        this.resumeCalls += 1;
        note('resume', { state: this.state });
        if (config.resumeNeverSettles) return new Promise(() => {});
        return new Promise(resolve => setTimeout(() => {
          if (!config.unlockRequiresGestureSource || this.unlockArmed) this.state = 'running';
          note('resume-settled', { state: this.state, armed: !!this.unlockArmed });
          resolve();
        }, config.resumeDelayMs));
      }
      close() {
        this.closeCalls += 1; this.state = 'closed'; note('context-closed');
        if (this.onstatechange) this.onstatechange();
        return Promise.resolve();
      }
      createGain() { return new FakeGain(); }
      createBuffer(channels, length, rate) { return new FakeBuffer(channels, length, rate); }
      createBufferSource() { return new FakeSource(this); }
      interrupt() { this.state = 'interrupted'; note('interrupted'); if (this.onstatechange) this.onstatechange(); }
    }
    window.AudioContext = FakeAudioContext;
    window.webkitAudioContext = FakeAudioContext;

    const realFetch = window.fetch.bind(window);
    window.fetch = function (input, init) {
      const url = typeof input === 'string' ? input : (input && input.url) || '';
      if (url.includes('/api/v1/baby-monitor/stream')) {
        const contexts = api.contexts;
        note('fetch', { state: contexts.length ? contexts[contexts.length - 1].state : 'none' });
      }
      return realFetch(input, init);
    };
}

/* ------------------------------------------------------------- PCM fixtures */

const RATE = 16000;
const CHUNK_FRAMES = 1600; /* 0.1 s of audio per chunk */
const STREAM_HEADER = 'pcm_s16_le;channels=1;rate=16000';
const SOURCE = {
  available: true, simulated: false,
  sources: [{
    id: '0:24', card: 0, device: 24, name: 'Echo array', rate: RATE, channels: 9,
    bits: 24, valid_bits: 16, encoding: 'pcm_s24_3le',
    microphones: [{ channel: 0, name: 'Array microphone 1' }, { channel: 3, name: 'Array microphone 4' }]
  }],
  calibration: { complete: false, q14: [16384], source: '/proc/idme/miccal' }
};

/* A ramp whose magnitude never collides with a counter or a state name, so the
   privacy assertion below cannot pass by accident. */
function pcmChunk(frames = CHUNK_FRAMES, offset = 0) {
  const buffer = Buffer.alloc(frames * 2);
  for (let i = 0; i < frames; i++) buffer.writeInt16LE((((i + offset) % 2048) - 1024) * 7, i * 2);
  return buffer;
}

/* Reverse proxy in front of the preview mock backend. Everything is forwarded
   unchanged except the Baby Monitor endpoints: the source list is replaced with
   a real (non-simulated) array and the stream is answered with real chunks, so
   the page keeps one origin and the shipped CSP applies. */
async function startDeviceProxy({ streamStatus = 200, intervalMs = 40, totalChunks = 250 } = {}) {
  const upstream = new URL(mockURL);
  const state = { requests: [], aborted: 0, completed: 0 };
  const server = http.createServer((request, response) => {
    const url = request.url || '/';
    if (url.startsWith('/api/v1/baby-monitor/stream')) {
      state.requests.push(url);
      if (streamStatus !== 200) {
        const body = JSON.stringify({ ok: false, data: null, error: { code: 'io', message: 'Microphone stream could not start' } });
        response.writeHead(streamStatus, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
        response.end(body);
        return;
      }
      response.writeHead(200, {
        'content-type': 'application/octet-stream',
        'X-LibreEcho-Audio': STREAM_HEADER,
        'cache-control': 'no-store'
      });
      let sent = 0, finished = false, timer = null;
      /* A client that goes away mid-response leaves the response unfinished, so
         `writableEnded` is false when 'close' arrives; a response we ended
         ourselves sets it true. Tracking the response rather than the request
         keeps a stream that completed normally out of the cancellation count,
         which is what the Stop and page-leave assertions rely on. */
      const cancelled = () => {
        if (response.writableEnded) return;
        state.aborted += 1;
        finished = true;
        if (timer) clearInterval(timer);
      };
      response.on('close', cancelled);
      timer = setInterval(() => {
        if (finished) return;
        if (sent >= totalChunks) {
          finished = true; clearInterval(timer); state.completed += 1; response.end(); return;
        }
        response.write(pcmChunk(CHUNK_FRAMES, sent * CHUNK_FRAMES));
        sent += 1;
      }, intervalMs);
      return;
    }
    if (url === '/api/v1/baby-monitor') {
      const body = JSON.stringify({ ok: true, data: SOURCE, error: null });
      response.writeHead(200, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
      response.end(body);
      return;
    }
    const forwarded = http.request({
      host: upstream.hostname, port: upstream.port, path: url, method: request.method, headers: request.headers
    }, reply => {
      response.writeHead(reply.statusCode, reply.headers);
      reply.pipe(response);
    });
    forwarded.on('error', () => { response.writeHead(502).end(); });
    request.pipe(forwarded);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  state.origin = `http://127.0.0.1:${server.address().port}`;
  state.close = () => new Promise(resolve => server.close(resolve));
  return state;
}

/* ---------------------------------------------------------------- harness */

async function preparePage(browser, origin) {
  const context = await browser.newContext({ baseURL: origin });
  await context.addInitScript(installAudioShim);
  const page = await context.newPage();
  const failures = captureBrowserFailures(page);
  return { context, page, failures };
}

async function openBabyMonitor(page) {
  await page.goto('/', { waitUntil: 'domcontentloaded' });
  await waitForPage(page, 'Overview');
  await selectPage(page, 'Baby Monitor');
  await page.locator('#baby-start').waitFor({ state: 'visible', timeout: 5000 });
}

function probe(page) {
  return page.evaluate(() => {
    const audio = window.__babyAudio;
    return {
      log: audio.log,
      seen: audio.seen,
      contexts: audio.contexts.map(c => ({
        state: c.state, resumeCalls: c.resumeCalls, closeCalls: c.closeCalls,
        started: c.sources.filter(s => s.startedAt !== null).length, unlockArmed: !!c.unlockArmed
      }))
    };
  });
}

function diagnostics(page) {
  return page.evaluate(() => {
    const text = id => { const el = document.querySelector(id); return el ? el.textContent : null; };
    return {
      context: text('#baby-diag-context'), http: text('#baby-diag-http'),
      received: text('#baby-diag-received'), frames: text('#baby-diag-frames'),
      scheduled: text('#baby-diag-scheduled'), playback: text('#baby-diag-playback'),
      error: text('#baby-diag-error'), status: text('#baby-status')
    };
  });
}

const bytesOf = text => parseInt((text || '').replace(/[^0-9]/g, ''), 10) || 0;

async function waitForCondition(predicate, timeout = 6000, step = 25) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (predicate()) return true;
    await new Promise(resolve => setTimeout(resolve, step));
  }
  return predicate();
}

const waitForPlayback = page => page.waitForFunction(() =>
  document.querySelector('#baby-diag-playback')?.textContent === 'playing', null, { timeout: 8000 });

const waitForAllClosed = page => page.waitForFunction(() =>
  window.__babyAudio.contexts.length > 0 && window.__babyAudio.contexts.every(c => c.state === 'closed'),
  null, { timeout: 5000 });

/* --------------------------------------------------------------- scenarios */

/* The reported failure: Safari unlocks Web Audio only when a source is started
   inside the gesture, and resume() resolving is not proof the output opened. */
async function unlockAndStreamSuite(browser) {
  const device = await startDeviceProxy();
  try {
    const { context, page, failures } = await preparePage(browser, device.origin);
    try {
      await openBabyMonitor(page);

      assert.equal(await page.locator('#baby-source option').count(), 1, 'the capture endpoint should be listed');
      assert.equal(await page.locator('#baby-channel option').count(), 2, 'the microphone lanes should be listed');
      pass('the Baby Monitor page renders the source and lane controls');

      /* Safari can take a while to open the output, so resume() must be allowed
         to resolve late without the page racing ahead of it. */
      await page.evaluate(() => { window.__babyAudio.config.resumeDelayMs = 150; });
      await page.locator('#baby-start').click();
      await waitForPlayback(page);
      pass('pressing Start listening reaches playing playback against a live chunked stream');

      const started = await probe(page);
      const first = started.contexts[0];
      assert.ok(first, 'a Web Audio context should have been created');
      assert.ok(first.unlockArmed, 'the output is only unlocked by a source started inside the gesture');
      assert.ok(first.started >= 1, 'a silent unlock source should have been started');
      assert.ok(started.log.findIndex(e => e.name === 'source-start') < started.log.findIndex(e => e.name === 'resume'),
        'the unlock source must start before the first resume() call, inside the gesture');
      assert.equal(first.state, 'running', 'the context must reach running, not merely have resume() resolve');

      for (const id of ['#baby-diag-context', '#baby-diag-http', '#baby-diag-received', '#baby-diag-frames', '#baby-diag-scheduled', '#baby-diag-playback', '#baby-diag-error']) {
        assert.ok(await page.locator(id).count(), `the playback diagnostics should expose ${id}`);
      }
      pass('the playback diagnostics block exposes context, stream, counters, playback and error state');

      /* The stream must not be requested before the output is open, or Safari
         spends the session scheduling into a suspended context. */
      const fetchIndex = started.log.findIndex(e => e.name === 'fetch');
      const runningIndex = started.log.findIndex(e => e.name === 'resume-settled' && e.state === 'running');
      assert.ok(fetchIndex > -1, 'the stream should have been requested once the context was running');
      assert.ok(runningIndex > -1 && runningIndex < fetchIndex, 'the stream request must follow the running context');
      assert.equal(device.requests.length, 1, 'exactly one stream request should reach the device');
      assert.match(device.requests[0], /source=0%3A24&channel=0/, 'the selected source and lane must be requested');
      assert.equal(device.aborted, 0, 'a stream the page is still reading must not be counted as cancelled');
      pass('the stream is requested only after the context reports running, for the selected source and lane');

      const contextHistory = started.seen.context;
      assert.ok(contextHistory.includes('suspended'), `the suspended context must be observable, saw ${contextHistory}`);
      assert.ok(contextHistory.indexOf('running') > contextHistory.indexOf('suspended'),
        `the context state history must show suspended before running, saw ${contextHistory}`);

      /* Counters must advance chunk by chunk, not only at end of stream. */
      await page.waitForFunction(threshold =>
        parseInt((document.querySelector('#baby-diag-frames')?.textContent || '').replace(/[^0-9]/g, ''), 10) >= threshold,
        CHUNK_FRAMES * 3, { timeout: 8000 });
      const live = await diagnostics(page);
      assert.equal(live.http, 'HTTP 200', 'the stream status must be observable');
      assert.ok(bytesOf(live.received) >= CHUNK_FRAMES * 2 * 3, `received bytes must be counted before EOF, got ${live.received}`);
      assert.ok(bytesOf(live.frames) >= CHUNK_FRAMES * 3, `decoded frames must be counted before EOF, got ${live.frames}`);
      assert.match(live.scheduled, /^[0-9]+\.[0-9]{3} s$/, 'scheduled playback seconds must be observable');
      assert.ok(parseFloat(live.scheduled) > 0, 'playback must be scheduled');
      assert.equal(live.context, 'running', 'the context state must stay observable');
      assert.equal(live.playback, 'playing', 'the playback state must be observable');
      pass('stream status, received bytes, decoded frames and scheduled seconds are observable while streaming');

      const diagText = Object.values(live).join(' ');
      assert.ok(!diagText.includes('-7168'), 'diagnostics must never expose a microphone sample value');
      pass('the diagnostics block stays privacy-safe (state and counters only, never samples)');

      /* Stop must abort the in-flight request, not just stop scheduling. */
      const requestsBeforeStop = device.requests.length;
      assert.equal(device.aborted, 0, 'a healthy stream must never be counted as cancelled before Stop');
      await page.locator('#baby-stop').click();
      await page.waitForFunction(() => document.querySelector('#baby-status')?.textContent === 'Stopped', null, { timeout: 5000 });
      await waitForAllClosed(page);
      const stopped = await diagnostics(page);
      const after = await probe(page);
      assert.equal(stopped.playback, 'stopped', 'Stop must leave the playback diagnostic at stopped');
      assert.ok(after.contexts.every(c => c.state === 'closed'), 'Stop must release the audio graph');
      const framesAtStop = bytesOf(stopped.frames);
      await page.waitForTimeout(500);
      assert.equal(bytesOf((await diagnostics(page)).frames), framesAtStop, 'a superseded reader must not keep counting after Stop');
      assert.equal(device.requests.length, requestsBeforeStop, 'Stop must not open another stream');
      const abortedInFlight = await waitForCondition(() => device.aborted >= 1);
      assert.ok(abortedInFlight, 'Stop must abort the in-flight stream request at the device, not only stop scheduling');
      assert.equal(device.aborted, 1, 'exactly the stopped stream must be counted as cancelled');
      pass('Stop aborts the in-flight stream request and no superseded reader keeps running');

      /* Restart after Stop: a fresh generation streams again, and leaving the
         page then aborts the new request. */
      await page.locator('#baby-start').click();
      await waitForPlayback(page);
      await page.waitForFunction(() => window.__babyAudio.contexts.length === 2, null, { timeout: 4000 });
      const restarted = await probe(page);
      assert.equal(restarted.contexts[1].state, 'running', 'a restart must unlock a fresh context');
      assert.equal(device.requests.length, 2, 'a restart must open exactly one new stream');
      assert.equal(device.aborted, 1, 'the restarted stream must not be counted as cancelled while it runs');
      await selectPage(page, 'Overview');
      await waitForAllClosed(page);
      await page.waitForFunction(() => window.__babyAudio.seen.status.includes('Stopped'), null, { timeout: 5000 });
      const abortedOnLeave = await waitForCondition(() => device.aborted >= 2);
      assert.ok(abortedOnLeave, 'leaving the page must abort the new stream at the device');
      assert.equal(device.aborted, 2, 'exactly the two cancelled streams must be counted, no more');
      pass('restarting streams again and leaving the page aborts the new stream');

      assert.deepEqual(failures, [], `browser failures:\n${failures.join('\n')}`);
    } finally {
      await context.close();
    }
  } finally {
    await device.close();
  }
}

/* resume() that never settles must reach the actionable error, not sit on
   "Connecting…" and then claim Listening. */
async function blockedContextSuite(browser) {
  const device = await startDeviceProxy();
  try {
    const { context, page, failures } = await preparePage(browser, device.origin);
    try {
      await openBabyMonitor(page);
      await page.evaluate(() => { window.__babyAudio.config.resumeNeverSettles = true; });
      await page.locator('#baby-start').click();

      await page.waitForFunction(() => document.querySelector('#baby-diag-playback')?.textContent === 'stopped', null, { timeout: 6000 });
      const diag = await diagnostics(page);
      const state = await probe(page);
      assert.equal(diag.status, 'Stopped', 'a context that never runs must not leave the page on Listening');
      assert.match(diag.error, /blocked|suspended|unknown/i, 'the failure must be actionable');
      assert.equal(device.requests.length, 0, 'a blocked context must not open the microphone stream');
      assert.ok(state.contexts[0].resumeCalls >= 1, 'the page should attempt resume before giving up');
      assert.ok(state.contexts[0].closeCalls >= 1, 'the failed attempt must release the context');
      assert.ok(!state.seen.status.includes('Listening'), 'a blocked context must never report Listening');
      pass('a context that never leaves suspended fails actionably and never opens the stream');

      /* The page reports this failure to the console on purpose. */
      const unexpected = failures.filter(f => !/Audio playback is blocked/.test(f));
      assert.deepEqual(unexpected, [], `browser failures:\n${unexpected.join('\n')}`);
    } finally {
      await context.close();
    }
  } finally {
    await device.close();
  }
}

/* A failing stream must be reported instead of silently looking like playback. */
async function streamFailureSuite(browser) {
  const device = await startDeviceProxy({ streamStatus: 503 });
  try {
    const { context, page, failures } = await preparePage(browser, device.origin);
    try {
      await openBabyMonitor(page);
      await page.locator('#baby-start').click();
      await page.waitForFunction(() => document.querySelector('#baby-diag-http')?.textContent === 'HTTP 503', null, { timeout: 8000 });
      const diag = await diagnostics(page);
      const state = await probe(page);
      assert.notEqual(diag.status, 'Listening', 'a failed stream must not report Listening');
      assert.notEqual(diag.playback, 'playing', 'a failed stream must not report playback');
      assert.match(diag.error, /503|failed|unavailable/i, 'the stream failure must be observable');
      assert.ok(state.contexts[0].closeCalls >= 1, 'a failed stream must release the context');
      pass('an HTTP 503 stream is reported and releases playback instead of reporting Listening');

      const unexpected = failures.filter(f => !/Microphone stream failed \(503\)/.test(f));
      assert.deepEqual(unexpected, [], `browser failures:\n${unexpected.join('\n')}`);
    } finally {
      await context.close();
    }
  } finally {
    await device.close();
  }
}

/* Safari can interrupt a running context mid-stream; playback must recover. */
async function interruptedRecoverySuite(browser) {
  const device = await startDeviceProxy();
  try {
    const { context, page, failures } = await preparePage(browser, device.origin);
    try {
      await openBabyMonitor(page);
      await page.locator('#baby-start').click();
      await waitForPlayback(page);

      const before = await probe(page);
      const resumeCalls = before.contexts[0].resumeCalls;
      await page.evaluate(() => {
        const contexts = window.__babyAudio.contexts;
        contexts[contexts.length - 1].interrupt();
      });
      await page.waitForFunction(expected => {
        const contexts = window.__babyAudio.contexts;
        const last = contexts[contexts.length - 1];
        return last.state === 'running' && last.resumeCalls > expected;
      }, resumeCalls, { timeout: 6000 });
      await page.waitForFunction(() => document.querySelector('#baby-diag-context')?.textContent === 'running', null, { timeout: 4000 });
      assert.equal((await diagnostics(page)).status, 'Listening', 'recovery must not silently stop playback');
      const after = await probe(page);
      assert.equal(after.log.filter(e => e.name === 'fetch').length, 1, 'the recovered stream must not have been restarted');
      pass('an interrupted context is re-resumed and playback continues without restarting the stream');

      await selectPage(page, 'Overview');
      await waitForAllClosed(page);
      pass('leaving the Baby Monitor page releases the audio graph');

      assert.deepEqual(failures, [], `browser failures:\n${failures.join('\n')}`);
    } finally {
      await context.close();
    }
  } finally {
    await device.close();
  }
}

/* A stream that reaches its own end must not be counted as a cancellation: the
   cancellation counter is what the Stop and page-leave assertions above assert
   on, and a completed response also closes its connection. */
async function completedStreamSuite(browser) {
  const device = await startDeviceProxy({ intervalMs: 30, totalChunks: 6 });
  try {
    const { context, page, failures } = await preparePage(browser, device.origin);
    try {
      await openBabyMonitor(page);
      await page.locator('#baby-start').click();
      await waitForPlayback(page);
      const finished = await waitForCondition(() => device.completed >= 1, 8000);
      assert.ok(finished, 'the finite stream should reach its own end');
      await page.waitForFunction(() => document.querySelector('#baby-status')?.textContent === 'Stopped', null, { timeout: 8000 });
      const diag = await diagnostics(page);
      assert.equal(diag.playback, 'ended', 'a stream that ends by itself must be reported as ended');
      assert.equal(device.aborted, 0, 'a stream that ended by itself must not be counted as cancelled');
      pass('a stream that ends by itself is reported as ended and is not counted as a cancellation');

      await selectPage(page, 'Overview');
      await waitForAllClosed(page);
      assert.equal(device.aborted, 0, 'leaving after a natural end must not count a cancellation');
      pass('leaving the page after a natural end releases the audio graph without a cancellation');

      assert.deepEqual(failures, [], `browser failures:\n${failures.join('\n')}`);
    } finally {
      await context.close();
    }
  } finally {
    await device.close();
  }
}

async function main() {
  const launcher = engines[engineName];
  assert.ok(launcher, `LIBREECHO_E2E_BROWSER must be chromium or webkit, got ${engineName}`);
  const browser = await launcher.launch({ headless: true });
  console.log(`baby monitor playback browser suite (${engineName})`);
  try {
    await unlockAndStreamSuite(browser);
    await blockedContextSuite(browser);
    await streamFailureSuite(browser);
    await interruptedRecoverySuite(browser);
    await completedStreamSuite(browser);
    console.log(`\n${checks.length} checks passed, 0 console errors / page errors`);
  } finally {
    await browser.close();
  }
}

main().catch(error => {
  console.error(error.stack || error);
  process.exit(1);
});
