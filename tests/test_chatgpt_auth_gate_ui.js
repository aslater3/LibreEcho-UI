/*
 * Behavioural coverage for the ChatGPT account gate on the Integrations page
 * (UI #272).
 *
 * The page is driven the way the browser loads it: every script in
 * index.html's own order, with the daemon API stubbed per case. What is
 * asserted here is what the browser would actually do -- the rendered
 * <details>/toggle markup, the bound handlers, the requests a click leads to,
 * and the re-render the pending-login poll performs.
 *
 * The account is shared by the On Device Voice Assistant and GPT-Live, so the
 * cases below cover both panels for every state: signed out, waiting for
 * sign-in, authenticated but disabled, enabled, and GPT-Live's service missing
 * from the image (a different axis from authentication).
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

function element(id) {
  return { id, innerHTML: '', textContent: '', value: '', checked: false,
    disabled: false, onclick: null, onchange: null,
    classList: { add() {}, remove() {}, toggle() {} }, style: {}, dataset: {},
    addEventListener() {}, appendChild() {}, querySelectorAll() { return []; },
    closest() { return null; }, focus() {} };
}
const elements = new Map();
const content = element('content');
globalThis.document = {
  querySelector(s) {
    if (s === '#content') return content;
    if (!elements.has(s)) elements.set(s, element(s));
    return elements.get(s);
  },
  querySelectorAll() { return []; },
  createElement: element,
  addEventListener() {},
  body: element('body'),
  activeElement: null
};
globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fixture', replace() {} };
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
globalThis.URL = { createObjectURL: () => 'blob:unused', revokeObjectURL() {} };
// Startup stays pending so nothing runs over the case under test; the page
// function is driven explicitly below.
globalThis.fetch = () => new Promise(() => {});

const scripts = [...fs.readFileSync('web/index.html', 'utf8')
  .matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(x => x[1]);
assert(scripts.includes('/js/app.js') && scripts.includes('/js/integrations-ui.js'),
  'the Integrations renderer under test is not among the page scripts');
for (const src of scripts) vm.runInThisContext(fs.readFileSync('web' + src, 'utf8'), { filename: src });

const state = vm.runInThisContext('state');
const integrationsPage = vm.runInThisContext('integrationsPage');

let timers = [];
globalThis.setTimeout = (fn, ms) => { timers.push({ fn, ms }); return timers.length; };
globalThis.clearTimeout = () => {};

/* --- markup readers (the fake DOM does not parse HTML) ------------------- */

function panelFor(id) {
  const parts = content.innerHTML.split('<details');
  const seg = parts.find(p => p.includes('id="' + id + '"'));
  if (!seg) throw new Error(`the panel holding #${id} was not rendered: ${id}`);
  return '<details' + seg;
}
function panelOpen(id) {
  const panel = panelFor(id);
  const tag = panel.slice(0, panel.indexOf('>'));
  return /\sopen(\s|>)?$/.test(tag);
}
function stateLabel(id, label) {
  return panelFor(id).includes('</span>' + label + '</span>');
}
function toggleTag(id) {
  const match = content.innerHTML.match(new RegExp('<input class="toggle-input" id="' + id + '"[^>]*>'));
  if (!match) throw new Error(`toggle #${id} was not rendered`);
  return match[0];
}
function toggleDisabled(id) { return /\sdisabled/.test(toggleTag(id)); }
function toggleChecked(id) { return /\schecked/.test(toggleTag(id)); }
function bound(id) { return typeof document.querySelector('#' + id).onclick === 'function'; }
function changed(id) { return typeof document.querySelector('#' + id).onchange === 'function'; }

/* --- fixtures and the api stub ------------------------------------------ */

function assistantFixture(extra) {
  return Object.assign({
    ready: true, provider: 'openai-codex', provider_name: 'ChatGPT',
    subscription_auth: true, enabled: false, authenticated: false,
    auth_state: 'signed_out', user_code: '', verification_url: '', auth_error: '',
    model: 'gpt-5.4', prompt: 'Reply briefly.', clock_format: '12',
    base_url: '', api_key_configured: false,
    wake_connected: true, audio_connected: true, completed_transcripts: 0,
    last_stt_audio_ms: 0, last_stt_processing_ms: 0,
    last_speech_end_to_first_pcm_ms: 0, latency_target_ms: 3000,
    home_location: 'Fixture', latitude: '51', longitude: '0', weather_provider: 'off'
  }, extra);
}
function liveFixture(extra) {
  return Object.assign({
    enabled: false, mode: 'inactive', transport: 'realtime', last_event: 'idle',
    session: { state: 'idle', last_end: 'none', sessions_completed: 0, delegations: 0 },
    transport_metrics: { transport: 'websocket', session_ready: false }
  }, extra);
}

let requests = [];
async function page({ assistant, live, homeAssistant = false } = {}) {
  requests = [];
  globalThis.api = async (path, options = {}) => {
    const method = (options.method || 'GET').toUpperCase();
    let body = null;
    if (options.body) { try { body = JSON.parse(options.body); } catch (_) { body = options.body; } }
    requests.push({ path, method, body });
    if (path === '/integrations') {
      return { items: [
        { id: 'home-assistant', name: 'Home Assistant', enabled: homeAssistant },
        { id: 'spotify', name: 'Spotify', enabled: false, installed: false }
      ] };
    }
    if (path === '/assistant') {
      if (!assistant) throw new Error('Voice assistant service is unavailable');
      return assistant;
    }
    if (path === '/voice-pipeline') return { mode: 'local', stt: {}, tts: {} };
    if (path === '/live') {
      if (live && live.unsupported) throw new Error(live.unsupported);
      return live || liveFixture();
    }
    if (path === '/privacy') return {};
    throw new Error('unexpected API path ' + path);
  };
  timers = [];
  await integrationsPage();
  return content.innerHTML;
}

function writes(path) { return requests.filter(r => r.path === path && r.method !== 'GET'); }
function enableRequests() {
  return requests.filter(r => r.method !== 'GET' && r.body && r.body.enabled === true);
}
function assertNoEnableWhileSignedOut() {
  assert.deepEqual(enableRequests().map(r => r.path), [],
    'a signed-out account must not produce an enable request from the browser');
}

/* ------------------------------------------------------------------------ */

(async () => {
  state.page = 'Integrations';

  /* 1. Signed out: both ChatGPT-backed switches are off, the account is one
     sign-in, and the panels are open so the control is reachable. */
  await page({ assistant: assistantFixture() });
  assert(stateLabel('use-device-provider', 'Sign in required'),
    'the device assistant must report "Sign in required" while signed out');
  assert(toggleDisabled('use-device-provider'),
    'the device assistant switch must be disabled while the account is signed out');
  assert(panelOpen('use-device-provider'),
    'the device panel must be open while signed out so Connect is reachable');
  assert(!toggleDisabled('use-local-provider'),
    'Local LLM must not be gated on the ChatGPT account');
  assert(bound('assistant-auth-start'),
    'Connect ChatGPT was rendered with no handler while the provider was selected');
  assert(!content.innerHTML.includes('id="assistant-auth-poll"'),
    'a signed-out account must offer Connect, not Check sign-in');

  assert(stateLabel('use-live-provider', 'Sign in required'),
    'GPT-Live must report the shared account as "Sign in required"');
  assert(toggleDisabled('use-live-provider'),
    'the GPT-Live switch must be disabled while the account is signed out');
  assert(panelOpen('use-live-provider'),
    'GPT-Live must offer the shared sign-in while signed out');
  assert(bound('live-auth-start'),
    'GPT-Live rendered its sign-in control with no handler');

  /* 2. The account is not the selected provider and not enabled: the sign-in
     controls must still be there, and the local provider must be untouched. */
  await page({ assistant: assistantFixture({ provider: 'openai-compatible', base_url: 'http://192.0.2.10:8000/v1' }) });
  assert(bound('assistant-auth-start'),
    'Connect ChatGPT must not require the device assistant to be selected or enabled');
  assert(panelOpen('use-device-provider'),
    'the signed-out account panel must stay open even when another provider is selected');
  assert(!content.innerHTML.includes('id="assistant-clock-format"'),
    'the device settings form must stay out of the unselected provider body');
  assert(!toggleDisabled('use-local-provider'),
    'Local LLM must remain switchable while the ChatGPT account is signed out');

  /* Starting device login is a POST to the account endpoint -- never an enable. */
  await document.querySelector('#assistant-auth-start').onclick();
  assert(writes('/assistant/auth/start').length === 1,
    'Connect ChatGPT did not start device login');
  assertNoEnableWhileSignedOut();

  /* 3. Waiting for sign-in: the code and link are on screen, the panel is
     open, and the poll's re-render must not take either away. */
  await page({ assistant: assistantFixture({ auth_state: 'waiting',
    user_code: 'WXYZ-4242', verification_url: 'https://chatgpt.com/device' }) });
  assert(panelOpen('use-device-provider'), 'the panel collapsed while a device code was showing');
  assert(content.innerHTML.includes('WXYZ-4242'),
    'the device code was not rendered while waiting for sign-in');
  assert(content.innerHTML.includes('https://chatgpt.com/device'),
    'the verification link was not rendered while waiting for sign-in');
  assert(stateLabel('use-device-provider', 'Waiting for sign-in'),
    'the device assistant must report "Waiting for sign-in"');
  assert(bound('assistant-auth-poll'), 'Check sign-in was rendered with no handler');
  assert(!content.innerHTML.includes('id="assistant-auth-start"'),
    'Connect and Check sign-in must not both be offered for one pending login');

  const poll = timers.find(t => t.ms === 3000);
  assert(poll, 'a pending device login must be polled so the code stays valid');
  await poll.fn();
  assert(writes('/assistant/auth/poll').length === 1,
    'the scheduled poll did not check the sign-in state');
  assert(content.innerHTML.includes('WXYZ-4242'),
    'the auth-poll re-render dropped the device code the user was told to enter');
  assert(content.innerHTML.includes('https://chatgpt.com/device'),
    'the auth-poll re-render dropped the verification link');
  assert(panelOpen('use-device-provider'),
    'the auth-poll re-render collapsed the panel holding the device code');
  assertNoEnableWhileSignedOut();

  /* The poll belongs to the account, not to the selected provider: a pending
     login started from either panel survives a local-provider selection. */
  await page({ assistant: assistantFixture({ provider: 'openai-compatible',
    base_url: 'http://192.0.2.10:8000/v1', auth_state: 'waiting',
    user_code: 'WXYZ-4242', verification_url: 'https://chatgpt.com/device' }) });
  assert(timers.some(t => t.ms === 3000),
    'the pending login stopped being polled once another provider was selected');
  assert(bound('assistant-auth-poll'),
    'Check sign-in lost its handler when another provider was selected');

  /* 4. Signed in but switched off: the switch works, the panels close again,
     and the states are reported distinctly from "Sign in required". */
  await page({ assistant: assistantFixture({ authenticated: true, auth_state: 'signed_in' }) });
  assert(stateLabel('use-device-provider', 'Disabled'),
    'a signed-in, switched-off assistant must report "Disabled"');
  assert(!stateLabel('use-device-provider', 'Sign in required'),
    'a signed-in account must not be reported as requiring sign-in');
  assert(!toggleDisabled('use-device-provider'),
    'the device assistant switch must be usable once the account is signed in');
  assert(!panelOpen('use-device-provider'),
    'the panel must not stay forced open once the account is signed in');
  assert(!content.innerHTML.includes('id="assistant-auth-start"') &&
    !content.innerHTML.includes('id="assistant-auth-poll"'),
    'a completed sign-in must not still offer Connect or Check sign-in');
  assert(bound('assistant-logout'), 'Disconnect was rendered with no handler');
  assert(stateLabel('use-live-provider', 'Disabled'),
    'a signed-in, switched-off GPT-Live must report "Disabled"');
  assert(!toggleDisabled('use-live-provider'),
    'the GPT-Live switch must be usable once the account is signed in');

  /* 5. Enabled: both switches agree with the daemon. */
  await page({ assistant: assistantFixture({ authenticated: true, auth_state: 'signed_in', enabled: true }),
    live: liveFixture({ enabled: true }) });
  assert(stateLabel('use-device-provider', 'Enabled'),
    'an enabled device assistant must report "Enabled"');
  assert(toggleChecked('use-device-provider'), 'an enabled assistant must render a checked switch');
  assert(stateLabel('use-live-provider', 'Enabled'), 'an enabled GPT-Live must report "Enabled"');
  assert(toggleChecked('use-live-provider'), 'an enabled GPT-Live must render a checked switch');

  /* 6. GPT-Live's own service is a separate axis: a missing daemon is not an
     authentication failure, and a signed-out account is not a missing daemon. */
  await page({ assistant: assistantFixture(), live: { unsupported: 'GPT-Live is not installed in this image' } });
  assert(stateLabel('use-live-provider', 'Unavailable'),
    'a missing GPT-Live service must report "Unavailable"');
  assert(!stateLabel('use-live-provider', 'Sign in required'),
    'a missing GPT-Live service must not be reported as an authentication problem');
  assert(toggleDisabled('use-live-provider'),
    'the GPT-Live switch must be disabled while its service is unavailable');
  assert(!panelOpen('use-live-provider'),
    'an unavailable service must not force its panel open');
  assert(stateLabel('use-device-provider', 'Sign in required') &&
    panelOpen('use-device-provider'),
    'the signed-out account must still be reported and reachable on its own panel');

  /* 7. The prerequisite holds even if a change event arrives anyway: a
     disabled switch is not the only path into these setters. */
  await page({ assistant: assistantFixture() });
  assert(changed('use-device-provider'), 'the device assistant switch was rendered with no handler');
  document.querySelector('#use-device-provider').checked = true;
  await document.querySelector('#use-device-provider').onchange();
  assertNoEnableWhileSignedOut();
  assert(changed('use-live-provider'), 'the GPT-Live switch was rendered with no handler');
  document.querySelector('#use-live-provider').checked = true;
  await document.querySelector('#use-live-provider').onchange();
  assertNoEnableWhileSignedOut();

  /* 8. Local LLM stays independent: enabling it while ChatGPT is signed out
     still reaches the local provider. */
  await page({ assistant: assistantFixture({ provider: 'openai-compatible', base_url: 'http://192.0.2.10:8000/v1' }) });
  document.querySelector('#use-local-provider').checked = true;
  await document.querySelector('#use-local-provider').onchange();
  const localEnable = writes('/assistant').find(r => r.body && r.body.enabled === true);
  assert(localEnable, 'enabling Local LLM while signed out must still reach the daemon');
  assert.equal(localEnable.body.provider, 'openai-compatible',
    'enabling Local LLM sent the wrong provider');
  assert(writes('/voice-pipeline').length >= 1,
    'enabling Local LLM did not select the custom voice pipeline');

  /* 9. A credential that expired after the assistant was switched on leaves
     the daemon reporting enabled with authenticated false: that is still a
     sign-in problem, and saving the panel must not re-send the enable. */
  await page({ assistant: assistantFixture({ authenticated: false, auth_state: 'signed_out', enabled: true }) });
  assert(stateLabel('use-device-provider', 'Sign in required'),
    'an expired credential must be reported as "Sign in required", not as a running assistant');
  assert(toggleDisabled('use-device-provider') && toggleChecked('use-device-provider'),
    'a stale enable must render as a disabled, checked switch while signed out');
  assert(panelOpen('use-device-provider'),
    'the panel must be open while the expired credential needs a sign-in');
  document.querySelector('#assistant-model').value = 'gpt-5.4';
  document.querySelector('#assistant-clock-format').value = '12';
  document.querySelector('#assistant-prompt').value = 'Reply briefly.';
  /* The save handler re-renders the whole app through mutate(); this case is
     about the payload it sends, so the re-render is stubbed out. */
  globalThis.render = async () => {};
  await document.querySelector('#save-assistant').onclick();
  const staleSave = writes('/assistant').find(r => r.body && r.body.prompt === 'Reply briefly.');
  assert(staleSave, 'saving the device panel did not reach /assistant');
  assert.equal(staleSave.body.enabled, false,
    'saving the device panel while signed out re-sent an enable the daemon cannot honour');
  assertNoEnableWhileSignedOut();

  console.log('ChatGPT account gate: switches, sign-in reachability, panel persistence and states are ok');
})().catch(error => { console.error(error); process.exitCode = 1; });
