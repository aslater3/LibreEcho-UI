function bluetoothDeviceCard(device, known) {
  const actionText = device.connected ? 'Disconnect' : known ? 'Connect / pair' : 'Pair';
  const actionId = device.connected ? 'bt-disconnect' : 'bt-pair';
  const operation = device.connected ? 'disconnect' : 'pair';
  return `<div class="wifi-network bluetooth-device"><span><strong>${esc(device.name||'Unknown device')}</strong><small>${esc(device.address)} · ${device.rssi ? `${device.rssi} dBm` : 'RSSI unavailable'}${device.connected ? ' · Connected' : ''}</small></span><span class="button-row"><button class="secondary-btn" data-bt-operation="${operation}" data-bt-address="${esc(device.address)}" data-bt-type="${device.type||0}">${actionText}</button>${known ? `<button class="danger-btn" data-bt-operation="unpair" data-bt-address="${esc(device.address)}" data-bt-type="${device.type||0}">Unpair</button>` : ''}</span></div>`;
}

function bluetoothPasskey(value) {
  const numeric=Number(value);
  return Number.isInteger(numeric) && numeric >= 0 && numeric <= 999999 ? String(numeric).padStart(6,'0') : String(value ?? '');
}

function bluetoothPending(b) {
  const p=b.pending_pairing;
  if (!b.pairing || !p || !p.address) return '<p class="muted">No pairing request is waiting for a response.</p>';
  if (p.method==='notify') return `<div class="privacy-callout">Passkey ${esc(bluetoothPasskey(p.value))} displayed by the remote device. Waiting for completion.</div>`;
  const buttons=p.method==='confirm' ? `${action('Confirm pairing','bt-confirm','primary-btn')}${action('Reject','bt-reject','danger-btn')}` : p.method==='passkey' ? `${field('Passkey','', 'bt-pairing-value','number','min="0" max="999999" inputmode="numeric"')}${action('Send passkey','bt-send-passkey','primary-btn')}${action('Reject','bt-reject','danger-btn')}` : p.method==='pin' ? `${field('PIN','', 'bt-pairing-pin','password','maxlength="16" autocomplete="off"')}${action('Send PIN','bt-send-pin','primary-btn')}${action('Reject','bt-reject','danger-btn')}` : `${action('Reject','bt-reject','danger-btn')}`;
  return `<div class="status-line"><span class="status-dot ok"></span><div><strong>${esc(p.method==='confirm'?'Confirm pairing':p.method==='passkey'?'Enter passkey':p.method==='pin'?'Enter PIN':'Pairing request')}</strong><small>${esc(p.address)}${p.value !== undefined && p.value !== null ? ` · ${bluetoothPasskey(p.value)}` : ''}</small></div></div><div class="button-row">${buttons}</div>`;
}

function bluetoothMarkup(b) {
  const caps=b.capabilities||{},profiles=b.profile_services||{};
  const pairing=!!b.pairing_mode;
  return `<div class="settings-grid"><section class="panel setting-panel"><h3>Bluetooth adapter</h3>${toggle('Bluetooth enabled',!!b.enabled,'bt-enabled')}${toggle('Accept incoming connections',!!caps.connectable,'bt-connectable',!b.enabled)}${toggle('Visible to nearby devices',!!caps.discoverable,'bt-discoverable',!b.enabled)}<dl class="facts"><dt>State</dt><dd class="${b.enabled?'connected':''}">${esc(b.state||'Unavailable')}</dd><dt>Pairing mode</dt><dd class="${pairing?'connected':''}">${pairing?'Active':'Off'}</dd><dt>Controller</dt><dd>${esc(b.hci||'—')}</dd><dt>Transport</dt><dd>${esc(b.transport||'—')}</dd><dt>Local name</dt><dd>${esc(b.local_name||'—')}</dd></dl><div class="button-row">${action(pairing?'Exit pairing mode':'Enter pairing mode','bt-pairing-mode',pairing?'danger-btn':'primary-btn')}${action(b.scanning?'Scanning…':'Scan for devices','bt-scan','secondary-btn')}${b.scanning?action('Stop scan','bt-scan-stop'):''}</div><p class="muted">Pairing mode automatically enables incoming connections, visibility, and bonding. You do not need to enable those switches first.</p>${!b.available?unsupported(b.last_error||'The MT8163 Bluetooth controller is not available in this image.') : ''}</section>${panel('Controller capabilities',`<dl class="facts"><dt>Classic Bluetooth</dt><dd class="${caps.classic?'connected':''}">${caps.classic?'Available':'Unavailable'}</dd><dt>Bluetooth LE</dt><dd class="${caps.le?'connected':''}">${caps.le?'Available':'Unavailable'}</dd><dt>Secure Simple Pairing</dt><dd class="${caps.ssp?'connected':''}">${caps.ssp?'Available':'Unavailable'}</dd><dt>Secure connections</dt><dd class="${caps.secure_connection?'connected':''}">${caps.secure_connection?'Available':'Unavailable'}</dd><dt>Service profile</dt><dd class="${b.profile_state==='ready'?'connected':''}">${esc(b.profile_state||'Unknown')}</dd><dt>SDP</dt><dd class="${profiles.sdp?'connected':''}">${profiles.sdp?'Registered':'Unavailable'}</dd><dt>A2DP sink</dt><dd class="${profiles.a2dp_sink?'connected':''}">${profiles.a2dp_sink?'Registered':'Unavailable'}</dd><dt>AVRCP</dt><dd class="${profiles.avrcp?'connected':''}">${profiles.avrcp?'Registered':'Unavailable'}</dd><dt>RFCOMM</dt><dd class="${profiles.rfcomm?'connected':''}">${profiles.rfcomm?'Registered':'Unavailable'}</dd></dl><p class="muted">${esc(b.profile_error||'Profile service status unavailable')}</p>`)}${panel('Pairing request',bluetoothPending(b),'wide')}${panel('Nearby devices',b.discovered?.length?b.discovered.map(x=>bluetoothDeviceCard(x,false)).join(''):'<p class="muted">No devices discovered. Start a scan to look for nearby controllers, speakers, headphones, keyboards or other peripherals.','wide')}${panel('Known devices',b.known_devices?.length?b.known_devices.map(x=>bluetoothDeviceCard(x,true)).join(''):'<p class="muted">No paired devices are stored on this device.','wide')}</div>`;
}

function bindBluetooth(b) {
  $('#bt-enabled').onchange=()=>mutate('/bluetooth',{enabled:$('#bt-enabled').checked},'Bluetooth power state updated');
  $('#bt-connectable').onchange=()=>mutate('/bluetooth',{connectable:$('#bt-connectable').checked},'Incoming connection setting updated');
  $('#bt-discoverable').onchange=()=>mutate('/bluetooth',{discoverable:$('#bt-discoverable').checked},'Visibility setting updated');
  if ($('#bt-pairing-mode')) $('#bt-pairing-mode').onclick=()=>post('/bluetooth/pairing-mode',{enabled:!b.pairing_mode},b.pairing_mode?'Pairing mode ended':'Pairing mode active — device is visible');
  $('#bt-scan').onclick=()=>post('/bluetooth/scan',{},'Bluetooth discovery started');
  if ($('#bt-scan-stop')) $('#bt-scan-stop').onclick=()=>post('/bluetooth/scan/stop',{},'Bluetooth discovery stopped');
  $$('[data-bt-operation]').forEach(button=>button.onclick=()=>{const operation=button.dataset.btOperation,address=button.dataset.btAddress,type=Number(button.dataset.btType||0);if(operation==='unpair'&&!confirm(`Unpair ${address}?`))return;post(`/bluetooth/${operation}`,{address,type},operation==='unpair'?'Device unpaired':operation==='disconnect'?'Device disconnected':'Pairing started')});
  if ($('#bt-confirm')) $('#bt-confirm').onclick=()=>post('/bluetooth/pairing/response',{address:b.pending_pairing.address,type:b.pending_pairing.type||0,method:'confirm'},'Pairing confirmed');
  if ($('#bt-reject')) $('#bt-reject').onclick=()=>post('/bluetooth/pairing/response',{address:b.pending_pairing.address,type:b.pending_pairing.type||0,method:'reject'},'Pairing rejected');
  if ($('#bt-send-passkey')) $('#bt-send-passkey').onclick=()=>post('/bluetooth/pairing/response',{address:b.pending_pairing.address,type:b.pending_pairing.type||0,method:'passkey',value:Number($('#bt-pairing-value').value)},'Passkey sent');
  if ($('#bt-send-pin')) $('#bt-send-pin').onclick=()=>post('/bluetooth/pairing/response',{address:b.pending_pairing.address,type:b.pending_pairing.type||0,method:'pin',pin:$('#bt-pairing-pin').value},'PIN sent');
}

async function bluetoothPage() {
  const b=await api('/bluetooth');
  content.innerHTML=bluetoothMarkup(b);
  bindBluetooth(b);
  state.timer=setTimeout(refreshBluetooth,2000);
}

async function refreshBluetooth() {
  if (state.page!=='Bluetooth') return;
  try {
    const b=await api('/bluetooth');
    if (state.page==='Bluetooth') { content.innerHTML=bluetoothMarkup(b); bindBluetooth(b); }
  } catch (_) { /* Preserve the last good Bluetooth state during a transient adapter failure. */ }
  finally { if (state.page==='Bluetooth') state.timer=setTimeout(refreshBluetooth,2000); }
}
