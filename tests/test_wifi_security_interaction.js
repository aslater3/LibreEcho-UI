/* Exercise the real setup and network UI handlers, not only source contracts. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function classList() {
    return { add() {}, remove() {}, toggle() {} };
}

function storage() {
    const values = new Map();
    return {
        getItem: key => values.get(key) || null,
        setItem: (key, value) => values.set(key, String(value)),
        removeItem: key => values.delete(key)
    };
}

function element(id = '') {
    return {
        id, innerHTML: '', textContent: '', value: '', disabled: false,
        hidden: false, required: false, dataset: {}, classList: classList(),
        style: { setProperty() {} }, onclick: null, onchange: null,
        addEventListener() {}, appendChild() {}, after() {}, focus() {},
        querySelector() { return null; }, querySelectorAll() { return []; },
        setAttribute() {}
    };
}

function button(classes, dataset) {
    const b = element();
    b.className = classes;
    b.dataset = dataset;
    b.closest = selector => selector === '.wifi-option' ? b.option : null;
    return b;
}

function installWifiMarkup(list) {
    let children = [];
    Object.defineProperty(list, 'innerHTML', {
        configurable: true,
        get: () => list._html || '',
        set: html => {
            list._html = html;
            children = [];
            const options = [...html.matchAll(
                /<div class="wifi-option[^>]*>([\s\S]*?)<\/div>/g
            )];
            for (const optionMatch of options) {
                const option = element();
                option.classList = classList();
                const selectMatch = optionMatch[1].match(
                    /class="wifi-select" data-ssid="([^"]*)" data-security="([^"]*)"/
                );
                if (!selectMatch) continue;
                const select = button('wifi-select', {
                    ssid: selectMatch[1], security: selectMatch[2]
                });
                select.option = option;
                option.querySelector = selector => selector === '.wifi-select' ? select : null;
                option.children = [select];
                children.push(select);
                const fallbackMatch = optionMatch[1].match(
                    /class="secondary compact wifi-wpa2"/
                );
                if (fallbackMatch) {
                    const fallback = button('secondary compact wifi-wpa2', {});
                    fallback.option = option;
                    option.children.push(fallback);
                    children.push(fallback);
                }
            }
        }
    });
    list.querySelectorAll = selector => children.filter(child =>
        selector === '.wifi-select' ? child.className === 'wifi-select' :
        selector === '.wifi-wpa2' ? child.className.includes('wifi-wpa2') : false
    );
    list.after = created => { list.afterElement = created; };
}

async function testSetupUi() {
    const elements = new Map();
    const wifiList = element('wifi-list');
    installWifiMarkup(wifiList);
    elements.set('wifi-list', wifiList);
    for (const id of [
        'setup-ssid', 'setup-security', 'setup-password', 'password-field', 'setup-error',
        'scan-wifi', 'setup-back', 'setup-next', 'step-count', 'setup-actions',
        'force-vendor-import', 'show-password', 'setup-volume', 'volume-output',
        'setup-sensitivity', 'sensitivity-output', 'setup-hostname',
        'hostname-preview', 'setup-local', 'setup-telemetry'
    ]) elements.set(id, element(id));
    elements.get('setup-security').value = 'wpa2';
    const document = {
        querySelector: selector => selector.startsWith('#') ? elements.get(selector.slice(1)) || null : null,
        querySelectorAll: selector => selector === '.wifi-select' || selector === '.wifi-wpa2' ?
            wifiList.querySelectorAll(selector) : [],
        createElement: tag => element(tag),
        addEventListener() {}
    };
    const calls = [];
    const context = {
        console, document, window: {}, location: { replace() {} },
        sessionStorage: storage(), setTimeout() {}, clearTimeout() {},
        confirm: () => false,
        fetch: async (url) => {
            calls.push(url);
            const data = url.endsWith('/config') ? { csrf_token: 'test', bootstrap_required: true } :
                { networks: [
                    { ssid: 'Transition', security: 'wpa3-transition', capabilities: 'WPA2-PSK, WPA3-SAE', wpa2_attempt: true, signal: 80 },
                    { ssid: 'WPA3 only', security: 'wpa3-only', capabilities: 'WPA3-SAE', wpa2_attempt: false, signal: 70 },
                    { ssid: 'Open', security: 'open', capabilities: 'open', wpa2_attempt: false, signal: 60 }
                ] };
            return { ok: true, json: async () => ({ ok: true, data }) };
        }
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync('web/js/setup.js', 'utf8'), context, { filename: 'setup.js' });
    await Promise.resolve();
    await vm.runInContext('setup.hardwareReady = true; scan()', context);

    const selectButtons = wifiList.querySelectorAll('.wifi-select');
    assert.strictEqual(selectButtons.length, 3);
    await selectButtons[0].onclick();
    assert.strictEqual(elements.get('setup-ssid').value, '',
        'WPA3 transition row must remain informational until Try WPA2');
    assert.strictEqual(elements.get('setup-security').value, 'wpa2');
    await selectButtons[1].onclick();
    assert.strictEqual(elements.get('setup-ssid').value, '',
        'WPA3-only row must remain informational');
    const fallback = wifiList.querySelectorAll('.wifi-wpa2')[0];
    await fallback.onclick();
    assert.strictEqual(elements.get('setup-ssid').value, 'Transition');
    assert.strictEqual(elements.get('setup-security').value, 'wpa2');
}

async function testNetworkUi() {
    const elements = new Map();
    const content = element('content');
    const nav = element('nav');
    const body = element('body');
    const wifiResults = element('wifi-results');
    let children = [];
    let wifiChildren = [];
    Object.defineProperty(wifiResults, 'innerHTML', {
        configurable: true,
        get: () => wifiResults._html || '',
        set: html => {
            wifiResults._html = html;
            wifiChildren = [];
            for (const match of html.matchAll(
                /<button class="([^"]+)" data-ssid="([^"]*)"(?: data-security="([^"]*)")?/g
            )) {
                wifiChildren.push(button(match[1], {
                    ssid: match[2], security: match[3] || ''
                }));
            }
        }
    });
    wifiResults.querySelectorAll = selector => wifiChildren.filter(child =>
        selector === '.wifi-network' ? child.className === 'wifi-network' :
        selector === '.wifi-wpa2' ? child.className.includes('wifi-wpa2') : false
    );
    Object.defineProperty(content, 'innerHTML', {
        configurable: true,
        get: () => content._html || '',
        set: html => {
            content._html = html;
            children = [];
            for (const match of html.matchAll(
                /<button class="([^"]+)" data-ssid="([^"]*)" data-security="([^"]*)"/g
            )) {
                const b = button(match[1], { ssid: match[2], security: match[3] });
                children.push(b);
            }
            for (const match of html.matchAll(
                /<button class="([^"]+)" id="([^"]+)"/g
            )) {
                const b = button(match[1], {});
                b.id = match[2];
                elements.set(b.id, b);
            }
            if (html.includes('id="wifi-results"')) elements.set('wifi-results', wifiResults);
        }
    });
    content.querySelectorAll = selector => selector === '.wifi-network' || selector === '.wifi-wpa2' ?
        wifiResults.querySelectorAll(selector) : children.filter(() => false);
    elements.set('content', content);
    elements.set('nav', nav);
    elements.set('body', body);
    for (const id of ['toast', 'page-title', 'page-subtitle', 'auth-control', 'reboot',
        'theme', 'menu', 'update-available']) elements.set(id, element(id));
    const document = {
        body,
        querySelector: selector => selector.startsWith('#') ? elements.get(selector.slice(1)) || null : null,
        querySelectorAll: selector => content.querySelectorAll(selector),
        createElement: tag => element(tag),
        addEventListener() {}
    };
    const context = {
        console, document, window: { addEventListener() {} },
        location: { pathname: '/network', hash: '' }, history: { pushState() {}, replaceState() {} },
        localStorage: storage(), sessionStorage: storage(), URL: {},
        setTimeout: () => 1, clearTimeout() {}, prompt: () => 'password123',
        fetch: () => new Promise(() => {})
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync('web/js/app.js', 'utf8'), context, { filename: 'app.js' });
    const requests = [];
    context.api = async path => {
        if (path === '/network') return { state: 'connected', connectivity: 'healthy', signal: 80 };
        if (path === '/network/wifi/scan') return { networks: [
            { ssid: 'Transition', security: 'wpa3-transition', capabilities: 'WPA2-PSK, WPA3-SAE', wpa2_attempt: true, signal: 80 },
            { ssid: 'WPA3 only', security: 'wpa3-only', capabilities: 'WPA3-SAE', wpa2_attempt: false, signal: 70 },
            { ssid: 'Open', security: 'open', capabilities: 'open', wpa2_attempt: false, signal: 60 }
        ] };
        throw new Error(`unexpected API path ${path}`);
    };
    context.post = async (path, data) => { requests.push({ path, data }); };
    await context.networkPage();
    await elements.get('wifi-scan').onclick();
    const cards = content.querySelectorAll('.wifi-network');
    assert.strictEqual(cards.length, 3);
    await cards[0].onclick();
    await cards[1].onclick();
    assert.deepStrictEqual(requests, [],
        'unsupported scan security labels must not reach the connect API');
    await content.querySelectorAll('.wifi-wpa2')[0].onclick();
    assert.strictEqual(JSON.stringify(requests), JSON.stringify([{
        path: '/network/wifi/connect', data: { ssid: 'Transition', password: 'password123', security: 'wpa2' }
    }]));
    await cards[2].onclick();
    assert.strictEqual(requests[1].data.security, 'open');
}

(async () => {
    await testSetupUi();
    await testNetworkUi();
    console.log('Wi-Fi security interaction regression: ok');
})().catch(error => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
