'use strict';

/*
 * Run the real LED page against a small DOM and API shim.  This is deliberately
 * not a copy of the gate: ledPage() renders the button, binds the controls, and
 * is re-run by the fake Save handler just as a successful PUT would re-render it.
 */
const assert = require('node:assert/strict');
const fs = require('fs');
const vm = require('vm');

class Element {
  constructor(tag = 'div', attrs = {}) {
    this.tagName = tag.toUpperCase();
    this.id = attrs.id || '';
    this.className = attrs.class || '';
    this.type = attrs.type || '';
    this.value = attrs.value || '';
    this.checked = attrs.checked === true;
    let disabledValue = attrs.disabled === true;
    Object.defineProperty(this, 'disabled', {
      configurable: true,
      get: () => disabledValue,
      set: value => {
        disabledValue = !!value;
      }
    });
    this.title = '';
    this.dataset = {};
    this.listeners = {};
    this.onclick = null;
    this.parentElement = null;
    this.children = [];
    this.appendChild = child => { child.parentElement = this; this.children.push(child); };
    this.classList = {
      add: (...names) => names.forEach(name => {
        if (!this.className.split(/\s+/).includes(name)) this.className += (this.className ? ' ' : '') + name;
      }),
      remove: (...names) => {
        this.className = this.className.split(/\s+/).filter(name => name && !names.includes(name)).join(' ');
      },
      toggle: (name, force) => {
        const has = this.className.split(/\s+/).includes(name);
        const next = force === undefined ? !has : !!force;
        if (next && !has) this.classList.add(name);
        if (!next && has) this.classList.remove(name);
        return next;
      }
    };
    this.style = { setProperty() {} };
  }

  addEventListener(event, handler) {
    (this.listeners[event] ||= []).push(handler);
  }

  dispatchEvent(event) {
    for (const handler of this.listeners[event.type] || []) handler.call(this, event);
  }

  setAttribute(name, value) {
    if (name === 'aria-label') this.ariaLabel = value;
  }

  querySelector(selector) {
    return selector === 'output' ? new Element('output') : null;
  }
}

class Content extends Element {
  constructor() {
    super('main', { id: 'content' });
    this.controls = [];
  }

  set innerHTML(html) {
    this.html = html;
    this.controls = [];
    const tagPattern = /<(button|input|select)\b([^>]*)>/g;
    let match;
    while ((match = tagPattern.exec(html)) !== null) {
      const [, tag, raw] = match;
      const attrs = {};
      for (const attr of raw.matchAll(/([\w-]+)(?:="([^"]*)")?/g)) attrs[attr[1]] = attr[2] ?? true;
      const element = new Element(tag, attrs);
      if (attrs['data-profile']) element.dataset.profile = attrs['data-profile'];
      element.parentElement = this;
      this.controls.push(element);
    }
  }

  querySelector(selector) {
    if (selector.startsWith('#')) return this.controls.find(element => element.id === selector.slice(1)) || null;
    return null;
  }

  querySelectorAll(selector) {
    if (selector === 'button,input,select') return this.controls;
    if (selector === '[data-busy-disabled]') return this.controls.filter(element => element.dataset.busyDisabled);
    if (selector === '.range-field input') return [];
    if (selector === 'input[type=color][data-profile]') return this.controls.filter(element => element.type === 'color' && element.dataset.profile);
    if (selector === '.profile-save') return this.controls.filter(element => element.className.split(/\s+/).includes('profile-save'));
    return [];
  }
}

const content = new Content();
const staticElements = new Map([
  ['nav', new Element('nav', { id: 'nav' })],
  ['toast', new Element('div', { id: 'toast' })],
  ['backend-badge', new Element('span', { id: 'backend-badge' })],
  ['update-available', new Element('button', { id: 'update-available' })],
  ['reboot', new Element('button', { id: 'reboot' })],
  ['theme', new Element('button', { id: 'theme' })],
  ['menu', new Element('button', { id: 'menu' })],
  ['auth-control', new Element('button', { id: 'auth-control' })],
  ['page-title', new Element('h1', { id: 'page-title' })],
  ['page-subtitle', new Element('p', { id: 'page-subtitle' })],
  ['sidebar-version', new Element('span', { id: 'sidebar-version' })]
]);
staticElements.set('content', content);
const body = new Element('body');

globalThis.document = {
  querySelector(selector) {
    if (selector.startsWith('#')) return staticElements.get(selector.slice(1)) || content.querySelector(selector);
    return null;
  },
  querySelectorAll() { return []; },
  createElement: tag => new Element(tag),
  addEventListener() {},
  body,
  title: ''
};
globalThis.window = { addEventListener() {} };
globalThis.location = { pathname: '/', hash: '', host: 'fake-device', replace() {} };
globalThis.history = { pushState() {}, replaceState() {} };
globalThis.sessionStorage = { getItem: () => null, setItem() {}, removeItem() {} };
globalThis.localStorage = { getItem: () => null, setItem() {}, removeItem() {}, clear() {} };
globalThis.fetch = () => new Promise(() => {}); /* keep app startup out of this fixture */
globalThis.setTimeout = () => 0;
globalThis.clearTimeout = () => {};
globalThis.confirm = () => false;
globalThis.prompt = () => '';
globalThis.URL.createObjectURL = () => 'blob:fake';
globalThis.URL.revokeObjectURL = () => {};

vm.runInThisContext(fs.readFileSync('web/js/app.js', 'utf8'), { filename: 'app.js' });
const state = vm.runInThisContext('state');
state.page = 'LED & Buttons';

let led = {
  colour: { r: 12, g: 34, b: 56 },
  brightness: 0,
  visualizer_enabled: true,
  visualizer_active: false,
  profiles: {},
  night: { enabled: false, active: false, start_minute: 1320, end_minute: 420 }
};
let lastPut = null;
globalThis.api = async (path, options = {}) => {
  if (path === '/led' && options.method === 'PUT') {
    lastPut = JSON.parse(options.body);
    led = { ...led, brightness: lastPut.brightness };
    return led;
  }
  if (path === '/led') return led;
  if (path === '/buttons') return {
    short_press: 'Start listening', long_press: 'Open pairing mode', hardware_mute: false,
    action: 'sound', action_brightness: 70, mute_brightness: 60, tones: true
  };
  throw new Error(`unexpected API path ${path}`);
};

async function main() {
  await globalThis.ledPage();
  let testButton = document.querySelector('#led-test');
  const brightness = document.querySelector('#led-brightness');
  const save = document.querySelector('#save-led');

  assert.ok(testButton, 'LED test button should render');
  assert.equal(testButton.disabled, true, 'saved zero brightness disables the LED test');

  brightness.value = '50';
  brightness.dispatchEvent({ type: 'input' });
  assert.equal(save.disabled, false, 'changing the slider marks LED settings dirty');
  assert.equal(testButton.disabled, true, 'an unsaved positive slider does not enable the test');

  await save.onclick();
  await globalThis.ledPage();
  testButton = document.querySelector('#led-test');
  assert.deepEqual(lastPut, {
    r: 12, g: 34, b: 56, brightness: 50, visualizer_enabled: true
  }, 'Save sends the positive brightness to the device');
  assert.equal(testButton.disabled, false, 'saving positive brightness re-enables the LED test');

  console.log('led brightness gate: ok');
}

main().catch(error => { console.error(error); process.exit(1); });
