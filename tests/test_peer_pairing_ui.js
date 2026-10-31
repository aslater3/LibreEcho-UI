/* Device pairing panel and peer trust labels (#450). Runs the real helper
 * functions from web/js/app.js in a sandbox. */
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const source = fs.readFileSync(path.resolve(__dirname, '..', 'web/js/app.js'), 'utf8');
function pick(pattern) {
    const line = source.split('\n').find(l => pattern.test(l));
    assert.ok(line, `app.js has ${pattern}`);
    return line;
}
const code = [/^function esc\(/, /^function field\(/, /^function action\(/, /^const PEER_TRUST=/, /^function keyId\(/,
    /^function pairingBox\(/, /^function peerRow\(/].map(pick).join('\n');
const sandbox = {};
vm.createContext(sandbox);
vm.runInContext(code + '\nthis.pairingBox=pairingBox;this.peerRow=peerRow;this.keyId=keyId;', sandbox);
const { pairingBox, peerRow, keyId } = sandbox;

assert.strictEqual(keyId('a1b2c3d4'), 'a1b2-c3d4');
assert.strictEqual(keyId(''), '');

let html = pairingBox({ supported: false, paired: false, key_id: '' });
assert.ok(/saved configuration/.test(html) && !/pair-create/.test(html));

html = pairingBox({ supported: true, paired: false, key_id: '' });
assert.ok(/id="pair-create"/.test(html) && /id="pair-code"/.test(html) && /id="pair-join"/.test(html));
assert.ok(!/pair-remove/.test(html));

html = pairingBox({ supported: true, paired: true, key_id: 'a1b2c3d4' });
assert.ok(/a1b2-c3d4/.test(html) && /id="pair-show"/.test(html) && /id="pair-remove"/.test(html));
assert.ok(!/pair-code-shown/.test(html), 'the code is only shown on request');

html = pairingBox({ supported: true, paired: true, key_id: 'a1b2c3d4' }, '0011-<b>');
assert.ok(/id="pair-code-shown">0011-&lt;b&gt;</.test(html), 'code is escaped');
assert.ok(!/pair-show/.test(html));

const base = { name: 'kitchen', room: 'Kitchen', version: '0.14.6', address: '192.0.2.20', port: 8080, url: 'http://192.0.2.20:8080/', self: false };
assert.ok(/Kitchen · LibreEcho 0\.14\.6 · Paired</.test(peerRow({ ...base, trust: 'paired' })));
assert.ok(/· Not paired</.test(peerRow({ ...base, trust: 'unpaired' })));
assert.ok(/· Could not verify</.test(peerRow({ ...base, trust: 'failed' })));
assert.ok(!/Paired|verify/.test(peerRow({ ...base, trust: 'off' })));
assert.ok(!/Paired|verify/.test(peerRow({ ...base })), 'older daemons send no trust');
assert.ok(!/Paired/.test(peerRow({ ...base, self: true, trust: 'self' })));
console.log('peer pairing ui: ok');
