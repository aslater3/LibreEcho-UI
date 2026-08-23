function bluetoothDeviceCard(device, known) {
  const actionText = device.connected ? 'Disconnect' : known ? 'Connect / pair' : 'Pair';
  const operation = device.connected ? 'disconnect' : 'pair';
  const name = device.name && device.name !== 'Unknown device' ? device.name : 'Unnamed device';
  const signal = device.rssi_valid ? `${device.rssi} dBm` : 'RSSI unavailable';
  return `<div class="wifi-network bluetooth-device"><span><strong>${esc(name)}</strong><small>${esc(device.address)} · ${signal}${device.connected ? ' · Connected' : ''}</small></span><span class="button-row"><button class="secondary-btn" data-bt-operation="${operation}" data-bt-address="${esc(device.address)}" data-bt-type="${device.type||0}">${actionText}</button>${known ? `<button class="danger-btn" data-bt-operation="unpair" data-bt-address="${esc(device.address)}" data-bt-type="${device.type||0}">Unpair</button>` : ''}</span></div>`;
}

function bluetoothPasskey(value) {
  const numeric=Number(value);
  return Number.isInteger(numeric) && numeric >= 0 && numeric <= 999999 ? String(numeric).padStart(6,'0') : String(value ?? '');
}

function bluetoothPairingCode(value) {
  return `<div class="pairing-code" aria-hidden="true"><span>Pairing code</span><strong class="pairing-code-value">${esc(bluetoothPasskey(value))}</strong></div>`;
}

function updateBluetoothLiveRegion(b) {
  let live=document.getElementById('bt-pairing-live');
  if(!live){live=document.createElement('div');live.id='bt-pairing-live';live.setAttribute('role','status');live.setAttribute('aria-live','polite');live.setAttribute('aria-atomic','true');live.className='sr-only';document.body.appendChild(live);}
  const p=b&&b.pending_pairing;
  const value=b&&b.pairing&&p&&p.address&&((p.method==='confirm'||p.method==='notify')&&p.value!==undefined&&p.value!==null)?`${p.method==='notify'?'Passkey notification':'Pairing confirmation'}: ${bluetoothPasskey(p.value)}`:'';
  if(live.dataset.value!==value){live.textContent=value;live.dataset.value=value;}
}

function bluetoothPending(b) {
  const p=b.pending_pairing;
  if (!b.pairing || !p || !p.address) return '<p class="muted">No pairing request is waiting for a response.</p>';
  const hasPairingCode=(p.method==='confirm'||p.method==='notify') && p.value !== undefined && p.value !== null;
  const pairingCode=hasPairingCode ? bluetoothPairingCode(p.value) : '';
  if (p.method==='notify') return `<div class="pairing-request"><div class="pairing-request-heading"><strong>Passkey notification</strong><small>${esc(p.address)}</small></div>${pairingCode}<p class="muted">Displayed by the remote device. Waiting for completion.</p></div>`;
  const buttons=p.method==='confirm' ? `${action('Confirm pairing','bt-confirm','primary-btn')}${action('Reject','bt-reject','danger-btn')}` : p.method==='passkey' ? `${field('Passkey','', 'bt-pairing-value','number','min="0" max="999999" inputmode="numeric"')}${action('Send passkey','bt-send-passkey','primary-btn')}${action('Reject','bt-reject','danger-btn')}` : p.method==='pin' ? `${field('PIN','', 'bt-pairing-pin','password','maxlength="16" autocomplete="off"')}${action('Send PIN','bt-send-pin','primary-btn')}${action('Reject','bt-reject','danger-btn')}` : `${action('Reject','bt-reject','danger-btn')}`;
  const title=p.method==='confirm'?'Confirm pairing':p.method==='passkey'?'Enter passkey':p.method==='pin'?'Enter PIN':'Pairing request';
  return `<div class="pairing-request"><div class="pairing-request-heading"><strong>${esc(title)}</strong><small>${esc(p.address)}</small></div>${pairingCode}</div><div class="button-row">${buttons}</div>`;
}

function bluetoothMarkup(b) {
  const caps=b.capabilities||{},profiles=b.profile_services||{};
  const pairing=!!b.pairing_mode;
  return `<div class="settings-grid"><section class="panel setting-panel"><h3>Bluetooth adapter</h3>${toggle('Bluetooth enabled',!!b.enabled,'bt-enabled')}${toggle('Accept incoming connections',!!caps.connectable,'bt-connectable',!b.enabled)}${toggle('Visible to nearby devices',!!caps.discoverable,'bt-discoverable',!b.enabled)}<dl class="facts"><dt>State</dt><dd class="${b.enabled?'connected':''}">${esc(b.state||'Unavailable')}</dd><dt>Pairing mode</dt><dd class="${pairing?'connected':''}">${pairing?'Active':'Off'}</dd><dt>Controller</dt><dd>${esc(b.hci||'—')}</dd><dt>Transport</dt><dd>${esc(b.transport||'—')}</dd><dt>Local name</dt><dd>${esc(b.local_name||'—')}</dd></dl><div class="button-row">${action(pairing?'Exit pairing mode':'Enter pairing mode','bt-pairing-mode',pairing?'danger-btn':'primary-btn')}${action(b.scanning?'Scanning…':'Scan for devices','bt-scan','secondary-btn')}${b.scanning?action('Stop scan','bt-scan-stop'):''}</div><p class="muted">Pairing mode automatically enables incoming connections, visibility, and bonding. You do not need to enable those switches first.</p>${!b.available?unsupported(b.last_error||'The MT8163 Bluetooth controller is not available in this image.') : ''}</section>${panel('Controller capabilities',`<dl class="facts"><dt>Classic Bluetooth</dt><dd class="${caps.classic?'connected':''}">${caps.classic?'Available':'Unavailable'}</dd><dt>Bluetooth LE</dt><dd class="${caps.le?'connected':''}">${caps.le?'Available':'Unavailable'}</dd><dt>Secure Simple Pairing</dt><dd class="${caps.ssp?'connected':''}">${caps.ssp?'Available':'Unavailable'}</dd><dt>Secure connections</dt><dd class="${caps.secure_connection?'connected':''}">${caps.secure_connection?'Available':'Unavailable'}</dd><dt>Service profile</dt><dd class="${b.profile_state==='ready'?'connected':''}">${esc(b.profile_state||'Unknown')}</dd><dt>SDP</dt><dd class="${profiles.sdp?'connected':''}">${profiles.sdp?'Registered':'Unavailable'}</dd><dt>A2DP sink</dt><dd class="${profiles.a2dp_sink?'connected':''}">${profiles.a2dp_sink?'Registered':'Unavailable'}</dd><dt>AVRCP</dt><dd class="${profiles.avrcp?'connected':''}">${profiles.avrcp?'Registered':'Unavailable'}</dd><dt>RFCOMM</dt><dd class="${profiles.rfcomm?'connected':''}">${profiles.rfcomm?'Registered':'Unavailable'}</dd></dl><p class="muted">${esc(b.profile_error||'Profile service status unavailable')}</p>`)}${panel('Pairing request',bluetoothPending(b),'wide')}${panel('Nearby devices',b.discovered?.length?b.discovered.map(x=>bluetoothDeviceCard(x,false)).join(''):'<p class="muted">No devices discovered. Start a scan to look for nearby controllers, speakers, headphones, keyboards or other peripherals.','wide')}${panel('Known devices',b.known_devices?.length?b.known_devices.map(x=>bluetoothDeviceCard(x,true)).join(''):'<p class="muted">No paired devices are stored on this device.','wide')}</div>`;
}

async function respondBluetoothPairing(p, method, extra={}) {
  if (state.btPairingResponding) return;
  state.btPairingResponding=true;
  clearTimeout(state.timer);
  $$('#bt-confirm,#bt-reject,#bt-send-passkey,#bt-send-pin').forEach(button=>button.disabled=true);
  try {
    await api('/bluetooth/pairing/response',{method:'POST',body:JSON.stringify({address:p.address,type:p.type||0,method,...extra})});
    toast(method==='confirm'?'Pairing confirmation sent':method==='reject'?'Pairing rejected':'Pairing response sent');
  } catch (error) {
    toast(error.message,true);
  } finally {
    state.btPairingResponding=false;
    if (state.page==='Bluetooth') await bluetoothPage();
  }
}

function bindBluetooth(b) {
  $('#bt-enabled').onchange=()=>mutate('/bluetooth',{enabled:$('#bt-enabled').checked},'Bluetooth power state updated');
  $('#bt-connectable').onchange=()=>mutate('/bluetooth',{connectable:$('#bt-connectable').checked},'Incoming connection setting updated');
  $('#bt-discoverable').onchange=()=>mutate('/bluetooth',{discoverable:$('#bt-discoverable').checked},'Visibility setting updated');
  if ($('#bt-pairing-mode')) $('#bt-pairing-mode').onclick=()=>post('/bluetooth/pairing-mode',{enabled:!b.pairing_mode},b.pairing_mode?'Pairing mode ended':'Pairing mode active — device is visible');
  $('#bt-scan').onclick=()=>post('/bluetooth/scan',{},'Bluetooth discovery started');
  if ($('#bt-scan-stop')) $('#bt-scan-stop').onclick=()=>post('/bluetooth/scan/stop',{},'Bluetooth discovery stopped');
  $$('[data-bt-operation]').forEach(button=>button.onclick=()=>{const operation=button.dataset.btOperation,address=button.dataset.btAddress,type=Number(button.dataset.btType||0);if(operation==='unpair'&&!confirm(`Unpair ${address}?`))return;post(`/bluetooth/${operation}`,{address,type},operation==='unpair'?'Device unpaired':operation==='disconnect'?'Device disconnected':'Pairing started')});
  if ($('#bt-confirm')) $('#bt-confirm').onclick=()=>respondBluetoothPairing(b.pending_pairing,'confirm');
  if ($('#bt-reject')) $('#bt-reject').onclick=()=>respondBluetoothPairing(b.pending_pairing,'reject');
  if ($('#bt-send-passkey')) $('#bt-send-passkey').onclick=()=>respondBluetoothPairing(b.pending_pairing,'passkey',{value:Number($('#bt-pairing-value').value)});
  if ($('#bt-send-pin')) $('#bt-send-pin').onclick=()=>respondBluetoothPairing(b.pending_pairing,'pin',{pin:$('#bt-pairing-pin').value});
}

async function bluetoothPage() {
  const b=await api('/bluetooth');
  content.innerHTML=bluetoothMarkup(b);
  bindBluetooth(b);
  updateBluetoothLiveRegion(b);
  state.timer=setTimeout(refreshBluetooth,2000);
}

async function refreshBluetooth() {
  if (state.page!=='Bluetooth') return;
  if (state.btPairingResponding) return;
  try {
    const b=await api('/bluetooth');
    if (state.page==='Bluetooth' && !state.btPairingResponding) { content.innerHTML=bluetoothMarkup(b); bindBluetooth(b); updateBluetoothLiveRegion(b); }
  } catch (_) { /* Preserve the last good Bluetooth state during a transient adapter failure. */ }
  finally { if (state.page==='Bluetooth') state.timer=setTimeout(refreshBluetooth,2000); }
}
