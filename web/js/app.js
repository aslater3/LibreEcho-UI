'use strict';
const $=(s,r=document)=>r.querySelector(s), $$=(s,r=document)=>[...r.querySelectorAll(s)];
const state={page:'Overview',csrf:'',token:sessionStorage.getItem('libreecho-token')||'',username:sessionStorage.getItem('libreecho-username')||'',authMode:'development-disabled',timer:null,data:{},busy:false,features:{simulation:false}};
const items=[['Overview','home'],['Device','device'],['Users','shield'],['Audio','audio'],['Baby Monitor','mic'],['Wake Word','mic'],['Simulation','mic'],['LED & Buttons','sun'],['Network','wifi'],['Bluetooth','bluetooth'],['Privacy','shield'],['Integrations','puzzle'],['System','gear'],['Logs','log'],['About','info']];
const descriptions={Overview:'Your LibreEcho at a glance',Device:'Identity, hardware and power controls',Users:'Manage local accounts and access',Audio:'Playback, microphone and volume controls','Baby Monitor':'Listen to selected microphones locally','Wake Word':'Configure local wake-word detection',Simulation:'Speak test phrases into the microphone path','LED & Buttons':'Customise light-ring behaviour and controls',Network:'Wi-Fi, addressing and connectivity',Bluetooth:'Scan, pair and manage nearby devices',Privacy:'Local processing and data retention controls',Integrations:'Connect services and home automation',System:'Updates, backup and advanced settings',Logs:'Diagnostics and troubleshooting',About:'Project, licences and version information'};
const nav=$('#nav'),content=$('#content');
document.body.classList.add('auth-pending');
function esc(v){return String(v??'').replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]))}
async function api(path,opt={}){const {allowUnavailable=false,...fetchOpt}=opt;const headers={'Accept':'application/json',...(state.token?{'Authorization':'Bearer '+state.token}:{}),...(fetchOpt.body?{'Content-Type':'application/json','X-LibreEcho-CSRF':state.csrf}:{}),...(fetchOpt.headers||{})};const res=await fetch('/api/v1'+path,{...fetchOpt,headers});let body;try{body=await res.json()}catch(_){throw new Error('The device returned an unreadable response')}if(!res.ok||!body.ok||(body.data?.unavailable===true&&!allowUnavailable))throw new Error(body.error?.message||body.data?.message||`Request failed (${res.status})`);return body.data}
function toast(message,error=false){const t=$('#toast');t.textContent=message;t.classList.toggle('error',error);t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2600)}
function panel(title,body,extra=''){return `<section class="panel setting-panel ${extra}"><h3>${title}</h3>${body}</section>`}
function collapsiblePanel(title,body,extra='',open=false){return `<details class="panel setting-panel integration-section ${extra}"${open?' open':''}><summary><h3>${title}</h3><span class="integration-toggle" aria-hidden="true">Show details</span></summary><div class="integration-section-body">${body}</div></details>`}
function field(label,value,id,type='text',extra=''){return `<label class="field"><span>${label}</span><input id="${id}" type="${type}" value="${esc(value)}" ${extra}></label>`}
function range(label,value,id){return `<label class="field range-field"><span>${label}: <output>${value}%</output></span><input id="${id}" type="range" min="0" max="100" value="${value}"></label>`}
function toggle(label,on,id,disabled=false){return `<label class="switch-row"><span>${label}</span><input class="toggle-input" id="${id}" type="checkbox" ${on?'checked':''} ${disabled?'disabled':''}><span class="switch" aria-hidden="true"></span></label>`}
function select(label,value,id,values){return `<label class="field"><span>${label}</span><select id="${id}">${values.map(v=>`<option ${v===value?'selected':''}>${esc(v)}</option>`).join('')}</select></label>`}
function action(label,id,kind='secondary-btn'){return `<button class="${kind}" id="${id}">${label}</button>`}
function linkAction(label,href,kind='secondary-btn'){return `<a class="${kind} action-link" href="${href}">${label}</a>`}
function saveButton(id){return `<button class="save-btn save-changes" id="${id}" disabled>Save changes</button>`}
function unsupported(msg){return `<div class="notice unsupported"><strong>Not supported</strong><span>${esc(msg)}</span></div>`}
function applyCssVars(root){const r=root||content;r.querySelectorAll('[data-level]').forEach(el=>{el.style.setProperty('--level',el.dataset.level+'%');el.style.setProperty('--band',el.dataset.band)});r.querySelectorAll('[data-led]').forEach(el=>el.style.setProperty('--led',el.dataset.led))}
function errorView(e){content.innerHTML=panel('Unable to load this section',`<div class="empty-state"><p>${esc(e.message)}</p>${action('Try again','retry','primary-btn')}</div>`);$('#retry').onclick=()=>render()}
function storageDisplay(value){const text=String(value??''),match=text.match(/^(null|\d+) \/ (null|\d+) MB$/);if(!match)return text==='undefined'||text==='null'||text==='NaN'?'Unavailable':text;const used=match[1]==='null'?null:Number(match[1]),total=match[2]==='null'?null:Number(match[2]);if(total===null||total<=0)return'Unavailable';return used===null?`${total} MB capacity · usage unavailable`:`${used} / ${total} MB`}
/*
 * Binary units, because the upload cap is 33554432 bytes -- a "32 MB" that is
 * really 32 MiB. Rounding it the decimal way would print 33.6 MB and make a
 * file that fits look like one that does not.
 */
function mib(bytes){const n=Number(bytes);return Number.isFinite(n)&&n>=0?(n/1048576).toFixed(1)+' MiB':'—'}
/* Drive sizes span kilobytes to hundreds of gigabytes, so scale the unit
   rather than printing six-figure MiB. Binary units throughout, matching mib(). */
function bytes(v){const n=Number(v);if(!Number.isFinite(n)||n<0)return '—';
 const u=['B','KiB','MiB','GiB','TiB'];let i=0,x=n;
 while(x>=1024&&i<u.length-1){x/=1024;i++}
 return (i===0?x:x.toFixed(x<10?1:0))+' '+u[i]}
function storageValue(s){const total=Number(s?.storage_total_mb);if(!Number.isFinite(total)||total<=0)return'Unavailable';if(s.storage_available&&Number.isFinite(Number(s.storage_used_mb)))return`${Number(s.storage_used_mb)} / ${total} MB`;return`${total} MB capacity · usage unavailable`}
function metric(icon,label,value,percent,connected=false,powerLed=false){if(label==='Storage')value=storageDisplay(value);const numericPercent=Number(percent),hasPercent=Number.isFinite(numericPercent);return `<div class="metric ${powerLed?'binary-metric':''}"><svg><use href="#${icon}"></use></svg><span>${label}</span><span class="value ${connected?'connected':''}">${esc(value)}</span>${powerLed?`<span class="power-led ${connected?'on':'off'}" role="img" aria-label="${connected?'Available':'Unavailable'}"></span>`:hasPercent?`<progress class="bar" max="100" value="${Math.max(0,Math.min(100,numericPercent))}" aria-label="${esc(label)}: ${esc(value)}"></progress>`:''}</div>`}
function cpuDashboard(s){const cores=Array.isArray(s.cpus)?s.cpus:(Array.isArray(s.cpus?.cores)?s.cpus.cores:[]);return `<section class="panel cpu-panel" id="cpu-dashboard"><div class="cpu-heading"><h3>CPU cores</h3><span>${cores.filter(c=>c.online).length}/${cores.length} online</span></div><div class="cpu-core-grid">${cores.map(c=>`<article class="cpu-core ${c.online?'online':'offline'}"><div><strong>CPU${esc(c.id)}</strong><small>${c.online?'Up':'Down'} · ${c.frequency_khz?Math.round(c.frequency_khz/1000)+' MHz':'—'}</small></div><b>${c.online?Number(c.utilization_percent??c.utilization??0)+'%':'—'}</b><progress class="bar" max="100" value="${c.online?Math.max(0,Math.min(100,Number(c.utilization_percent??c.utilization??0))):0}" aria-label="CPU${c.id} utilization"></progress></article>`).join('')}</div></section>`}
function playbackSource(source){return ({airplay2:'AirPlay 2',bluetooth:'Bluetooth',radio:'Internet radio',media:'Media',system:'System',announcement:'Announcement',alarm:'Alarm'})[source]||source||'LibreEcho'}
/*
 * What is playing, and what can be done about it.
 *
 * A track title is shown only when the source actually sent one. AirPlay and
 * Bluetooth senders push metadata; an internet radio station does so only when
 * it interleaves ICY StreamTitle blocks, and plenty do not. When the stream
 * said nothing the station name stands in and the card says which of the two
 * happened, rather than dressing "media audio is playing" up as a track.
 */
function nowPlayingCopy(p){
 const m=p.metadata||{},source=playbackSource(p.source);
 if(p.state==='playing'){
  if(m.title)return{source,title:m.title,detail:[m.artist,m.album].filter(Boolean).join(' · ')||(m.station?'on '+m.station:'Playing now')};
  if(m.station)return{source,title:m.station,detail:'The station is not sending a track title'};
  return{source,title:`${source} audio`,detail:'This source is not sending track information'};}
 if(p.state==='announcing')return{source:'Announcement',title:'Announcement in progress',detail:'Voice and notification audio has priority'};
 if(p.state==='alarm')return{source:'Alarm',title:'Alarm active',detail:'Alarm audio has priority'};
 if(p.state==='system')return{source:'System',title:'System audio',detail:'LibreEcho is playing a system sound'};
 return{source:'Idle',title:'Nothing playing',detail:'Your audio sources will appear here'};}
/*
 * The transport is whatever the device reported in `transport`, never a guess.
 * A live stream can be stopped and started again but not paused -- there is no
 * buffered position to resume from -- and AirPlay and Bluetooth are driven by
 * the phone that started them, so the button is disabled and carries the
 * device's own reason instead of failing when pressed.
 */
function nowPlayingTransport(p,l){
 const t=p.transport||{},reason=t.reason||'LibreEcho cannot start or stop this source.';
 const act=t.stop?{action:'stop',label:'Stop'}:t.play?{action:'play',label:'Play'}:null;
 /* The light-ring toggle lives here because this is where the music is. It is
    the same setting as "Music visualizer" on the LED & Buttons page and the
    same endpoint, so the two always agree. */
 const lights=l&&!l.unsupported&&l.visualizer_enabled!==undefined?
  toggle('Lights with music',l.visualizer_enabled!==false,'now-playing-visualizer'):'';
 return `<div class="now-playing-transport"><button class="secondary-btn" id="playback-transport" type="button" data-action="${act?act.action:'play'}" title="${esc(reason)}"${act?'':' disabled'}>${act?act.label:'Play'}</button>${lights}${act?'':`<small>${esc(reason)}</small>`}</div>`;}
/*
 * Ambient light, from the board's TSL2540.
 *
 * The backend reports -1 when there is no usable sensor, which is not the same
 * as a dark room: 0 lux is a real reading. A board without a sensor shows no
 * row at all rather than a permanent "0 lux".
 *
 * The bar is logarithmic because lux is. A lit room is around 100 and daylight
 * is tens of thousands, so a linear bar would sit pinned at zero indoors and
 * tell you nothing.
 */
/*
 * The ambient light sensor in full. ch0 is the visible channel and ch1 the
 * infrared one; the driver derives lux from the pair, so showing both makes it
 * obvious when a reading is dominated by IR -- incandescent light or sunlight
 * through glass -- rather than by anything the eye would call brightness.
 *
 * Gain and integration time are what the reading was taken with. They matter
 * because this driver ships in manual gain, which saturates outdoors: a lux
 * figure that stops climbing is a clipped one, and the gain row is where that
 * shows.
 */
function lightPanel(x){
 if(!x)return '';
 if(!x.available)return panel('Ambient light sensor',
   '<p class="muted">No ambient light sensor is present on this board.</p>','light-panel');
 const rows=[
   ['Illuminance', x.lux>=0?`${x.lux} lux`:'—'],
   ['Calibrated', x.calibrated_lux>=0?`${x.calibrated_lux} lux`:'—'],
   ['Visible channel', x.visible],
   ['Infrared channel', x.infrared],
   ['Gain', `${x.gain}\u00d7 ${x.auto_gain?'(auto)':'(manual)'}`],
   ['Integration time', x.integration_us?`${Math.round(x.integration_us/1000)} ms`:'—'],
   ['Powered', x.powered?'yes':'no'],
   ['Device', `${x.driver} on ${x.bus}`]
 ];
 return panel('Ambient light sensor',
  `<dl class="facts">${rows.map(([k,v])=>`<dt>${esc(k)}</dt><dd>${esc(String(v))}</dd>`).join('')}</dl>`+
  (x.auto_gain?'':'<p class="muted">Gain is fixed rather than automatic, so the reading saturates in bright daylight.</p>'),
  'light-panel');
}
function ambientLightParts(lux){
 const pct=Math.max(0,Math.min(100,Math.round(Math.log10(Math.max(lux,1)+1)/Math.log10(10001)*100)));
 const label=lux<10?'Dark':lux<50?'Dim':lux<300?'Indoors':lux<1000?'Bright':'Daylight';
 return {value:`${lux} lux · ${label}`,percent:pct};
}
function lightMetric(s){
 const lux=s.light_lux;
 if(typeof lux!=='number'||lux<0)return '';
 const light=ambientLightParts(lux);
 return metric('sun','Ambient light',light.value,light.percent);
}
function nowPlaying(p={},l={}){const active=p.state&&p.state!=='idle',copy=nowPlayingCopy(p),levels=Array.isArray(l.visualizer_levels)&&l.visualizer_levels.length===12?l.visualizer_levels:Array(12).fill(0);return `<section class="panel now-playing ${active?'active':'idle'}" id="now-playing"><div class="now-playing-mark" aria-hidden="true"><svg viewBox="0 0 100 100"><circle cx="50" cy="50" r="36"></circle><circle cx="50" cy="50" r="8"></circle></svg></div><div class="now-playing-copy"><span class="source-pill">${esc(copy.source)}</span><h3>${esc(copy.title)}</h3><p>${esc(copy.detail)}</p></div>${nowPlayingTransport(p,l)}<div class="spectrum-mini" aria-label="${l.visualizer_active?'Live 12-band music spectrum':'Audio spectrum inactive'}">${levels.map((v,i)=>`<i data-level="${Math.max(4,Math.min(100,Math.round(Number(v)||0)))}" data-band="${i}"></i>`).join('')}</div><div class="now-playing-state"><span class="status-dot ${active?'ok':''}"></span>${esc(p.state||'idle')}</div></section>`}
/*
 * The card is replaced wholesale on every poll, so its handlers are bound
 * after every render rather than once. A refresh landing while a request is in
 * flight would also throw away what the user just did, which is why the poll
 * leaves the card alone while state.busy is set.
 */
function renderNowPlaying(){const card=$('#now-playing');if(!card)return;card.outerHTML=nowPlaying(state.data.playback||{},state.data.led||{});applyCssVars(content);bindNowPlaying()}
function bindNowPlaying(){
 const b=$('#playback-transport');
 if(b&&!b.disabled)b.onclick=()=>playbackTransport(b.dataset.action);
 const v=$('#now-playing-visualizer');
 if(v)v.onchange=()=>setMusicLights(v.checked);}
async function playbackTransport(action){
 if(state.busy)return;
 setBusy(true);
 try{state.data.playback=await api('/playback/transport',{method:'POST',body:JSON.stringify({action})});
  toast(action==='stop'?'Playback stopped':'Playback started')}
 catch(e){toast(e.message,true)}
 finally{setBusy(false);renderNowPlaying()}}
async function setMusicLights(enabled){
 if(state.busy)return;
 setBusy(true);
 try{state.data.led=await api('/led',{method:'PUT',body:JSON.stringify({visualizer_enabled:enabled})});
  toast(enabled?'The light ring follows music':'The light ring stays still during music')}
 catch(e){toast(e.message,true)}
 finally{setBusy(false);renderNowPlaying()}}

function uptime(s){s=Math.max(0,Number(s)||0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return `${d?d+'d ':''}${h}h ${m}m`}
function logTime(t,boot){const seconds=Number(t),relative=Number(boot);if(relative>0)return `Boot +${relative}s`;return seconds>=1577836800?new Date(seconds*1000).toLocaleTimeString():'Clock unset'}
function rgb(c){return `rgb(${c.r}, ${c.g}, ${c.b})`}
function networkLabel(n){return n.ssid?`${n.state} · ${n.ssid}`:n.state}
/*
 * Optional features are off by default and the menu follows them: Simulation
 * plays audio into the microphone path, which is a testing capability rather
 * than something a live device should offer, so its entry is absent until the
 * feature is switched on. The endpoint is gated server-side as well, so this
 * is presentation -- but a menu entry that leads to a 403 is still wrong.
 *
 * The nav used to be built once from the constant `items`; it is rebuilt from
 * a filtered view of that array instead, so the entry can come and go without
 * a reload.
 */
function navItems(){return items.filter(([name])=>name!=='Simulation'||state.features.simulation)}
function renderNav(){nav.innerHTML='';navItems().forEach(([name,icon])=>{const b=document.createElement('button');b.className='nav-item'+(name===state.page?' active':'');b.dataset.page=name;b.innerHTML=`<svg><use href="#${icon}"></use></svg><span>${name}</span>`;b.onclick=()=>showPage(name);nav.appendChild(b)})}
function applyFeatures(f){state.features={simulation:!!(f&&f.simulation),https:!!(f&&f.https),https_active:!!(f&&f.https_active),https_port:f&&f.https_port,https_expires:f&&f.https_expires,https_fingerprint:f&&f.https_fingerprint};renderNav();if(!navItems().some(([n])=>n===state.page))showPage('Overview');return state.features}
function pageSlug(name){return name.toLowerCase().replace(/ & /g,'-').replace(/ /g,'-')}
function pageFromLocation(){const slug=decodeURIComponent(location.pathname.slice(1));return items.find(([name])=>pageSlug(name)===slug)?.[0]||'Overview'}
function updateVersionDisplay(d,ota=state.data.ota){const full=String(d.os_version||'LibreEcho OS'),version=full.replace(/^LibreEcho OS\s*/,'')||full,badge=$('#backend-badge'),available=ota?.check_status==='update-available'&&!ota.pending_reboot;$('#sidebar-version').textContent=full;badge.dataset.version=version;badge.title=full;badge.setAttribute('aria-label',full);$('#update-available').hidden=!available;state.data.ota=ota;state.data.otaCheckedAt=Date.now()}
function bindRange(){ $$('.range-field input').forEach(el=>el.oninput=()=>el.parentElement.querySelector('output').textContent=el.value+'%') }
function bindDirty(ids,button){const b=$(button);ids.map(id=>$(id)).filter(Boolean).forEach(el=>el.addEventListener('input',()=>b.disabled=false));}
/*
 * Disabling every control while a request is in flight is right, but
 * re-enabling every control afterwards was not: it cleared disabled state
 * the page had set deliberately.  Every Save button came back enabled with
 * nothing dirty, and controls marked unsupported (the notification volume
 * slider, the echo-cancellation toggle) became editable after any action.
 *
 * Mark only what this function disabled, and re-enable only that.  After a
 * re-render the fresh nodes carry no mark, so their state is left alone --
 * which is what the callers that re-render want.  The update-upload error
 * path does not re-render, and its original nodes are still marked, so it
 * still recovers.
 */
function setBusy(b){state.busy=b;if(b){$$('button,input,select',content).forEach(x=>{if(!x.disabled){x.disabled=true;x.dataset.busyDisabled='1'}})}else{$$('[data-busy-disabled]',content).forEach(x=>{x.disabled=false;delete x.dataset.busyDisabled})}}
async function mutate(path,data,message,headers={}){if(state.busy)return;setBusy(true);try{await api(path,{method:'PUT',body:JSON.stringify(data),headers});toast(message);await render()}catch(e){toast(e.message,true);await render()}finally{setBusy(false)}}
async function del(path,message='Action accepted',headers={}){if(state.busy)return;setBusy(true);try{await api(path,{method:'DELETE',headers:{'X-LibreEcho-CSRF':state.csrf,...headers}});toast(message);await render()}catch(e){toast(e.message,true);await render()}finally{setBusy(false)}}
async function post(path,data={},message='Action accepted',headers={}){if(state.busy)return;setBusy(true);try{const r=await api(path,{method:'POST',body:JSON.stringify(data),headers});toast(message);await render();return r}catch(e){toast(e.message,true);await render()}finally{setBusy(false)}}
/*
 * Reboot used to fire and forget: the toast said "Reboot requested", the
 * follow-up render hit a device that was already going down, and the user
 * was left with an error and no idea whether it worked or how long to wait.
 *
 * Wait for the device instead. The estimate is how long the last boot took
 * to reach this daemon, reported by /system, so it reflects this hardware
 * rather than a guess.
 */
async function waitForDevice(estimate,title){
 const dlg=document.createElement('dialog');
 dlg.className='auth-form reboot-dialog';
 dlg.innerHTML=`<h2>${esc(title)}</h2><p id="reboot-status">Sending the request…</p>`+
  `<progress id="reboot-progress" max="1000" value="0"></progress>`+
  `<p class="muted" id="reboot-hint">This page reconnects on its own once the device is back.</p>`;
 document.body.appendChild(dlg);
 dlg.showModal();
 const status=dlg.querySelector('#reboot-status'),bar=dlg.querySelector('#reboot-progress'),
       hint=dlg.querySelector('#reboot-hint'),started=Date.now();
 let wentDown=false,finished=false;
 /* Bound every probe. When the device drops off the network the request is
    not refused, it simply never answers, so an unbounded fetch parks the
    loop on its first poll: the modal appears and then sits on its opening
    message with the bar at zero for the whole reboot. Refused connections
    fail fast and hid this -- only a real device does the silent kind. */
 const alive=async()=>{
  const abort=new AbortController(),timer=setTimeout(()=>abort.abort(),2000);
  try{const r=await fetch('/healthz',{cache:'no-store',signal:abort.signal});return r.ok}
  catch(_){return false}
  finally{clearTimeout(timer)}};
 /* Drive the display off its own clock rather than off the poll, so the
    countdown stays smooth however long a probe takes. */
 const paint=()=>{
  if(finished)return;
  const elapsed=(Date.now()-started)/1000;
  bar.value=Math.min(990,Math.round(elapsed/estimate*1000));
  if(!wentDown){status.textContent='Waiting for the device to go down…';return}
  const left=Math.max(0,Math.ceil(estimate-elapsed));
  if(elapsed<estimate)status.textContent=`Restarting — about ${left} second${left===1?'':'s'} left`;
  else{status.textContent='Still restarting…';
   hint.textContent='This is taking longer than the last boot did. It will reconnect as soon as the device answers.'}};
 const ticker=setInterval(paint,500);
 paint();
 try{
  for(;;){
   const up=await alive();
   if(!up)wentDown=true;
   else if(wentDown){
    finished=true;status.textContent='Back online. Reloading…';bar.value=1000;
    await new Promise(r=>setTimeout(r,600));location.reload();return}
   paint();
   await new Promise(r=>setTimeout(r,1000));
  }
 }finally{clearInterval(ticker)}}
async function power(path,name){
 if(!confirm(`${name} this LibreEcho device?`))return;
 if(state.busy)return;
 let estimate=45;
 try{estimate=(await api('/system')).boot_estimate_seconds||45}catch(_){/* keep the default */}
 if(path==='shutdown'){
  try{await api(`/system/${path}`,{method:'POST',body:'{}',headers:{'X-LibreEcho-Confirm':'confirm-device-action'}});
      toast('Shutting down')}catch(e){toast(e.message,true)}
  return}
 /* Fire the reboot but do not wait on it. The device frequently goes down
    before it can answer, so awaiting the response hangs forever and the
    progress modal below is never reached -- the reboot happens and the user
    sees nothing, which is the behaviour this feature exists to fix. A
    dropped connection here is the expected case; only a real HTTP error
    means the request was refused. */
 let refused=null;
 const fired=api(`/system/${path}`,{method:'POST',body:'{}',headers:{'X-LibreEcho-Confirm':'confirm-device-action'}})
   .catch(e=>{if(/^Request failed \((4|5)\d\d\)$/.test(e.message)||/refused|not permitted|confirm/i.test(e.message))refused=e});
 await Promise.race([fired,new Promise(r=>setTimeout(r,1500))]);
 if(refused){toast(refused.message,true);return}
 await waitForDevice(estimate,'Restarting your LibreEcho');}
async function overview(){const [s,n,a,l,d,p,ota]=await Promise.all([api('/status'),api('/network'),api('/audio').catch(e=>({unsupported:e.message})),api('/led').catch(e=>({unsupported:e.message})),api('/device'),api('/playback').catch(()=>({state:'idle',source:null,metadata:{available:false}})),api('/system/update').catch(()=>({supported:false,check_status:'not-checked'}))]);state.data.status=s;state.data.playback=p;state.data.led=l;$('#backend-badge').textContent=s.backend+(s.simulated?' · simulated':'');$('#backend-badge').className='backend-badge '+s.backend;$('#device-online').innerHTML=`<span></span>${esc(s.device_state)}`;$('#sidebar-uptime').textContent='Uptime: '+uptime(s.uptime_seconds);updateVersionDisplay(d,ota);content.innerHTML=`<div class="grid-top"><div class="panel hero"><div class="sim-label" id="hero-device-label">${esc(d.hostname||d.name||'LibreEcho')}</div><h2>LibreEcho</h2><p>Open source voice assistant<br>built for privacy and freedom.</p><img class="device-img" src="/assets/device.png" alt="Amazon Echo device"><div class="hero-actions">${action('Device details','device-details','primary-btn')}${linkAction('API','/api/v1')}${linkAction('Swagger','/swagger.html')}</div></div><div class="panel status-panel"><h3>System Status</h3>${metric('device','CPU Load',s.cpu_percent+'%',s.cpu_percent)}${metric('device','Memory',`${s.memory_used_mb} / ${s.memory_total_mb} MB`,s.memory_percent)}${metric('device','Storage',storageValue(s),s.storage_available?s.storage_percent:null)}${metric('sun','Temperature',s.temperature_c+' °C',s.temperature_c)}${lightMetric(s)}${metric('wifi','Wi-Fi',networkLabel(n),n.signal,n.state==='connected',true)}${metric('info','Internet',n.internet?'Reachable':'Unavailable',0,n.internet,true)+(a.unsupported?'':metric('mic','Microphone',a.microphone_muted?'Muted':'Live',0,!a.microphone_muted,true))}</div></div>${nowPlaying(p,l)}${cpuDashboard(s)}<div class="cards">${items.slice(2,10).map(([name,icon],i)=>`<button class="panel shortcut" data-page="${name}"><svg class="${['green','purple','blue','sky','green','orange','grey','orange'][i]}"><use href="#${icon}"></use></svg><span><strong>${name}</strong><small>${descriptions[name]}</small></span><span class="arrow">›</span></button>`).join('')}</div><div class="panel community"><img src="/assets/mark.svg" alt="" class="community-mark"><div><h3>Open Source. Community Driven.</h3><p>Configuration stays on your device. ${s.simulated?'This development session uses deterministic mock-capable hardware state.':'Values shown come from the Linux backend.'}</p></div></div>`;$$('[data-page]').forEach(b=>b.onclick=()=>showPage(b.dataset.page));$('#device-details').onclick=()=>showPage('Device');bindNowPlaying()}
function updateOverviewMetric(label,value,percent,connected){const row=$$('.status-panel .metric').find(x=>x.querySelector('span')?.textContent===label);if(!row)return;if(label==='Storage')value=storageDisplay(value);const output=row.querySelector('.value');output.textContent=value;if(connected!==undefined)output.classList.toggle('connected',connected);const bar=row.querySelector('progress');if(bar)bar.value=Math.max(0,Math.min(100,percent));const led=row.querySelector('.power-led');if(led){led.classList.toggle('on',!!connected);led.classList.toggle('off',!connected);led.setAttribute('aria-label',connected?'Available':'Unavailable')}}
function updateOverviewAmbientLight(s){
 const panel=$('.status-panel');
 if(!panel)return;
 const row=$$('.metric',panel).find(x=>x.querySelector('span')?.textContent==='Ambient light');
 const lux=s.light_lux;
 if(typeof lux!=='number'||lux<0){if(row)row.remove();return;}
 const light=ambientLightParts(lux);
 if(row){
  const output=row.querySelector('.value');
  if(output)output.textContent=light.value;
  const bar=row.querySelector('progress');
  if(bar)bar.value=light.percent;
  return;
 }
 const temperature=$$('.metric',panel).find(x=>x.querySelector('span')?.textContent==='Temperature');
 if(temperature)temperature.insertAdjacentHTML('afterend',lightMetric(s));
}
async function refreshOverview(){if(state.page!=='Overview')return;let delay=5000;try{const [s,n,d,p,l]=await Promise.all([api('/status'),api('/network'),api('/device'),api('/playback'),api('/led').catch(()=>({}))]);if(state.page!=='Overview')return;state.data.status=s;$('#backend-badge').textContent=s.backend+(s.simulated?' · simulated':'');$('#backend-badge').className='backend-badge '+s.backend;$('#device-online').innerHTML=`<span></span>${esc(s.device_state)}`;$('#sidebar-uptime').textContent='Uptime: '+uptime(s.uptime_seconds);$('#sidebar-version').textContent=d.os_version;if(Date.now()-(state.data.otaCheckedAt||0)>=60000)updateVersionDisplay(d,await api('/system/update').catch(()=>state.data.ota));const deviceLabel=$('#hero-device-label');if(deviceLabel)deviceLabel.textContent=d.hostname||d.name||'LibreEcho';updateOverviewMetric('CPU Load',s.cpu_percent+'%',s.cpu_percent);updateOverviewMetric('Memory',`${s.memory_used_mb} / ${s.memory_total_mb} MB`,s.memory_percent);updateOverviewMetric('Storage',storageValue(s),s.storage_available?s.storage_percent:null);updateOverviewMetric('Temperature',s.temperature_c+' °C',s.temperature_c);updateOverviewAmbientLight(s);updateOverviewMetric('Wi-Fi',networkLabel(n),0,n.state==='connected');updateOverviewMetric('Internet',n.internet?'Reachable':'Unavailable',0,n.internet);state.data.playback=p;state.data.led=l;
 /* A poll landing mid-interaction would throw away the click that is still in
    flight, so the card keeps whatever the user just did until it settles. */
 if(!state.busy)renderNowPlaying();const cpu=$('#cpu-dashboard');if(cpu)cpu.outerHTML=cpuDashboard(s);applyCssVars(content);if(p.state!=='idle'||l.visualizer_active)delay=1000}catch(_){/* Preserve the last good telemetry when a background refresh fails. */}finally{if(state.page==='Overview')state.timer=setTimeout(refreshOverview,delay)}}
async function usersPage(){const u=await api('/auth/users');const rows=u.users.map(x=>`<div class="status-line"><span class="status-dot ok"></span><span>${esc(x.username)}${x.username===state.username?' <small>(current session)</small>':''}</span>${u.users.length>1&&x.username!==state.username?action('Remove','remove-user-'+esc(x.username),'danger-btn'):''}</div>`).join('');content.innerHTML=`<div class="settings-grid">${panel('Local accounts',`<p class="muted">These accounts can sign in to the LibreEcho control centre. Passwords are stored as salted hashes and are never displayed.</p><div class="user-list">${rows||'<p class="muted">No users configured.</p>'}</div>`)}${panel('Add user',field('Username','','new-user-name','text','autocomplete="username" maxlength="31" pattern="[A-Za-z0-9._-]+"')+field('Password','','new-user-password','password','autocomplete="new-password" minlength="8" maxlength="128"')+field('Confirm password','','new-user-confirm','password','autocomplete="new-password" minlength="8" maxlength="128"')+`<div class="button-row">${action('Add user','add-user','primary-btn')}</div>`)} </div>`;u.users.filter(x=>x.username!==state.username).forEach(x=>{const b=$('#remove-user-'+x.username);if(b)b.onclick=async()=>{if(!confirm(`Remove user ${x.username}?`))return;try{await api('/auth/users/'+encodeURIComponent(x.username),{method:'DELETE'});toast('User removed');await usersPage()}catch(e){toast(e.message,true)}}});$('#add-user').onclick=async()=>{const username=$('#new-user-name').value.trim(),password=$('#new-user-password').value,confirmPassword=$('#new-user-confirm').value;if(password!==confirmPassword){toast('Passwords do not match',true);return}try{await api('/auth/users',{method:'POST',body:JSON.stringify({username,password,password_confirm:confirmPassword})});toast('User added');await usersPage()}catch(e){toast(e.message,true)}}}
/*
 * Hardware and audio capability, shown on the Device page as reference
 * information rather than settings -- none of it is adjustable from here.
 *
 * Every field is optional: an older daemon returns no `audio` object at all
 * and the mock backend does not fill one in either, so each row is dropped
 * when its value is missing rather than rendered as "undefined".
 */
function factList(pairs){
 const rows=pairs.filter(([,v])=>v!==undefined&&v!==null&&v!==''&&!(Array.isArray(v)&&!v.length))
  .map(([k,v])=>`<dt>${esc(k)}</dt><dd>${esc(Array.isArray(v)?v.join(', '):v)}</dd>`).join('');
 return rows?`<dl class="facts">${rows}</dl>`:'';}
function rateText(v){const n=Number(v);return Number.isFinite(n)&&n>0?(n%1000?n+' Hz':(n/1000)+' kHz'):null}
function captureChannelText(c){
 const raw=Number(c.raw_channels),mics=Number(c.microphones),transport=Number(c.transport_channels);
 if(!Number.isFinite(raw))return Number.isFinite(mics)?mics+' microphones':null;
 const parts=[];
 if(Number.isFinite(mics))parts.push(mics+' microphone'+(mics===1?'':'s'));
 if(Number.isFinite(transport))parts.push(transport+' transport');
 return raw+' raw channels'+(parts.length?' — '+parts.join(' + '):'');}
function formatText(c){
 const f=c.format,bits=Number(c.valid_bits);
 if(!f)return null;
 return Number.isFinite(bits)?`${f} (${bits} valid bits)`:f;}
const AUDIO_TRANSPORTS={'airplay2':'AirPlay 2','bluetooth-a2dp':'Bluetooth A2DP','bluetooth':'Bluetooth'};
function transportName(id){return AUDIO_TRANSPORTS[id]||id}
/*
 * "Can it play a stream URL?" is a question people actually ask, so answer it
 * in a sentence instead of printing an empty `decoders` array at them.
 */
function streamingText(st){
 const decoders=Array.isArray(st.decoders)?st.decoders:null;
 const available=(Array.isArray(st.available)?st.available:[]).map(transportName);
 if(!decoders&&!available.length)return '';
 const inputs=available.length?`Audio reaches it already decoded, over ${available.join(' and ')}.`:'';
 if(decoders&&!decoders.length)
  return `<p class="muted">This image carries no compressed-audio decoder, so the device cannot play a stream URL by itself. ${inputs}</p>`;
 if(decoders&&decoders.length)
  return `<p class="muted">Decodes ${decoders.join(', ')} on the device. ${inputs}</p>`;
 return `<p class="muted">${inputs}</p>`;}
function hardwareCard(d){
 const audio=d.audio||{},capture=audio.capture||{},output=audio.output||{},streaming=audio.streaming||{};
 const hardware=factList([['Model',d.model],['System on chip',d.hardware_revision],['Kernel',d.kernel],['Serial',d.serial]]);
 const captureFacts=factList([
  ['Sample rate',rateText(capture.rate_hz)],['Channels',captureChannelText(capture)],
  ['Format',formatText(capture)],['Beamforming',capture.beamforming],
  ['High-pass filter',Number.isFinite(Number(capture.high_pass_hz))?capture.high_pass_hz+' Hz':null],
  ['Digital gain',capture.digital_gain],['Frequency response',capture.response],
  ['Noise floor',Number.isFinite(Number(capture.noise_floor_dbfs))?capture.noise_floor_dbfs+' dBFS':null],
  ['THD+N',Number.isFinite(Number(capture.thd_n_percent_max))?'up to '+capture.thd_n_percent_max+' %':null],
  ['Clips above',Number.isFinite(Number(capture.clipping_from_input_amplitude))?'input amplitude '+capture.clipping_from_input_amplitude:null]]);
 const outputFacts=factList([
  ['Sample rate',rateText(output.rate_hz)],['Channels',output.channels],['Format',output.format],
  ['Mixer volume',output.mixer_volume_range],['Mixing buses',output.buses]]);
 const streamingFacts=factList([
  ['Decoders',Array.isArray(streaming.decoders)?(streaming.decoders.length?streaming.decoders:'None on this image'):undefined],
  ['Inputs',(Array.isArray(streaming.available)?streaming.available:[]).map(transportName)]]);
 const groups=[
  captureFacts?`<div><h4>Capture</h4>${captureFacts}</div>`:'',
  outputFacts?`<div><h4>Output</h4>${outputFacts}</div>`:'',
  (streamingFacts||streamingText(streaming))?`<div><h4>Streaming</h4>${streamingText(streaming)}${streamingFacts}</div>`:''
 ].filter(Boolean).join('');
 return panel('Hardware and audio capability',
  `<p class="muted">Reported by the device. Reference information, not settings.</p>`+
  (hardware?`<h4>Hardware</h4>${hardware}`:'')+
  (groups?`<h4>Audio capability</h4><div class="hardware-groups">${groups}</div>`
         :`<p class="muted">This daemon does not report audio capability.</p>`),
  'wide hardware-card');}
async function devicePage(){const d=await api('/device');const light=await api('/light').catch(()=>null);content.innerHTML=`<div class="settings-grid">${panel('Device identity',field('Device name',d.name,'device-name','text','disabled')+field('Hostname',d.hostname,'hostname')+field('Model',d.model,'model','text','disabled')+field('Serial / development ID',d.serial,'serial','text','disabled')+saveButton('save-device'))}${panel('Platform',`<dl class="facts"><dt>OS version</dt><dd>${esc(d.os_version)}</dd><dt>Kernel</dt><dd>${esc(d.kernel)}</dd><dt>Hardware revision</dt><dd>${esc(d.hardware_revision)}</dd><dt>Backend</dt><dd>${esc(d.backend)}</dd></dl>`)}${hardwareCard(d)}${panel('Power controls',`<p class="muted">These actions require a confirmation token and are rate limited.</p><div class="button-row">${action('Reboot','power-reboot','outline-btn')}${action('Shut down','power-shutdown','danger-btn')}${action('Factory reset','power-reset','danger-btn')}</div>`,'wide')}${lightPanel(light)}</div>`;bindDirty(['#hostname'],'#save-device');$('#save-device').onclick=()=>mutate('/network',{hostname:$('#hostname').value},'Device changes saved');$('#power-reboot').onclick=()=>power('reboot','Reboot');$('#power-shutdown').onclick=()=>power('shutdown','Shut down');$('#power-reset').onclick=()=>power('factory-reset','Factory reset')}
const NOISE_COLOURS=[['white','White'],['pink','Pink'],['brown','Brown']];
const NOISE_TIMERS=[[0,'No timer'],[15,'15 minutes'],[30,'30 minutes'],[45,'45 minutes'],[60,'1 hour'],[90,'1.5 hours'],[120,'2 hours'],[480,'8 hours']];
function noiseRemainingText(n){
 if(!n.active)return 'Not playing';
 const name=(NOISE_COLOURS.find(c=>c[0]===n.colour)||['','White'])[1];
 if(n.remaining_seconds<0)return `${name} noise at ${n.level}% — until stopped`;
 const m=Math.ceil(n.remaining_seconds/60);
 return `${name} noise at ${n.level}% — ${m} minute${m===1?'':'s'} left`;}
function noisePanel(n){
 const colour=n.active?n.colour:'brown',level=n.active?n.level:40;
 return panel('Sleep sounds',
  `<label class="field"><span>Sound</span><select id="noise-colour">${NOISE_COLOURS.map(([v,l])=>`<option value="${v}" ${v===colour?'selected':''}>${l}</option>`).join('')}</select></label>`+
  range('Level',level,'noise-level')+
  `<label class="field"><span>Sleep timer</span><select id="noise-minutes">${NOISE_TIMERS.map(([v,l])=>`<option value="${v}" ${v===30?'selected':''}>${l}</option>`).join('')}</select></label>`+
  `<dl class="facts"><dt>Status</dt><dd class="${n.active?'connected':''}">${esc(noiseRemainingText(n))}</dd></dl>`+
  `<p class="muted">Generated on the device, so it keeps playing with the internet down. The sleep timer stops it on its own.</p>`+
  `<div class="button-row">${action(n.active?'Restart':'Start','noise-start','primary-btn')}${action('Stop','noise-stop')}</div>`);}
function bindNoise(n){
 const stop=$('#noise-stop');if(stop){stop.disabled=!n.active;stop.onclick=()=>del('/audio/noise','Sleep sounds stopped')}
 const start=$('#noise-start');if(start)start.onclick=()=>post('/audio/noise',{colour:$('#noise-colour').value,level:Math.max(1,+$('#noise-level').value),minutes:+$('#noise-minutes').value},'Sleep sounds playing')}
async function audioPage(){const a=await api('/audio'),voices=a.tts_voices||[{id:'southern-female',name:'Southern English — female'},{id:'northern-male',name:'Northern English — male'}],voiceOptions=voices.map(v=>`<option value="${esc(v.id)}" ${v.id===a.tts_voice?'selected':''}>${esc(v.name)}</option>`).join('');content.innerHTML=`<div class="settings-grid">${panel('Output',range('Master volume',a.volume,'volume')+range('Notification volume',a.notification_volume,'notification-volume').replace('value="'+a.notification_volume+'"','value="'+a.notification_volume+'" disabled')+toggle('Startup sound',a.startup_sound,'startup-sound',true)+`<dl class="facts"><dt>Output</dt><dd class="${a.output_available?'connected':''}">${a.output_available?'Available':'Unavailable'}</dd><dt>Amplifier</dt><dd>${a.amplifier_on?'On':'Off'}</dd></dl><div class="button-row">${saveButton('save-output')}${action('Play test tone','test-tone')}</div>`)}${panel('Announcements',`<label class="field"><span>Voice</span><select id="tts-voice">${voiceOptions}</select></label><p class="muted">The selected British voice stays loaded for low-latency streamed announcements. Changing voice restarts the speech service.</p>${saveButton('save-voice')}`)}${noisePanel(a.noise||{})}${panel('Microphones',range('Microphone gain',a.microphone_gain,'mic-gain')+toggle('Microphone muted',a.microphone_muted,'mic-muted')+toggle('Acoustic echo cancellation',true,'aec',true)+`<p class="muted">Echo cancellation is reported by the future audio adapter and cannot yet be changed.</p>`+saveButton('save-microphones'))}</div>`;bindRange();bindDirty(['#volume'],'#save-output');bindDirty(['#tts-voice'],'#save-voice');bindDirty(['#mic-gain','#mic-muted'],'#save-microphones');$('#save-output').onclick=()=>mutate('/audio',{volume:+$('#volume').value},'Output changes saved');$('#save-voice').onclick=()=>mutate('/audio',{tts_voice:$('#tts-voice').value},'Announcement voice changed');$('#save-microphones').onclick=()=>mutate('/audio',{microphone_gain:+$('#mic-gain').value,microphone_muted:$('#mic-muted').checked},'Microphone changes saved');$('#test-tone').onclick=()=>post('/audio/test',{},'Test tone playing');bindNoise(a.noise||{})}
const babyStream={controller:null,context:null,gain:null,nextTime:0,generation:0};
function stopBabyStream(){const controller=babyStream.controller,context=babyStream.context;babyStream.generation++;babyStream.controller=null;babyStream.context=null;babyStream.gain=null;babyStream.nextTime=0;if(controller)controller.abort();if(context&&context.state!=='closed')context.close().catch(()=>{});const status=$('#baby-status');if(status)status.textContent='Stopped'}
function babyAudioContract(response,source,channel){const header=response.headers.get('X-LibreEcho-Audio')||'',fields=header.split(';'),format=fields[0]||'',value=key=>{const field=fields.find(x=>x.startsWith(key+'='));return field?field.slice(key.length+1):''},bits=format==='pcm_s24_3le'?24:format==='pcm_s16_le'?16:Number(source.bits)||16,validBits=Math.max(2,Math.min(bits,Number(value('valid-bits'))||Number(source.valid_bits)||bits)),channels=Math.max(1,Number(value('channels'))||Number(source.channels)||1),rate=Number(value('rate'))||Number(source.rate)||16000,selected=Number(value('selected-channel')),selectedChannel=Number.isFinite(selected)?selected:channels===1?0:channel;return{bits,validBits,channels,rate,channel:Math.max(0,Math.min(channels-1,selectedChannel))}}
async function babyMonitorPage(){
  const d=await api('/baby-monitor');
  if(!d.sources.length){
    content.innerHTML='<div class="settings-grid">'+panel('Baby monitor',unsupported('No Echo microphone array is currently available.'))+
      panel('Privacy','<p class="muted">Audio is streamed directly from this browser and is not recorded by LibreEcho.</p>')+'</div>';
    return;
  }
  const options=d.sources.map(x=>'<option value="'+esc(x.id)+'">'+esc(x.name)+' · card '+x.card+', device '+x.device+'</option>').join('');
  const first=d.sources[0];
  const microphoneOptions=source=> (source.microphones||[{channel:0,name:'Microphone 1'}])
    .map(x=>'<option value="'+x.channel+'">'+esc(x.name)+'</option>').join('');
  content.innerHTML='<div class="settings-grid">'+
    panel('Microphone source','<label class="field"><span>Capture endpoint</span><select id="baby-source">'+options+
      '</select></label><label class="field"><span>Microphone</span><select id="baby-channel">'+microphoneOptions(first)+
      '</select></label><p class="muted">The Echo array transports nine packed 24-bit lanes at 16 kHz. Hardware testing confirms that lanes 1–7 carry the seven microphones; lanes 8–9 are inactive or reserved.</p>'+
      '<div class="notice unsupported"><strong>Stock calibration</strong><span>'+(d.calibration&&d.calibration.complete?'Seven logical mic gains were found in '+esc(d.calibration.source)+'. Raw monitoring remains uncalibrated until the microphones’ logical geometry order is verified.':'No complete seven-entry miccal set is available; using the stock fallback value 16384 for metadata only.')+'</span></div>'+
      (d.simulated?'<div class="notice unsupported"><strong>Preview only</strong><span>The mock backend exposes a source list but cannot produce live microphone audio.</span></div>':'')+
      '</div>')+
    panel('Playback',range('Browser playback volume',35,'baby-volume')+
      '<div class="status-line"><span class="status-dot" id="baby-dot"></span><span id="baby-status" aria-live="polite">Stopped</span></div>'+
      '<div class="button-row">'+action('Start listening','baby-start','primary-btn')+action('Stop','baby-stop')+'</div>'+
      '<p class="muted">Listening starts only when you press the button. Closing this page stops capture.</p>')+
    '</div>';
  bindRange();
  const sourceSelect=$('#baby-source');
  const channelSelect=$('#baby-channel');
  const updateChannels=()=>{
    const source=d.sources.find(x=>x.id===sourceSelect.value)||first;
    channelSelect.innerHTML=microphoneOptions(source);
  };
  sourceSelect.onchange=updateChannels;
  $('#baby-volume').oninput=()=>{
    const value=Number($('#baby-volume').value)/100;
    $('#baby-volume').parentElement.querySelector('output').textContent=$('#baby-volume').value+'%';
    if(babyStream.gain)babyStream.gain.gain.value=value;
  };
  $('#baby-start').onclick=async()=>{
    stopBabyStream();
    if(d.simulated){toast('Live microphone streaming is unavailable in the mock backend',true);return}
    const source=d.sources.find(x=>x.id===sourceSelect.value)||first;
    const channel=Number(channelSelect.value)||0;
    const context=new(window.AudioContext||window.webkitAudioContext)();
    const controller=new AbortController();
    const generation=babyStream.generation;
    babyStream.context=context;
    babyStream.controller=controller;
    babyStream.nextTime=context.currentTime+0.05;
    const gain=context.createGain();
    gain.gain.value=Number($('#baby-volume').value)/100;
    gain.connect(context.destination);
    babyStream.gain=gain;
    $('#baby-status').textContent='Connecting…';
    $('#baby-dot').classList.add('ok');
    try{
      await context.resume();
      if(generation!==babyStream.generation)return;
      const response=await fetch('/api/v1/baby-monitor/stream?source='+encodeURIComponent(source.id)+'&channel='+channel,{
        headers:{Accept:'application/octet-stream',...(state.token?{Authorization:'Bearer '+state.token}:{})},
        signal:controller.signal
      });
      if(!response.ok)throw new Error('Microphone stream failed ('+response.status+')');
      if(generation!==babyStream.generation)return;
      if(!response.body)throw new Error('Microphone stream returned no audio body');
      const contract=babyAudioContract(response,source,channel);
      const bytesPerSample=contract.bits===24?3:2;
      const frameBytes=contract.channels*bytesPerSample;
      const sampleScale=2**(contract.validBits-1);
      $('#baby-status').textContent='Listening';
      const reader=response.body.getReader();
      let carry=new Uint8Array(0);
      while(true){
        const chunk=await reader.read();
        if(generation!==babyStream.generation)return;
        if(chunk.done)break;
        const bytes=new Uint8Array(carry.length+chunk.value.length);
        bytes.set(carry);
        bytes.set(chunk.value,carry.length);
        const frames=Math.floor(bytes.length/frameBytes);
        const used=frames*frameBytes;
        if(!frames){carry=bytes;continue}
        carry=bytes.slice(used);
        const audio=context.createBuffer(1,frames,contract.rate);
        const samples=audio.getChannelData(0);
        for(let i=0;i<frames;i++){
          const offset=i*frameBytes+contract.channel*bytesPerSample;
          let value;
          if(contract.bits===24){
            value=bytes[offset]|(bytes[offset+1]<<8)|(bytes[offset+2]<<16);
            if(value&0x800000)value|=-16777216;
            samples[i]=Math.max(-1,Math.min(1,value/sampleScale));
          }else{
            value=bytes[offset]|(bytes[offset+1]<<8);
            if(value&0x8000)value|=-65536;
            samples[i]=Math.max(-1,Math.min(1,value/sampleScale));
          }
        }
        const node=context.createBufferSource();
        node.buffer=audio;
        node.connect(gain);
        const start=Math.max(babyStream.nextTime,context.currentTime+0.02);
        node.start(start);
        babyStream.nextTime=start+audio.duration;
      }
    }catch(e){
      if(generation===babyStream.generation){
        if(e.name!=='AbortError'){console.error(e);toast(e.message,true)}
        stopBabyStream();
      }
    }
  };
  $('#baby-stop').onclick=stopBabyStream;
}

/*
 * Simulation: render a phrase with the device's own TTS and play it into the
 * capture path, so wake, STT and the assistant see it exactly as if it had
 * been spoken in the room. The capture mux substitutes it for the microphones
 * without interrupting the stream, so nothing has to be restarted and the wake
 * daemon -- which connects to the mic stream once and never reconnects --
 * keeps running throughout.
 */
const SIM_PHRASES=[
 ['Weather now','Alexa, what is the weather?'],
 ['Weather forecast','Alexa, what is the forecast for tomorrow?'],
 ['Current time','Alexa, what time is it?'],
 ['Current date','Alexa, what is the date today?'],
 ['Set a timer','Alexa, set a timer for five minutes'],
 ['Cancel a timer','Alexa, cancel the timer'],
 ['Set an alarm','Alexa, set an alarm for seven in the morning'],
 ['Play music','Alexa, play some music'],
 ['Pause playback','Alexa, pause'],
 ['Set volume','Alexa, set the volume to four'],
 ['Factual question','Alexa, how tall is the Eiffel Tower?'],
 ['Stop','Alexa, stop'],
 ['No wake word','What time is it?'],
 ['Wake word only','Alexa'],
 ['False start','Alexa, uh, set a timer'],
 ['Self-correction','Alexa, set a five, no, ten minute timer'],
 /* Three ways to ask for one named station. The device matches a station by
    its configured word, so these vary where that word sits in the sentence:
    buried mid-phrase, alone, and trailing. Each cleans up after itself, so
    they are safe to run ahead of the ordered pair below. */
 ['Play a named station','Alexa, play classical radio',simRadioSay('classical')],
 ['Named station alone','Alexa, classical',simRadioSay('classical')],
 ['Play a named station, short','Alexa, play classical',simRadioSay('classical')],
 /* An ordered pair, and last on purpose: the first half leaves a station
    playing and the second half is the only thing that stops it. See the
    block below simRun for what the two halves actually measure. */
 ['Play the radio','Alexa, play the radio',simRadioStart],
 ['Stop the radio while it plays','Alexa, stop',simRadioStop]];
/*
 * The response-time goal: under one second from the end of the user's speech
 * to the first audio out of the speaker.
 *
 * The device does not report that quantity, and this page does not pretend it
 * does. What it records is derived from log lines, and api_log stamps
 * boot_seconds as whole seconds -- so every log-derived duration here lands on
 * a 1000 ms boundary and cannot resolve a sub-second target at all. On top of
 * that, "request to first audio" starts when the simulate request is accepted,
 * which means it also contains the phrase playing into the microphone in real
 * time; a person speaking in the room never pays that. It is an upper bound,
 * not the goal's number.
 *
 * processing_ms is different: agentd measures speech-to-text plus the model
 * itself and reports it in milliseconds. It is a genuine component of the
 * goal's interval and, at 2.8-3.1 s on this hardware, the dominant one. That
 * makes it the closest honest proxy, so it is shown and marked alongside.
 */
const SIM_RESPONSE_GOAL_MS=1000;
/* The prose form of the same number; ms() would render it as "1.00 s". */
const SIM_RESPONSE_GOAL_TEXT='1 s';
function simGoalClass(v){const n=Number(v);return Number.isFinite(n)?(n<SIM_RESPONSE_GOAL_MS?'connected':'error-text'):''}
const SIM_HISTORY_KEY='libreecho-simulation-history';
const SIM_DEVICE_KEY='libreecho-simulation-device-history';
const SIM_DEVICE_GENERATION_KEY='libreecho-simulation-device-history-generation';
const SIM_HISTORY_MAX=100;
let simDeviceTurns=[];
/*
 * Two sources, deliberately not merged.
 *
 * Runs this browser performed know the phrase and whether the wake word fired;
 * nothing on the device records either, so those rows stay in localStorage.
 * But localStorage is scoped per origin and this device takes a new DHCP lease
 * on most boots -- the Wi-Fi driver generates a fresh MAC each time -- so every
 * reboot moved the UI to a new origin and the panel came up empty. agentd keeps
 * the last 12 turns it measured itself for the current device generation.
 *
 * The two cannot be joined: a local row's timings are parsed from log lines
 * stamped in whole seconds, the device's are milliseconds off its own clock, so
 * the same turn appears with different numbers and there is no key to match on.
 * Rather than double-count it in the summary, local rows win when this origin
 * has any and the device's are the fallback that makes a fresh origin useful.
 */
function simLocalHistory(){
 try{const raw=localStorage.getItem(SIM_HISTORY_KEY);const v=raw?JSON.parse(raw):[];
     return Array.isArray(v)?v:[]}catch(_){return []}}
function simLocalSave(list){
 try{localStorage.setItem(SIM_HISTORY_KEY,JSON.stringify(list.slice(0,SIM_HISTORY_MAX)))}catch(_){/* private mode, quota */}}
function simHistory(){const local=simLocalHistory();return local.length?local:simDeviceTurns}
function simShowingDevice(){return !simLocalHistory().length&&simDeviceTurns.length>0}
/*
 * A device turn carries timings only. The field names have to be the ones
 * simParseLogs produces or every column renders blank and simStat finds nothing
 * to summarise; the columns the device cannot fill are left undefined, which ms
 * and simRow render as an em dash.
 */
function simDeviceRow(t){
 if(!Number.isFinite(Number(t.first_pcm_ms))||Number(t.first_pcm_ms)<=0)return null;
 return {at:t.at_ms,source:'device',follow_up:!!t.follow_up,
  audio_ms:t.stt_audio_ms,processing_ms:t.stt_processing_ms,
  queue_to_first_audio_ms:t.first_pcm_ms};}
async function simHistoryLoad(){
 try{
  const h=await api('/assistant/history');
  const turns=Array.isArray(h&&h.turns)?h.turns:[];
  const generation=Number(h&&h.history_generation);
  const previousGeneration=Number(localStorage.getItem(SIM_DEVICE_GENERATION_KEY)||0);
  const remotelyCleared=Number.isFinite(generation)&&generation>0&&previousGeneration>0&&generation!==previousGeneration;
  simDeviceTurns=turns.map(simDeviceRow).filter(Boolean);
  if(Number.isFinite(generation)&&generation>0){try{localStorage.setItem(SIM_DEVICE_GENERATION_KEY,String(generation))}catch(_){/* private mode, quota */}}
  if(remotelyCleared){try{localStorage.removeItem(SIM_DEVICE_KEY)}catch(_){/* private mode, quota */}}
  else if(simDeviceTurns.length){try{localStorage.setItem(SIM_DEVICE_KEY,JSON.stringify(simDeviceTurns))}catch(_){/* private mode, quota */}}
  else {try{const raw=localStorage.getItem(SIM_DEVICE_KEY);const v=raw?JSON.parse(raw):[];if(Array.isArray(v))simDeviceTurns=v}catch(_){} }
 }catch(_){
  /* Unreachable, or an image whose agentd predates /assistant/history: show the
     last copy fetched rather than nothing. */
  try{const raw=localStorage.getItem(SIM_DEVICE_KEY);const v=raw?JSON.parse(raw):[];
      if(Array.isArray(v))simDeviceTurns=v}catch(_){}
 }
 return simHistory();}
async function simHistoryClear(){
 await api('/assistant/history/clear',{method:'POST',body:'{}'});
 simDeviceTurns=[];
 try{localStorage.removeItem(SIM_HISTORY_KEY);localStorage.removeItem(SIM_DEVICE_KEY);localStorage.removeItem('libreecho-simulation-device-history-cleared-at')}catch(_){} }
function ms(v){return (v===null||v===undefined)?'—':(v>=1000?(v/1000).toFixed(2)+' s':Math.round(v)+' ms')}
/*
 * Pull the turn's real timings out of the device log rather than inferring
 * them from poll intervals. agentd reports audio_ms, processing_ms and
 * total_ms for the turn, and ttsd marks when the first PCM chunk reaches the
 * bus -- which is the moment a person actually hears an answer. Correlated by
 * boot_seconds, taking only lines newer than the mark taken before the run.
 */
function simParseLogs(entries,since){
 const out={};
 /*
  * Every marker below must come from the turn being measured, so nothing is
  * read until this turn's queue line has gone by. boot_seconds is stamped in
  * whole seconds, so `since` alone does not separate turns: when the previous
  * reply was still being spoken in the same second the request went out, its
  * "streaming first PCM chunk" line passed the filter and was paired with this
  * turn's queue line, reporting first audio several seconds before the request
  * that supposedly caused it. Anchoring on the queue line is the best split
  * available at this resolution; a turn whose queue line never arrives reports
  * nothing rather than a number attributed to the wrong turn.
  */
 let started=false;
 for(const e of entries){
  if(e.boot_seconds<since)continue;
  const m=e.message||'';
  let g;
  if(/Simulated audio queued/.test(m)&&out.queued_at===undefined){
   out.queued_at=e.boot_seconds;started=true;continue;
  }
  if(!started)continue;
  if((g=m.match(/audio_ms=(\d+)\s+processing_ms=(\d+)\s+total_ms=(\d+)/))){
   out.audio_ms=+g[1]; out.processing_ms=+g[2]; out.total_ms=+g[3];
   out.transcript_at=e.boot_seconds;
   const chars=m.match(/text_chars=(\d+)/); if(chars)out.text_chars=+chars[1];
  }
  if(/speak requested/.test(m)&&out.speak_at===undefined)out.speak_at=e.boot_seconds;
  if(/streaming first PCM chunk/.test(m)&&out.first_audio_at===undefined)out.first_audio_at=e.boot_seconds;
 }
 /* Time does not run backwards. A negative span means the markers were not
    both from this turn after all, so drop it rather than render it. */
 const span=(a,b)=>(a===undefined||b===undefined||b<a)?undefined:(b-a)*1000;
 out.queue_to_transcript_ms=span(out.queued_at,out.transcript_at);
 out.transcript_to_audio_ms=span(out.transcript_at,out.first_audio_at);
 out.queue_to_first_audio_ms=span(out.queued_at,out.first_audio_at);
 return out;
}
function simRow(r){
 const cap=r.audio_ms&&r.max_utterance_ms&&r.audio_ms>=r.max_utterance_ms-100;
 /* A device row records no phrase and no wake result. Those columns say so
    rather than reading as an empty phrase that failed to wake the device. */
 const text=r.text||(r.source==='device'?(r.follow_up?'spoken turn (follow-up)':'spoken turn'):'');
 return `<tr><td>${esc(new Date(r.at).toLocaleTimeString())}</td>`+
  `<td class="${r.wake===true?'connected':''}">${r.wake===undefined||r.wake===null?'—':(r.wake?'yes':'no')}</td>`+
  `<td>${esc(text.length>34?text.slice(0,34)+'…':text)}`+
   /* The radio pair is the only row kind with a verdict the columns cannot
      hold, so it is written under the phrase rather than given a column that
      is empty on every other row. */
   (r.radio_status?`<small class="${r.voice_stopped===false?'error-text':''}">${esc(r.radio_status)}${r.radio_stop_ms!=null?' in '+ms(r.radio_stop_ms):''}</small>`:'')+`</td>`+
  `<td>${ms(r.wake_latency_ms)}</td>`+
  `<td class="${cap?'error-text':''}">${ms(r.audio_ms)}${cap?' (cap)':''}</td>`+
  `<td class="${simGoalClass(r.processing_ms)}">${ms(r.processing_ms)}</td>`+
  `<td class="${simGoalClass(r.queue_to_first_audio_ms)}">${ms(r.queue_to_first_audio_ms)}</td></tr>`;
}
/* Best and median rather than a mean: one run that hit the utterance cap or
   lost the network would drag an average somewhere no run actually was. */
function simStat(list,key){
 const v=list.map(r=>Number(r[key])).filter(n=>Number.isFinite(n)).sort((a,b)=>a-b);
 if(!v.length)return null;
 const mid=v.length>>1;
 return {best:v[0],median:v.length%2?v[mid]:Math.round((v[mid-1]+v[mid])/2),count:v.length};}
function simStatRow(label,stat){
 if(!stat)return '';
 return `<dt>${esc(label)}</dt><dd>best <span class="${simGoalClass(stat.best)}">${ms(stat.best)}</span>`+
  ` · median <span class="${simGoalClass(stat.median)}">${ms(stat.median)}</span>`+
  ` <small>over ${stat.count} run${stat.count===1?'':'s'}</small></dd>`;}
function simSummaryHtml(h){
 const rows=simStatRow('STT + model',simStat(h,'processing_ms'))+
   simStatRow('Request → first audio',simStat(h,'queue_to_first_audio_ms'));
 if(!rows)return '';
 return `<dl class="facts sim-summary">${rows}</dl>`+
  `<p class="muted">Measured against the goal of under ${SIM_RESPONSE_GOAL_TEXT}. The largest cost today is speech-to-text plus the model.</p>`;}
function simRender(){
 const h=simHistory();
 const el=$('#sim-history'); if(!el)return;
 if(!h.length){el.innerHTML='<p class="muted">No runs yet.</p>';return}
 const note=simShowingDevice()
  ? '<p class="muted">Showing the last '+h.length+(h.length===1?' turn':' turns')+' the device measured itself for this device generation. It does not record the phrase or the wake result &mdash; run a simulation to fill those columns.</p>'
  : '';
 el.innerHTML=note+simSummaryHtml(h)+'<div class="table-scroll"><table class="sim-table"><thead><tr>'+
  '<th>time</th><th>wake</th><th>phrase</th><th>wake latency</th>'+
  '<th>turn audio</th><th>stt + model</th><th>request → first audio</th></tr></thead><tbody>'+
  h.map(simRow).join('')+'</tbody></table></div>';
 /* The device keeps its own, shorter ring, so the ceiling shown has to follow
    whichever source is on screen. */
 const c=$('#sim-count-runs');
 if(c)c.textContent=simShowingDevice()?h.length+' from the device':h.length+' of '+SIM_HISTORY_MAX;
}
/*
 * One simulated utterance, start to finish. Shared by the single-phrase button
 * and the run-all sweep so both record exactly the same fields -- a history
 * where some rows were measured differently is worse than no history.
 *
 * onSent fires the instant the device accepted the phrase. A caller that wants
 * to time the device's reaction to the utterance needs that anchor: simRun's
 * own t0 is one request earlier, and everything after it is inside this
 * function's poll loop.
 */
async function simRun(phrase,cap,onStatus,onSent){
 const pre=await api('/logs').catch(()=>({entries:[]}));
 const mark=(pre.entries||[]).reduce((m,e)=>Math.max(m,e.boot_seconds||0),0);
 const before=(await api('/wake-word').catch(()=>({}))).detected_count??0;
 const t0=performance.now();
 await api('/audio/simulate',{method:'POST',body:JSON.stringify({text:phrase})});
 if(onSent)onSent();
 let wake=false,wakeLatency=null;
 for(let i=0;i<40&&!wake;i++){
  await new Promise(r=>setTimeout(r,250));
  const st=await api('/wake-word').catch(()=>null);
  if(st&&(st.detected_count??before)>before){wake=true;wakeLatency=performance.now()-t0}
 }
 if(onStatus)onStatus(wake?'Wake detected, waiting for the answer…':'No wake yet…');
 await new Promise(r=>setTimeout(r,7000));
 const post=await api('/logs').catch(()=>({entries:[]}));
 const entry={at:new Date().toISOString(),text:phrase,wake,
   wake_latency_ms:wakeLatency?Math.round(wakeLatency):null,
   max_utterance_ms:cap,...simParseLogs(post.entries||[],mark)};
 simLocalSave([entry,...simLocalHistory()]);
 simRender();
 return entry;
}
/*
 * Internet radio, as an ordered pair.
 *
 * "Play the radio" and the "Alexa, stop" that follows it are one test in two
 * halves and only mean anything run in that order: the first leaves a station
 * playing, the second says stop into that music and checks that the music
 * actually stopped. Having sent the phrase proves nothing on its own -- the
 * failure worth catching is a turn that is woken, transcribed and answered
 * while the stream carries straight on -- so the verdict comes from radiod's
 * own playing flag over the API, not from a log line.
 *
 * agentd has no radio path: nothing in it refers to radio at all, and it ships
 * inside a squashfs payload that OTA cannot replace, so a spoken "play the
 * radio" cannot reach radiod on this image. The first half therefore speaks
 * the phrase, looks to see whether that started anything, falls back to the
 * API when it did not, and records which of the two actually did it. Read the
 * pair as a measurement of the stop path over music, not as evidence that
 * voice can start playback.
 */
const SIM_RADIO_POLL_MS=250;
/* 40 ticks is 10 s, the same window simRun already watches for a wake. */
const SIM_RADIO_TICKS=40;
/*
 * Two independent witnesses that the stream really stopped: radiod's playing
 * flag, and the amplifier, which powers up only when PCM is actually flowing.
 * They are allowed to disagree, and it is worth knowing when they do -- the
 * amplifier also stays up while the device speaks its own reply, so it lags
 * the stream rather than contradicting it. Both are recorded; only the playing
 * flag decides the verdict.
 */
async function simRadioWitness(){
 const [r,a]=await Promise.all([api('/integrations/radio').catch(()=>null),
                               api('/audio').catch(()=>null)]);
 return {playing:r?r.playing===true:null,supported:r?r.playback_supported!==false:null,
         url:r?(r.playing_url||''):'',amplifier_on:a?a.amplifier_on===true:null};}
/* Bounded by tick count rather than by a deadline, so the loop still ends on a
   device that never changes its mind. */
async function simRadioAwait(want,ticks){
 let w=await simRadioWitness();
 for(let i=0;i<ticks&&!want(w);i++){
  await new Promise(r=>setTimeout(r,SIM_RADIO_POLL_MS));
  w=await simRadioWitness();}
 return w;}
/* The default station is the first enabled one in the device's own list --
   the one the shortest "play <word>" would reach for. */
function simRadioDefault(r){
 const s=Array.isArray(r&&r.stations)?r.stations:[];
 return s.find(x=>x.enabled!==false)||null;}
/* simRun has already stored the row by the time the radio verdict is known, so
   the stored copy is rewritten in place rather than appended a second time. */
function simHistoryAmend(entry){
 const h=simLocalHistory();
 if(h.length&&h[0].at===entry.at)h[0]=entry;
 simLocalSave(h); simRender();}
/*
 * Unconditional, pass or fail: a stop against a stream that already stopped is
 * harmless, and skipping it on the strength of the last poll would leave the
 * radio playing whenever that poll was wrong. Every later phrase in the sweep
 * would then be measured over music.
 */
async function simRadioCleanup(){
 await api('/integrations/radio/stop',{method:'POST',body:'{}'}).catch(()=>null);
 return simRadioAwait(w=>w.playing!==true,8);}
/*
 * "Play <station>" by name. Unlike simRadioStart this never falls back to the
 * API: the question is whether speech alone reaches the player, and a fallback
 * would record the same "playing" for a device that ignored the phrase
 * entirely. It also checks which stream came up -- "play classical" that
 * starts FIP is a failure, and matching on playing alone would call it a pass.
 */
function simRadioSay(word){
 return async function(phrase,cap,onStatus){
  const r=await api('/integrations/radio').catch(()=>null);
  const stations=Array.isArray(r&&r.stations)?r.stations:[];
  const station=stations.find(x=>String(x.word||'').toLowerCase()===word)||null;
  const before=await simRadioWitness();
  const entry=await simRun(phrase,cap,onStatus);
  entry.radio_station=word; entry.radio_started_by=null; entry.radio_playing=false;
  if(!r||r.playback_supported===false||!station){
   entry.radio_status=!r?'radio unavailable':r.playback_supported===false?
     'no stream player on this image':'no \u201c'+word+'\u201d station configured';
   simHistoryAmend(entry); return entry;}
  if(before.playing===true){
   /* Something was already streaming, so nothing this turn does can be told
      apart from it. Say so rather than crediting the phrase. */
   entry.radio_status='inconclusive \u2014 a stream was already playing';
   const left=await simRadioCleanup();
   state.simRadio={playing:left.playing===true};
   simHistoryAmend(entry); return entry;}
  const w=await simRadioAwait(x=>x.playing===true,SIM_RADIO_TICKS);
  entry.radio_playing=w.playing===true; entry.radio_url=w.url; entry.amplifier_on=w.amplifier_on;
  if(!entry.radio_playing)entry.radio_status='did not start';
  else{entry.radio_started_by='voice';
       entry.radio_status=station.url&&w.url&&w.url!==station.url?
         'started, but not \u201c'+word+'\u201d':'playing (voice)';}
  /* Left playing, it would put music under every later phrase in the sweep. */
  const after=await simRadioCleanup();
  entry.radio_playing_after=after.playing;
  state.simRadio={playing:after.playing===true};
  simHistoryAmend(entry); return entry;};}
async function simRadioStart(phrase,cap,onStatus){
 const r=await api('/integrations/radio').catch(()=>null);
 const station=simRadioDefault(r);
 /* Whether a stream was already running before the phrase. Without this, a
    radio someone had left on would be recorded as one the turn started, which
    is the one result here worth being careful not to invent. */
 const already=!!(r&&r.playing);
 const entry=await simRun(phrase,cap,onStatus);
 entry.radio_station=station?station.word:null;
 entry.radio_started_by=null; entry.radio_playing=false;
 if(!r||r.playback_supported===false||!station){
  entry.radio_status=!r?'radio unavailable':r.playback_supported===false?
    'no stream player on this image':'no station configured';
  state.simRadio={playing:false};
  simHistoryAmend(entry); return entry;}
 let w=await simRadioWitness();
 if(w.playing)entry.radio_started_by=already?'already playing':'voice';
 else{
  if(onStatus)onStatus('The turn started nothing; starting “'+station.word+'” over the API…');
  try{ await api('/integrations/radio/play',{method:'POST',body:JSON.stringify({word:station.word})});
       entry.radio_started_by='api'; }
  catch(e){ entry.radio_status='could not start: '+e.message;
            state.simRadio={playing:false};
            simHistoryAmend(entry); return entry; }
  w=await simRadioAwait(x=>x.playing===true,SIM_RADIO_TICKS);}
 entry.radio_playing=w.playing===true;
 entry.radio_url=w.url; entry.amplifier_on=w.amplifier_on;
 entry.radio_status=entry.radio_playing?'playing ('+entry.radio_started_by+')':'did not start';
 state.simRadio={playing:entry.radio_playing,station:station.word};
 simHistoryAmend(entry); return entry;}
/*
 * "Alexa, stop" spoken into a playing stream. The clock starts when the device
 * accepts the phrase, so the recorded duration also covers the phrase playing
 * into the microphone in real time, the wake, the transcript and the model --
 * the same upper-bound caveat as request → first audio. It is polled at
 * SIM_RADIO_POLL_MS, so it is no finer than a quarter second either; unlike
 * the log-derived rows it is at least not quantised to whole seconds.
 *
 * It measures a coincidence, not a cause: a stream that died of its own accord
 * during the turn would read the same way. What it can do reliably is fail --
 * a stream still playing at the end of the turn did not stop.
 */
async function simRadioStop(phrase,cap,onStatus){
 const before=await simRadioWitness();
 let entry=null,t0=null,stoppedAt=null,ampAt=null,watching=true;
 if(before.playing!==true){
  /* Nothing was playing, so nothing here would be barge-in. Say that, rather
     than speaking into silence and recording it as a pass. */
  entry=await simRun(phrase,cap,onStatus);
  entry.radio_status='skipped — no stream was playing';
  entry.voice_stopped=null; entry.radio_stop_ms=null; entry.amplifier_off_ms=null;
  state.simRadio={playing:false};
  simHistoryAmend(entry); return entry;}
 try{
  const turn=simRun(phrase,cap,onStatus,()=>{t0=performance.now()});
  /* Watched while the turn is still in flight. Waiting for simRun to return
     would put its whole answer window between the stop and the measurement. */
  const watch=(async()=>{
   for(let i=0;i<SIM_RADIO_TICKS&&watching&&(stoppedAt===null||ampAt===null);i++){
    await new Promise(r=>setTimeout(r,SIM_RADIO_POLL_MS));
    if(t0===null)continue;   /* the phrase has not been accepted yet */
    const w=await simRadioWitness();
    if(stoppedAt===null&&w.playing===false)stoppedAt=performance.now();
    if(ampAt===null&&w.amplifier_on===false)ampAt=performance.now();}})();
  entry=await turn; await watch;
  entry.voice_stopped=stoppedAt!==null;
  entry.radio_stop_ms=stoppedAt!==null&&t0!==null?Math.round(stoppedAt-t0):null;
  entry.amplifier_off_ms=ampAt!==null&&t0!==null?Math.round(ampAt-t0):null;
  entry.radio_status=entry.voice_stopped?'stopped by voice':'still playing after the turn';
 }finally{
  /* A turn that threw leaves the watcher polling; stop it before the
     cleanup rather than letting it outlive the run. */
  watching=false;
  const after=await simRadioCleanup();
  if(entry){
   entry.radio_stopped_by=entry.voice_stopped?'voice':after.playing!==true?'API cleanup':'not stopped';
   entry.radio_playing_after=after.playing;
   simHistoryAmend(entry);}
  state.simRadio={playing:after.playing===true};}
 return entry;}
function simRadioTimingHtml(e){
 if(!e||e.radio_status===undefined)return '';
 const good=e.voice_stopped===true||e.radio_playing===true;
 return '<dt>Radio</dt><dd class="'+(e.voice_stopped===null?'':good?'connected':'error-text')+'">'+esc(e.radio_status)+'</dd>'+
  (e.radio_station?'<dt>Station</dt><dd>'+esc(e.radio_station)+'</dd>':'')+
  (e.radio_started_by?'<dt>Started by</dt><dd class="'+(e.radio_started_by==='voice'?'connected':'')+'">'+esc(e.radio_started_by)+'</dd>':'')+
  (e.radio_stopped_by?'<dt>Stopped by</dt><dd class="'+(e.voice_stopped?'connected':'error-text')+'">'+esc(e.radio_stopped_by)+'</dd>':'')+
  (e.radio_stop_ms!==undefined?'<dt>Stop → playback ceased</dt><dd>'+ms(e.radio_stop_ms)+'</dd>':'')+
  (e.amplifier_off_ms!==undefined?'<dt>Stop → amplifier off</dt><dd>'+ms(e.amplifier_off_ms)+'</dd>':'');}
function simTimingHtml(entry,cap){
 const capped=entry.audio_ms&&cap&&entry.audio_ms>=cap-100;
 return '<dt>Wake</dt><dd class="'+(entry.wake?'connected':'')+'">'+(entry.wake?'detected':'not detected')+'</dd>'+
  '<dt>Wake latency</dt><dd>'+ms(entry.wake_latency_ms)+'</dd>'+
  '<dt>Turn audio</dt><dd class="'+(capped?'error-text':'')+'">'+ms(entry.audio_ms)+
    (capped?' — hit the utterance cap, endpointing did not fire':'')+'</dd>'+
  '<dt>STT + model</dt><dd class="'+simGoalClass(entry.processing_ms)+'">'+ms(entry.processing_ms)+'</dd>'+
  '<dt>Request → transcript</dt><dd>'+ms(entry.queue_to_transcript_ms)+'</dd>'+
  '<dt>Transcript → first audio</dt><dd>'+ms(entry.transcript_to_audio_ms)+'</dd>'+
  '<dt>Request → first audio</dt><dd class="'+simGoalClass(entry.queue_to_first_audio_ms)+'">'+ms(entry.queue_to_first_audio_ms)+'</dd>'+
  simRadioTimingHtml(entry);
}
/*
 * The hardware specification, collapsed. This is the map that the wake and
 * button work was done against, and every row of it was read off the running
 * device rather than inferred from the device tree -- where the two disagree,
 * the device wins. It lives on this page because Simulation is where someone
 * goes when the pipeline misbehaves, and the first question is always which
 * pin or codec is involved.
 *
 * Deliberately static. Reading it live would mean shipping an endpoint that
 * dumps gpio, i2c and USB descriptor state, and the USB gadget serial is the
 * device's real IDME serial -- see the note in the USB table.
 */
/* mono=true only where the first column is an identifier (GPIO number, I2C
   address, PCM node); prose like "Volume Down" reads badly in monospace. */
function specTable(head,rows,mono){return `<div class="table-scroll"><table class="sim-table spec-table${mono?' spec-mono':''}"><thead><tr>${head.map(h=>`<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>${rows.map(r=>`<tr>${r.map(c=>`<td>${esc(c)}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`}
const SPEC_GPIO=[
 ['392','35','KPROW2','aic3101_enable','out hi — mic-array codec enable'],
 ['393','36','KPCOL0','Action Key','in, IRQ, active-low'],
 ['394','37','KPCOL1','Volume Down','in, IRQ, active-low'],
 ['400','43','EINT11','spi0.0','out hi — audio FPGA'],
 ['407','50','EINT18','Volume Up','in, IRQ, active-low'],
 ['408','51','EINT19','tlv320aic32x4 rstn','out hi — speaker codec reset'],
 ['444','87','MSDC2_DAT0','amz_privacy','out, active-low — mute lamp + privacy cut'],
 ['445','88','MSDC2_DAT1','amz-bright-state','out, active-low — inverse of privacy'],
 ['390','33','KPROW0','— unclaimed','floats; fires spurious presses'],
 ['391','34','KPROW1','— unclaimed','floats; fires spurious presses']];
const SPEC_I2C=[
 ['0-0018…1b','tlv320aic3101 ×4','bound','mic-array codecs'],
 ['0-0029','tsl2584tsv','—','ambient light'],
 ['0-002c','lp855x-led','—','backlight controller'],
 ['0-0039','tsl2540','bound','ambient light'],
 ['0-003f','is31fl3236','is31fl32xx','light ring, 12 RGB, all 36 channels used'],
 ['0-0032','lp55231','absent','probe returns ENXIO — not populated'],
 ['1-0060','sym827-regulator','—','regulator'],
 ['2-0018','tlv320aic32x4','bound','speaker codec']];
const SPEC_BUTTONS=[
 ['Mute','PMIC, mt6323keys','113 KEY_MUTE','toggles the privacy latch; lamp on GPIO 444'],
 ['Action','KPCOL0, gpio-keys','138 KEY_HELP','input only — there is no lamp on this button'],
 ['Volume Up','pin 50, gpio-keys','115 KEY_VOLUMEUP',''],
 ['Volume Down','pin 37, gpio-keys','114 KEY_VOLUMEDOWN','']];
const SPEC_USB=[
 ['Controller','musb-mtk @ 11200000.usb','UDC musb-hdrc.1.auto, high-speed'],
 ['Role switch','11200000.usb-role-switch','device by default; host needs the vendor session-edge restart'],
 ['Gadget','configfs usb_gadget/libreecho','function ffs.adb, FunctionFS at /dev/usb-ffs/adb'],
 ['VID:PID','18d1:d001','Google / Nexus-4 fastboot ID'],
 ['Strings','LibreEcho / MT8163-ARM32-ADB','the serial string is the real IDME serial — a leak vector if USB descriptors are ever captured'],
 ['VBUS','none','the port never sources power, so a drive must be self-powered'],
 ['Exclusivity','one port, one use','a drive takes the gadget at t+60 s; ADB and storage cannot coexist']];
const SPEC_AUDIO=[
 ['pcm23p / pcm25p','playback',''],
 ['pcm24c','capture','9 ch, 24-bit, 16 kHz — ch0–6 live, ch7/8 silent'],
 ['Bus format','mono S16LE 48 kHz','/run/libreecho-audio/{system,media}.pcm'],
 ['Other','auxadc, thermal, storage','16 ADC channels, 4 thermal zones, eMMC mmcblk0 ≈3.8 GB'],
 ['wlan0','MAC','randomised on every boot']];
function specSection(){return collapsiblePanel('Hardware specification',
  `<p class="muted">Read off the running device, not taken from the device tree. GPIO numbering starts at 357, so <strong>DTS pin = GPIO &minus; 357</strong>.</p>`+
  `<h4>GPIO</h4>`+specTable(['GPIO','Pin','SoC name','Claimed by','State'],SPEC_GPIO,1)+
  `<p class="muted">The buttons are discrete GPIOs, not a scan matrix — the vendor <code>keypad@10010000</code> node is inherited SoC boilerplate with no driver behind it, and its matrix index does not give you the pin. Identify a pin by counting presses over a window, never from one event: KPCOL0 reported 18 times across deliberate presses while KPROW0 and KPROW1 each fired once at boot and never again. Those two float, and trusting a single boot fire wired the wrong pin twice.</p>`+
  `<h4>Buttons</h4>`+specTable(['Button','Source','Keycode','Notes'],SPEC_BUTTONS)+
  `<h4>I²C</h4>`+specTable(['Address','Device','Driver','Role'],SPEC_I2C,1)+
  `<h4>USB and ADB</h4>`+specTable(['Item','Value','Notes'],SPEC_USB)+
  `<h4>Audio</h4>`+specTable(['Interface','Role','Notes'],SPEC_AUDIO,1),
  'sim-spec')}

async function simulationPage(){
 await simHistoryLoad();   /* device-side history for the current generation */
 const w=await api('/wake-word').catch(()=>({}));
 const vp=await api('/voice-pipeline').catch(()=>({}));
 const cap=vp?.listening?.max_utterance_ms;
 content.innerHTML=`<div class="settings-grid">${panel('Speak a phrase',
   `<label class="field"><span>Phrase</span><select id="sim-preset">`+
   SIM_PHRASES.map(([l],i)=>`<option value="${i}">${esc(l)}</option>`).join('')+
   `<option value="custom">Custom…</option></select></label>`+
   field('Text','Alexa, what time is it?','sim-text')+
   `<p class="muted">Rendered by the device's own text-to-speech and played into the microphone path. Nothing is recorded and nothing leaves the device.</p>`+
   `<p class="muted">The last two presets are one test in two halves and are meant to run in that order: the first leaves a station playing, the second says stop into that music and checks with radiod that playback really ceased. The radio is stopped again afterwards either way.</p>`+
   `<div class="button-row">${action('Speak into the microphone','sim-send','primary-btn')}${action('Run all '+SIM_PHRASES.length+' phrases','sim-run-all')}<button class="secondary-btn" id="sim-stop-all" hidden>Stop</button></div>`+
   `<dl class="facts"><dt>Wake word</dt><dd>${esc(w.wake_word||'—')}</dd><dt>Sensitivity</dt><dd>${w.sensitivity??'—'}</dd><dt>Utterance cap</dt><dd>${ms(cap)}</dd></dl>`)}
   ${panel('Last run',`<p class="muted sim-goal">Goal: under ${SIM_RESPONSE_GOAL_TEXT} from the end of speech to the first audio out of the speaker.</p><dl class="facts" id="sim-timing"><dt>Status</dt><dd>Nothing sent yet</dd></dl><p class="muted">The device does not report end-of-speech to first audio, so it is not shown. The log-derived rows here are stamped in whole seconds, and <em>request → first audio</em> also covers the phrase playing into the microphone in real time, which a person speaking in the room would not pay — read it as an upper bound. <em>STT + model</em> is measured by agentd in milliseconds and is the closest honest proxy, and the dominant cost.</p>`)}
   </div>
   ${panel('History',`<div class="button-row"><span class="muted" id="sim-count-runs">0 of ${SIM_HISTORY_MAX}</span>`+
     action('Download JSON','sim-download')+action('Clear','sim-clear')+`</div><div id="sim-history"></div>`,'sim-history-panel')}
   ${specSection()}`;
 const preset=$('#sim-preset'),text=$('#sim-text');
 preset.onchange=()=>{ if(preset.value!=='custom') text.value=SIM_PHRASES[+preset.value][1]; };
 text.oninput=()=>{ preset.value='custom'; };
 simRender();
 $('#sim-download').onclick=()=>{
  const blob=new Blob([JSON.stringify({exported:new Date().toISOString(),
    device:location.host,max_utterance_ms:cap,runs:simHistory()},null,2)],
    {type:'application/json'});
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob);
  a.download='libreecho-simulation-'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(()=>URL.revokeObjectURL(a.href),2000);
 };
 $('#sim-clear').onclick=async()=>{try{await simHistoryClear();simRender();toast('History cleared')}catch(e){toast(e.message,true)}};
 $('#sim-send').onclick=async()=>{
  const phrase=text.value.trim();
  if(!phrase){toast('Enter a phrase first',true);return}
  if(state.simBusy)return;
  const timing=$('#sim-timing');
  state.simBusy=true;
  timing.innerHTML='<dt>Status</dt><dd>Rendering and playing…</dd>';
  /* A preset with its own runner keeps it only while its text is unedited: an
     edited phrase is a custom phrase and gets the plain run. */
  const run=(preset.value!=='custom'&&SIM_PHRASES[+preset.value][1]===phrase&&SIM_PHRASES[+preset.value][2])||simRun;
  try{
   const entry=await run(phrase,cap,t=>{timing.innerHTML='<dt>Status</dt><dd>'+esc(t)+'</dd>'});
   timing.innerHTML=simTimingHtml(entry,cap);
   toast(entry.wake?'Wake detected':'No wake detected',!entry.wake);
  }catch(e){ const busy=/already playing/i.test(e.message);
    timing.innerHTML='<dt>Status</dt><dd>'+esc(e.message)+'</dd>';
    toast(busy?'Still playing the previous phrase — try again in a moment':e.message,true); }
  finally{ state.simBusy=false; }
 };
 /*
  * Run every preset in turn. Each utterance makes the device answer out loud,
  * so leave a gap between them: starting the next phrase while the speaker is
  * still going would be testing barge-in, not the phrase.
  *
  * The radio pair at the end is the exception -- it is barge-in on purpose --
  * and it carries its own runner, so the sweep dispatches to the third element
  * of the preset when there is one. It runs last and stops the radio when it
  * finishes, so nothing after it is measured over music.
  */
 $('#sim-run-all').onclick=async()=>{
  if(state.simBusy)return;
  state.simBusy=true; state.simStop=false;
  const timing=$('#sim-timing'),btn=$('#sim-run-all'),stop=$('#sim-stop-all');
  btn.disabled=true; if(stop)stop.hidden=false;
  let done=0,woke=0,barge='';
  try{
   for(const [label,phrase,runner] of SIM_PHRASES){
    if(state.simStop){toast('Stopped after '+done+' phrases');break}
    timing.innerHTML='<dt>Running</dt><dd>'+esc(label)+' ('+(done+1)+' of '+SIM_PHRASES.length+')</dd>';
    const run=runner||simRun;
    const note=e=>{ if(!e)return; if(e.wake)woke++;
      if(run===simRadioStop)barge=e.voice_stopped===null?'; the stop half was skipped, nothing was playing'
        :'; the radio '+(e.voice_stopped?'stopped':'did not stop')+' on voice'; };
    try{ note(await run(phrase,cap)); }
    catch(err){ if(/already playing/i.test(err.message||'')){
        /* The device was still speaking. Wait it out and retry once,
           rather than recording a failure that is really contention. */
        await new Promise(r=>setTimeout(r,6000));
        try{ note(await run(phrase,cap)); }catch(_){}
      } }
    done++;
    if(!state.simStop&&done<SIM_PHRASES.length)await new Promise(r=>setTimeout(r,3000));
   }
   timing.innerHTML='<dt>Sweep complete</dt><dd>'+woke+' of '+done+' phrases woke the device'+esc(barge)+'</dd>';
   toast(woke+' of '+done+' woke the device');
  } finally {
   /* A sweep stopped part-way through can be stopped between the two halves
      of the radio pair, which would leave the stream running. */
   if(state.simRadio&&state.simRadio.playing)await simRadioCleanup().catch(()=>null);
   state.simBusy=false; btn.disabled=false; if(stop)stop.hidden=true;
  }
 };
 if($('#sim-stop-all'))$('#sim-stop-all').onclick=()=>{state.simStop=true};

}
async function wakePage(){let w;try{w=await api('/wake-word')}catch(e){if(/not available|not supported/i.test(e.message)){content.innerHTML=`<div class="settings-grid">${panel('Wake-word engine',unsupported('The wake-word adapter is not installed in this image. No microphone processing is running.'))}</div>`;return}throw e}const approved=['LibreEcho','Computer','Echo','Custom model'],current=String(w.wake_word||''),wakeOptions=approved.includes(current)?approved:[current+' (current)',...approved];content.innerHTML=`<div class="settings-grid">${panel('Wake-word engine',`<dl class="facts"><dt>Detection</dt><dd class="${w.enabled?'connected':''}">${w.enabled?'Enabled':'Disabled'}</dd></dl><p class="muted">Detection enablement is reported by the live wake-word adapter and is not configurable through this API.</p>`+select('Active wake word',current,'wake-word',wakeOptions)+range('Detection sensitivity',w.sensitivity,'sensitivity')+`<dl class="facts"><dt>Detection cooldown</dt><dd>${w.cooldown_ms} ms</dd></dl>`+`<div class="button-row">${saveButton('save-wake')}${action('Simulate detection','wake-test')}</div>`)}${panel('Local processing',`<div class="privacy-callout">✓ Audio remains on this device</div><dl class="facts"><dt>Model status</dt><dd class="connected">${esc(w.model_status)}</dd><dt>Detections</dt><dd>${w.detected_count}</dd><dt>Estimated CPU cost</dt><dd>${w.cpu_cost_percent}%</dd><dt>Estimated memory cost</dt><dd>${w.memory_cost_mb} MB</dd></dl>`)}</div>`;bindRange();bindDirty(['#wake-word','#sensitivity'],'#save-wake');$('#save-wake').onclick=()=>mutate('/wake-word',{wake_word:$('#wake-word').value.replace(/ \(current\)$/,''),sensitivity:+$('#sensitivity').value},'Wake-word changes saved');$('#wake-test').onclick=()=>post('/wake-word/test',{},'Wake word detected')}
function ledRing(l){const physical=Array.isArray(l.pixels)&&l.pixels.length===12,colours=physical?l.pixels:Array.from({length:24},()=>l.colour),count=colours.length,opacity=physical?1:Math.max(0,Math.min(1,Number(l.brightness??0)/100));const dots=colours.map((colour,i)=>{const angle=(i/count)*Math.PI*2-Math.PI/2,x=50+38*Math.cos(angle),y=50+38*Math.sin(angle);return `<circle class="led-pixel" cx="${x.toFixed(2)}" cy="${y.toFixed(2)}" r="${physical?'6.4':'5.5'}" fill="${rgb(colour)}" opacity="${opacity.toFixed(2)}"></circle>`}).join('');const label=l.pattern_active?`Pattern · ${esc(l.pattern||'pattern')}`:l.visualizer_active?`Music visualizer · ${esc(l.visualizer_mood||'balanced')}`:l.animation_active?`Animating · ${esc(l.animation_profile||'profile')}`:'Steady',moving=l.animation_active||l.pattern_active;return `<div class="led-ring-view${moving?' animating':''}${l.visualizer_active?' reactive':''}" role="img" aria-label="${esc(label)} LED ring"><svg viewBox="0 0 100 100" aria-hidden="true"><circle class="led-ring-track" cx="50" cy="50" r="38"></circle>${dots}<circle class="led-ring-centre" cx="50" cy="50" r="24"></circle></svg><span>${esc(label)}</span></div>`}
function updateLedVisual(l){const ring=$('.led-ring-view');if(ring)ring.outerHTML=ledRing(l);const preview=$('.led-preview');if(preview)preview.style.setProperty('--led',rgb(l.colour))}
async function refreshLed(){if(state.page!=='LED & Buttons')return;let delay=1000;try{const l=await api('/led');updateLedVisual(l);if(l.visualizer_active)delay=200}catch(_){/* Preserve the last good LED state during a transient adapter failure. */}finally{if(state.page==='LED & Buttons')state.timer=setTimeout(refreshLed,delay)}}
/*
 * The action button is a sound machine: one curated list, and each press plays
 * the next entry. The list is stored as a comma-separated string of sound
 * names in play order -- the order the rows appear is the order they rotate --
 * because buttond re-reads it from the config file on every tick, and a flat
 * string is the cheapest thing for it to parse.
 *
 * Sounds come from whatever is present in the device's sound directory rather
 * than a hard-coded list, so dropping a new .raw in makes it selectable here
 * without a UI change.
 */
function actionSoundPanel(b){
 const available=Array.isArray(b.available_sounds)?b.available_sounds:[];
 const enabled=(b.action_sounds||'').split(',').map(x=>x.trim()).filter(Boolean);
 /* Enabled first, in rotation order, then the rest: the list IS the running
    order, so showing it in any other order would misdescribe it. */
 const ordered=enabled.filter(n=>available.includes(n))
   .concat(available.filter(n=>!enabled.includes(n)));
 const rows=ordered.length?ordered.map(name=>{
   const on=enabled.includes(name);
   const pos=on?enabled.indexOf(name)+1:0;
   return `<label class="sound-row${on?' on':''}"><input type="checkbox" data-sound="${esc(name)}"${on?' checked':''}>`+
     `<span class="sound-name">${esc(name)}</span>`+
     `<span class="sound-pos">${pos?'#'+pos:'off'}</span>`+
     `<button class="secondary-btn sound-try" data-try="${esc(name)}">Play</button></label>`;
 }).join(''):'<p class="muted">No sounds are installed on this device.</p>';
 return panel('Action button sounds',
   `<p class="muted">Each press plays the next sound that is switched on, then starts again. Switch them all off and the button just flashes the ring.</p>`+
   `<div class="sound-list">${rows}</div>`+
   `<p class="muted" id="ab-count"></p>`,'action-sound-panel');
}
function bindActionSounds(){
 const boxes=()=>$$('#content input[data-sound]');
 const countLine=()=>{
  const n=boxes().filter(x=>x.checked).length;
  const out=$('#ab-count');
  if(out)out.textContent=n?`${n} sound${n===1?'':'s'} in the rotation.`
    :'No sounds selected — the button will flash the ring and stay silent.';
 };
 const positions=()=>{
  let i=0;
  boxes().forEach(x=>{
   const row=x.closest('.sound-row'),tag=row&&row.querySelector('.sound-pos');
   if(row)row.classList.toggle('on',x.checked);
   if(tag)tag.textContent=x.checked?'#'+(++i):'off';
  });
 };
 boxes().forEach(x=>x.onchange=()=>{positions();countLine();const b=$('#save-buttons');if(b)b.disabled=false});
 $$('#content .sound-try').forEach(x=>x.onclick=async e=>{
  /* Preview is deliberately not a generic post(): post() re-renders the
     current page after success, which would replace the form with the last
     saved values and discard unsaved sound choices. */
  e.preventDefault();
  e.stopPropagation();
  if(state.busy)return;
  setBusy(true);
  try{
   await api('/audio/sample',{method:'POST',body:JSON.stringify({name:x.dataset.try})});
   toast('Playing '+x.dataset.try);
  }catch(err){toast(err.message,true)}
  finally{setBusy(false)}
 });
 countLine();
}
function actionSoundSelection(){
 return $$('#content input[data-sound]').filter(x=>x.checked).map(x=>x.dataset.sound).join(',');
}
async function ledPage(){const [l,b]=await Promise.all([api('/led'),api('/buttons')]);const hex='#'+[l.colour.r,l.colour.g,l.colour.b].map(n=>n.toString(16).padStart(2,'0')).join('');const names={listening:'Listening',thinking:'Thinking',error:'Error',dnd:'Do not disturb',night:'Night'};const n=l.night||{enabled:false,active:false,start_minute:1320,end_minute:420};const hhmm=m=>String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');const mins=t=>{const p=String(t||'').split(':');return p.length===2?(+p[0])*60+(+p[1]):NaN};const profiles=Object.entries(l.profiles||{}).map(([k,v])=>{const colour=typeof v==='string'?v:'#'+[v.r,v.g,v.b].map(n=>Number(n).toString(16).padStart(2,'0')).join('');const brightness=typeof v==='object'&&v?Number(v.brightness??l.brightness):l.brightness;return `<div class="led-profile"><label class="field"><span>${names[k]||k}</span><input id="led-profile-${k}" data-profile="${k}" type="color" value="${colour}"></label><div class="swatch-row"><code id="led-profile-hex-${k}">${esc(colour)}</code></div>${range('Brightness',brightness,`led-profile-brightness-${k}`)}<button class="secondary-btn profile-save" data-profile="${k}">Save ${names[k]||k}</button></div>`}).join('');content.innerHTML=`<div class="settings-grid">${panel('Light ring',`<label class="field"><span>Current colour</span><input id="led-colour" type="color" value="${hex}"></label>`+range('Brightness',l.brightness,'led-brightness')+toggle('Music visualizer',l.visualizer_enabled!==false,'led-visualizer')+`<p class="muted">Show reactive equalizer animations on the ring while music is playing.</p>`+ledRing(l)+`<div class="led-preview" data-led="${rgb(l.colour)}"><i></i><span>Live preview</span></div><div class="button-row">${saveButton('save-led')}${action('Run LED test','led-test')}</div>`)}${panel('State themes',`<p class="muted">Choose the ring colour and brightness used while LibreEcho is listening, thinking, reporting an error, or in do-not-disturb mode.</p><div class="led-profiles">${profiles}</div>`)}${panel('Night mode',`<p class="muted">Between these times the ring is capped to the night brightness. Colours are kept, so the device still shows what it is doing -- just dimly. Times are the device's local time.</p>${toggle('Enable night mode',n.enabled,'night-enabled')}<div class="settings-grid">${field('Starts','','night-start','time')}${field('Ends','','night-end','time')}</div><div class="status-line"><span class="status-dot ${n.active?'ok':''}"></span><span>${n.active?'Night mode is active now':'Not active right now'}</span></div>${saveButton('save-night')}`)}${panel('Buttons',select('Short press',b.short_press,'short-action',['Start listening','Play / pause','Run automation','Disabled'])+select('Long press',b.long_press,'long-action',['Open pairing mode','Toggle privacy mode','Reboot device'])+toggle('Hardware mute state',b.hardware_mute,'hw-mute',true)+select('Action button',{sound:'Play a sound',listen:'Start listening',playpause:'Play / pause',disabled:'Do nothing'}[b.action||'sound'],'action-behaviour',['Play a sound','Start listening','Play / pause','Do nothing'])+range('Action flash brightness',b.action_brightness??70,'action-brightness')+range('Mute ring brightness',b.mute_brightness??60,'mute-brightness')+`<p class="muted">Only <strong>Play a sound</strong> is wired up so far; the others are placeholders and will say so in the log if chosen. The flash is on the light ring &mdash; the action button has no lamp of its own. Mute brightness sets the red ring only: the lamp in the mute button is a plain on/off line with no dimming behind it.</p>`+toggle('Press tones',b.tones!==false,'button-tones')+`<p class="muted">A short rising or falling pair when a button is pressed. The buttons are on top of the device where the ring cannot be seen, so the tone is how you know a press registered &mdash; rising for volume up and for leaving mute, falling for the opposite.</p>`+saveButton('save-buttons'),'wide')}${actionSoundPanel(b)}</div>`;bindRange();if($('#night-start')){$('#night-start').value=hhmm(n.start_minute);$('#night-end').value=hhmm(n.end_minute);bindDirty(['#night-enabled','#night-start','#night-end'],'#save-night');$('#save-night').onclick=()=>{const s2=mins($('#night-start').value),e2=mins($('#night-end').value);if(!Number.isFinite(s2)||!Number.isFinite(e2)){toast('Enter both times as HH:MM',true);return}mutate('/led/night',{enabled:$('#night-enabled').checked,start_minute:s2,end_minute:e2},'Night mode saved')}}
bindActionSounds();bindDirty(['#led-colour','#led-brightness','#led-visualizer'],'#save-led');bindDirty(['#short-action','#long-action','#button-tones','#action-behaviour','#action-brightness','#mute-brightness'],'#save-buttons');$('#save-led').onclick=()=>{const h=$('#led-colour').value;mutate('/led',{r:parseInt(h.slice(1,3),16),g:parseInt(h.slice(3,5),16),b:parseInt(h.slice(5,7),16),brightness:+$('#led-brightness').value,visualizer_enabled:$('#led-visualizer').checked},'LED changes saved')};$$('input[type=color][data-profile]').forEach(i=>{const out=$('#led-profile-hex-'+i.dataset.profile);if(out)i.oninput=()=>{out.textContent=i.value}});
$$('.profile-save').forEach(button=>button.onclick=()=>{const k=button.dataset.profile;const h=$(`#led-profile-${k}`).value;const brightnessInput=$(`#led-profile-brightness-${k}`);mutate('/led/profile',{name:k,r:parseInt(h.slice(1,3),16),g:parseInt(h.slice(3,5),16),b:parseInt(h.slice(5,7),16),brightness:+brightnessInput.value},`${names[k]||k} theme saved`)});$('#save-buttons').onclick=()=>mutate('/buttons',{short_press:$('#short-action').value,long_press:$('#long-action').value,tones:$('#button-tones').checked,action:{'Play a sound':'sound','Start listening':'listen','Play / pause':'playpause','Do nothing':'disabled'}[$('#action-behaviour').value]||'sound',action_sounds:actionSoundSelection(),action_brightness:parseInt($('#action-brightness').value,10),mute_brightness:parseInt($('#mute-brightness').value,10)},'Button changes saved');/* The ring test paints at the ring's own brightness, so with brightness at 0
     it runs invisibly and still reports success -- which reads as a dead
     button, and did. Gate it on the saved brightness rather than on the
     slider: the slider is not applied until Save, so enabling the button the
     moment it is dragged would promise a test the device still cannot show. */
  {const testBtn=$('#led-test');
   if(testBtn){
    if(!(l.brightness>0)){
     testBtn.disabled=true;
     testBtn.title='Ring brightness is 0%. The test would run with the ring dark \u2014 raise the brightness, save, then try again.';
    }else testBtn.title='Cycle the ring through red, green, blue and white';
   }}
  $('#led-test').onclick=()=>post('/led/test',{},'LED test started');state.timer=setTimeout(refreshLed,1000)}
/* Board address beside the one in use, with the override that decides it.
   They differ when the driver has generated an address instead of taking the
   board's, which is worth being able to see rather than guess at. */
function macRow(label,id,live,factory,configured){
 const same=live&&factory&&live.toLowerCase()===factory.toLowerCase();
 return `<dl class="facts"><dt>${esc(label)} in use</dt><dd class="mono${same?' connected':''}">${esc(live||'unknown')}</dd>`
  +`<dt>On the board</dt><dd class="mono">${esc(factory||'not recorded')}</dd></dl>`
  +(id==='wifi'?`<p class="muted">This adapter's address cannot be changed: the driver rejects it (<code>SIOCSIFHWADDR: Not supported</code>), and it is not exposed through NVRAM or a module parameter. The address in use is generated by the driver and differs between boots, which is why the device does not keep a stable DHCP lease.</p>${configured?`<button id="clear-wifi-mac" class="secondary-btn">Clear saved Wi-Fi override</button>`:''}`:field(`${label} address override`,configured||'',`mac-${id}`));
}
async function networkPage(){const n=await api('/network'),lanEffective=n.api_lan_effective??n.api_lan,lanForced=!!n.api_lan_forced,healthy=n.connectivity==='healthy',healthDetail=n.recovery_stage&&n.recovery_stage!=='none'?`Recovery: ${n.recovery_stage}`:`Gateway: ${n.gateway_reachable===true?'reachable':n.gateway_reachable===false?'unreachable':'not yet verified'}`;content.innerHTML=`<div class="settings-grid">${panel('Wireless network',`<div class="status-line"><span class="status-dot ${healthy?'ok':''}"></span><div><strong>${esc(n.state)} · ${esc(n.connectivity||'unknown')}</strong><small>${esc(n.ssid||'No network')} · ${esc(healthDetail)}</small></div><span>${n.signal||0}%</span></div><div id="wifi-results" class="wifi-results"><p class="muted">Scan to discover nearby networks.</p></div><div class="button-row">${action('Scan Wi-Fi','wifi-scan','primary-btn')}${action('Disconnect','wifi-disconnect')}</div>`)}${panel('Addressing',field('Hostname',n.hostname,'net-hostname')+toggle('Use DHCP',n.dhcp,'dhcp',true)+`<dl class="facts"><dt>IP address</dt><dd>${esc(n.ip||'—')}</dd><dt>Gateway</dt><dd>${esc(n.gateway||'—')}</dd><dt>DNS</dt><dd>${esc(n.dns||'—')}</dd><dt>Internet</dt><dd class="${n.internet?'connected':''}">${n.internet?'Reachable':'Unavailable'}</dd></dl>`+saveButton('save-network'))}${panel('Wi-Fi address',macRow('Wi-Fi','wifi',n.wifi_mac,n.wifi_mac_factory,n.wifi_mac_configured)+`<p class="muted">Leave a field empty to use the address built into the board. A change is written now and applied on the next restart &mdash; changing the address of a connected interface would drop the connection making the change.</p>`+'')}${panel('Local access',toggle('LAN API access',lanEffective,'api-lan',lanForced)+toggle('SSH',n.ssh,'ssh')+`<p class="muted">${lanForced?'LAN API access is forced by the development image binding. Authentication is intentionally disabled for this development build.':'Enabling LAN access exposes the API beyond loopback. Configure authentication before using this on an untrusted network.'}</p>`+saveButton('save-access'),'wide')}</div>`;bindDirty(['#net-hostname'],'#save-network');bindDirty(['#api-lan','#ssh'],'#save-access');$('#save-network').onclick=()=>mutate('/network',{hostname:$('#net-hostname').value},'Network changes saved');$('#save-access').onclick=()=>mutate('/network',{api_lan:$('#api-lan').checked,ssh:$('#ssh').checked},'Local access changes saved');$('#wifi-scan').onclick=async()=>{const box=$('#wifi-results');box.innerHTML='<p class="muted">Scanning…</p>';try{const s=await api('/network/wifi/scan');box.innerHTML=s.networks.length?s.networks.map(x=>`<button class="wifi-network" data-ssid="${esc(x.ssid)}" data-security="${esc(x.security)}"><span><strong>${esc(x.ssid)}</strong><small>${esc(x.security)}</small></span><span>${x.signal}%</span></button>`).join(''):'<p class="muted">No networks found.</p>';$$('.wifi-network').forEach(b=>b.onclick=()=>connectWifi(b.dataset.ssid,b.dataset.security))}catch(e){box.innerHTML=unsupported(e.message)}};$('#wifi-disconnect').onclick=()=>post('/network/wifi/disconnect',{},'Wi-Fi disconnected');if($('#clear-wifi-mac'))$('#clear-wifi-mac').onclick=()=>mutate('/network',{wifi_mac:''},'Saved Wi-Fi override cleared')}
async function connectWifi(ssid,security){let password='';if(security!=='open'){password=prompt(`Password for ${ssid}`)||'';if(!password)return}await post('/network/wifi/connect',{ssid,password,security},`Connecting to ${ssid}`)}
async function privacyPage(){const p=await api('/privacy');content.innerHTML=`<div class="settings-grid">${panel('Processing',toggle('Local speech recognition only',p.local_only,'local-only')+toggle('Retain microphone audio',p.audio_retention!=='none','audio-retention')+toggle('Diagnostic telemetry',p.diagnostic_telemetry,'telemetry')+toggle('Crash reports',p.crash_reports,'crash-reports')+saveButton('save-privacy'))}${panel('Retention',select('Log retention',String(p.log_retention_hours)+' hours','retention',['24 hours','168 hours','720 hours'])+`<div class="privacy-callout">No cloud dependency is enabled by default.</div><div class="button-row">${saveButton('save-retention')}${action('Reset privacy settings','privacy-reset','danger-btn')}</div>`)}</div>`;bindDirty(['#local-only','#audio-retention','#telemetry','#crash-reports'],'#save-privacy');bindDirty(['#retention'],'#save-retention');$('#save-privacy').onclick=()=>mutate('/privacy',{local_only:$('#local-only').checked,audio_retention:$('#audio-retention').checked?'24h':'none',diagnostic_telemetry:$('#telemetry').checked,crash_reports:$('#crash-reports').checked},'Privacy changes saved');$('#save-retention').onclick=()=>mutate('/privacy',{log_retention_hours:parseInt($('#retention').value,10)},'Retention changes saved');$('#privacy-reset').onclick=()=>confirm('Reset privacy settings?')&&toast('Privacy defaults restored')}
function assistantCard(a){if(a.unsupported)return `<section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${collapsiblePanel('Local LLM',unsupported(a.unsupported),'assistant-provider')}${collapsiblePanel('ChatGPT',unsupported(a.unsupported),'assistant-provider')}</section>`;
/* The Local LLM panel and the ChatGPT panel are two views of one setting:
 * agentd stores a single provider, model, base_url and api_key. Saving from
 * either panel switches the active provider, so each says so plainly. The API
 * key is write-only -- agentd reports api_key_configured and never returns the
 * key itself, so nothing here can render one back. */
const localUrl=String(a.base_url||''),localActive=a.provider==='openai-compatible',keySet=!!a.api_key_configured,local=collapsiblePanel('Local LLM',`<div class="assistant-heading"><div><span class="source-pill">Self-hosted</span><h4>Local LLM</h4><p class="muted">Any OpenAI-compatible endpoint on your network — Ollama, llama.cpp or LM Studio. No subscription and no metered billing. The model runs on that machine; this device is the client.</p></div><div class="assistant-state"><span class="status-dot ${localActive&&localUrl?'ok':''}"></span>${localActive&&localUrl?'Active':localUrl?'Configured':'Not configured'}</div></div><div class="settings-grid assistant-settings"><div>${toggle('Enable wake-to-reply voice loop',localActive&&a.enabled,'local-enabled')}${field('Endpoint URL',localUrl,'local-base-url','text','placeholder="http://198.51.100.10:11434/v1"')}${field('Model',localActive?a.model:'','local-model','text','placeholder="qwen2.5:3b"')}${field(keySet?'API key (stored — type to replace)':'API key (optional)','','local-api-key','password',keySet?'placeholder="Leave blank to keep the stored key"':'placeholder="Leave blank if the endpoint needs no key"')}${saveButton('save-local')}</div><div><dl class="facts"><dt>Active provider</dt><dd class="${localActive?'connected':''}">${localActive?'Local LLM':'ChatGPT'}</dd><dt>Endpoint</dt><dd>${localUrl?esc(localUrl):'Not set'}</dd><dt>API key</dt><dd>${keySet?'Stored':'None'}</dd></dl><p class="muted">Accepts <code>http://</code> on your LAN as well as <code>https://</code>. Saving here makes the local endpoint the active assistant and disconnects ChatGPT.</p><div class="button-row">${localActive&&localUrl?action('Send test prompt','local-test'):''}${keySet?action('Clear key','local-clear-key','danger-btn'):''}</div></div></div>`,'assistant-provider',localActive);const chatgptActive=a.provider==='openai-codex',signedIn=chatgptActive&&a.authenticated,waiting=chatgptActive&&a.auth_state==='waiting',latency=Number(a.last_speech_end_to_first_pcm_ms||0),chatgpt=collapsiblePanel('ChatGPT',`<div class="assistant-heading"><div><span class="source-pill">Subscription</span><h4>${esc(a.provider_name||'ChatGPT')}</h4><p class="muted">Uses your ChatGPT subscription with device login. LibreEcho never asks for or stores an API key, and does not fall back to metered API billing.</p></div><div class="assistant-state"><span class="status-dot ${signedIn?'ok':''}"></span>${signedIn?'Connected':waiting?'Waiting for sign-in':'Not connected'}</div></div>${waiting?`<div class="device-code"><span>Enter this code</span><strong>${esc(a.user_code)}</strong><a class="primary-btn action-link" href="${esc(a.verification_url)}" target="_blank" rel="noopener">Open ChatGPT sign-in</a></div>`:''}<div class="settings-grid assistant-settings"><div>${toggle('Enable wake-to-reply voice loop',chatgptActive&&a.enabled,'assistant-enabled',!signedIn)}${field('Provider',a.provider_name||a.provider,'assistant-provider','text','disabled')}${field('Model',a.model,'assistant-model')}<label class="field"><span>Voice response prompt</span><textarea id="assistant-prompt" rows="8">${esc(a.prompt)}</textarea></label>${saveButton('save-assistant')}</div><div><dl class="facts"><dt>Wake audio</dt><dd class="${a.wake_connected?'connected':''}">${a.wake_connected?'Connected':'Unavailable'}</dd><dt>Post-AEC stream</dt><dd class="${a.audio_connected?'connected':''}">${a.audio_connected?'Connected':'Unavailable'}</dd><dt>Local STT</dt><dd>${a.recognizing?'Recognizing':'Ready'}</dd><dt>Completed voice turns</dt><dd>${Number(a.completed_transcripts||0)}</dd><dt>Last STT processing</dt><dd>${Number(a.last_stt_processing_ms||0)||'—'}${a.last_stt_processing_ms?' ms':''}</dd><dt>Speech end → first PCM</dt><dd class="${latency&&latency<=3000?'connected':''}">${latency?latency+' ms':'Not measured'}</dd><dt>Target</dt><dd>≤ ${Number(a.latency_target_ms||3000)} ms</dd><dt>Target violations</dt><dd>${Number(a.latency_violations||0)}</dd></dl><div class="button-row">${!signedIn&&!waiting?action('Connect ChatGPT','assistant-auth-start','primary-btn'):''}${waiting?action('Check sign-in','assistant-auth-poll','primary-btn'):''}${signedIn?action('Disconnect','assistant-logout','danger-btn'):''}</div>${signedIn?`<label class="field"><span>Test prompt</span><input id="assistant-test-text" value="Say hello in one short sentence."></label>${action('Speak test response','assistant-test')}`:''}</div></div>`,'assistant-provider',waiting);return `<section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${local}${chatgpt}</section>`}
async function assistantAction(path,message){if(state.busy)return;setBusy(true);try{await api(path,{method:'POST',body:'{}'});toast(message);await integrationsPage()}catch(e){toast(e.message,true);await integrationsPage()}finally{setBusy(false)}}
const WX_PROVIDERS=[['open-meteo','Open-Meteo'],['met-no','MET Norway'],['off','Off']];
function wxLabel(id){const m=WX_PROVIDERS.find(p=>p[0]===id);return m?m[1]:'Open-Meteo'}
function wxId(label){const m=WX_PROVIDERS.find(p=>p[1]===label);return m?m[0]:'open-meteo'}
/*
 * Home location.
 *
 * This card used to be titled "Weather" with a field called "Location name",
 * so anyone looking for where their home address goes did not find it: they
 * were hunting for an address and reading a weather-provider setting. The
 * location is not a weather setting -- it feeds weather, local time and, later,
 * directions -- so the title says location first and the field asks for an
 * address. The panel stays expanded; a collapsed panel is another place for it
 * to hide.
 */
function weatherCard(a){if(a.unsupported)return '';
return collapsiblePanel('Home location &amp; weather',`<p class="muted">Where this device is. The assistant uses it for weather, local time and, in future, directions.</p><p class="muted">The place name below is what the assistant says back; the coordinates are what the weather providers actually use. Both providers are free and need no account, and nothing is sent until a location is set.</p><div class="settings-grid">${field('Home address or place','','wx-location','text','placeholder="Austin, Texas"')}${select('Weather provider',wxLabel(a.weather_provider),'wx-provider',WX_PROVIDERS.map(p=>p[1]))}${field('Latitude','','wx-lat','text','placeholder="30.2672"')}${field('Longitude','','wx-lon','text','placeholder="-97.7431"')}</div><div class="button-row">${action('Look up coordinates','wx-lookup')}<span class="muted" id="wx-lookup-note"></span></div><p class="muted" id="wx-warn"></p><div class="settings-grid">${saveButton('save-wx')}</div>`,'weather-provider')}
function bindWeather(a){
 if(a.unsupported||!$('#wx-provider'))return;
 $('#wx-location').value=a.home_location||'';
 $('#wx-lat').value=a.latitude||'';
 $('#wx-lon').value=a.longitude||'';
 bindDirty(['#wx-provider','#wx-location','#wx-lat','#wx-lon'],'#save-wx');
 /*
  * Coordinates are what the weather provider actually queries; the place name
  * is only what the assistant says back. They can therefore disagree
  * silently, and the device will confidently report the wrong town's weather
  * under the right town's name.
  *
  * agentd makes that worse: it applies each field with json_get_string and
  * ignores the return, so an empty value leaves the previous one in place.
  * Clearing a coordinate does nothing at all. Since it cannot be cleared, it
  * has to be replaced -- hence a lookup rather than a "clear and let the
  * device work it out".
  */
 const startLoc=(a.home_location||'').trim();
 const warn=()=>{
  const el=$('#wx-warn'); if(!el)return;
  const loc=$('#wx-location').value.trim();
  const lat=$('#wx-lat').value.trim(), lon=$('#wx-lon').value.trim();
  if(loc&&(!lat||!lon)){
   el.textContent='This place has no coordinates yet. Look them up before saving — an empty coordinate is ignored by the device, so the previous location would stay in use.';
   el.className='muted error-text';
  }else if(loc&&startLoc&&loc!==startLoc&&lat===(a.latitude||'')&&lon===(a.longitude||'')){
   el.textContent='The place changed but the coordinates did not. Weather would still come from the old location.';
   el.className='muted error-text';
  }else{ el.textContent=''; el.className='muted'; }
 };
 ['#wx-location','#wx-lat','#wx-lon'].forEach(sel=>{const el=$(sel);if(el)el.oninput=warn});
 warn();
 $('#wx-lookup').onclick=async()=>{
  const place=$('#wx-location').value.trim();
  const note=$('#wx-lookup-note');
  if(!place){note.textContent='Enter a place first';return}
  note.textContent='Looking up…';
  try{
   /* Queried from this browser rather than the device: the daemon is a
      single bounded poll() loop with no threads, and a blocking lookup
      inside it would stall every other request. Explicit button, so no
      request leaves the browser unless it is asked for. */
   const r=await fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=en&format=json&name='
                       +encodeURIComponent(place),{cache:'no-store'});
   const j=await r.json();
   const hits=j.results||[];
   if(!hits.length){note.textContent='No match for that place';return}
   const h=hits[0];
   $('#wx-lat').value=(+h.latitude).toFixed(4);
   $('#wx-lon').value=(+h.longitude).toFixed(4);
   $('#wx-location').value=[h.name,h.admin1].filter(Boolean).join(', ');
   note.textContent=hits.length>1
     ? `Using ${h.name}, ${h.admin1||''} ${h.country_code||''} — ${hits.length-1} other match(es); edit and look up again if wrong`
     : `Found ${h.name}, ${h.admin1||''} ${h.country_code||''}`;
   $('#save-wx').disabled=false;
   warn();
  }catch(e){ note.textContent='Lookup failed: '+e.message; }
 };
 $('#save-wx').onclick=()=>{
  const loc=$('#wx-location').value.trim();
  const lat=$('#wx-lat').value.trim(), lon=$('#wx-lon').value.trim();
  if(loc&&(!lat||!lon)){toast('Look up the coordinates first — an empty coordinate is ignored by the device',true);return}
  mutate('/assistant',{weather_provider:wxId($('#wx-provider').value),home_location:loc,
                       latitude:lat,longitude:lon},'Home location saved');};}
/*
 * Internet radio stations.
 *
 * The write format is flat and numbered (station_count, station_0_word, ...)
 * rather than an array: the C daemon reads JSON scalars by key and has no
 * array parser, and adding one for client-supplied nested data is exactly the
 * kind of code that turns into a buffer bug. Reads come back as a proper
 * array. The validation below mirrors the daemon's rules so a mistake is
 * pointed at before a request is made -- the server still decides, and its
 * message is what the toast shows when it refuses.
 */
const RADIO_MAX_STATIONS=32;
const RADIO_EXAMPLE={word:'groove',name:'Groove Salad',url:'http://ice1.somafm.com/groovesalad-128-mp3',enabled:true};
/* The word and the URL are trimmed before they are validated or sent, which
   satisfies the daemon's no-leading-or-trailing-space rule without making
   anyone hunt for an invisible character. */
function radioWordError(word,index,words){
 if(!word)return 'Every station needs a word';
 if(word.length>31)return 'The word must be under 32 characters';
 if(!/^[a-z0-9 -]+$/.test(word))return 'Use lowercase letters, digits, spaces and hyphens only';
 if(words.indexOf(word)!==index)return `Another station already uses the word “${word}”`;
 return '';}
function radioUrlError(url){
 if(!url)return 'Every station needs a stream URL';
 if(!/^https?:\/\//.test(url))return 'The stream URL must start with http:// or https://';
 if(url.length>511)return 'The stream URL must be under 512 characters';
 if(!/^[\x21-\x7e]+$/.test(url))return 'The stream URL cannot contain spaces or non-ASCII characters';
 return '';}
function radioNameError(name){return name.length>63?'The name must be under 64 characters':''}
function radioRowHtml(st,i){
 return `<div class="radio-row" data-radio-row>`+
  `<label class="field"><span>Word</span><input class="radio-word" value="${esc(st.word||'')}" placeholder="groove" spellcheck="false" aria-label="Station ${i+1} word"></label>`+
  `<label class="field"><span>Name</span><input class="radio-name" value="${esc(st.name||'')}" placeholder="Groove Salad" aria-label="Station ${i+1} name"></label>`+
  `<label class="field"><span>Stream URL</span><input class="radio-url" value="${esc(st.url||'')}" placeholder="https://" spellcheck="false" aria-label="Station ${i+1} stream URL"></label>`+
  `<label class="switch-row"><span>Enabled</span><input class="toggle-input radio-enabled" type="checkbox" ${st.enabled===false?'':'checked'} aria-label="Station ${i+1} enabled"><span class="switch" aria-hidden="true"></span></label>`+
  `<button class="secondary-btn radio-play" type="button" aria-label="Play station ${i+1}">Play</button>`+
  `<button class="danger-btn radio-remove" type="button" aria-label="Remove station ${i+1}">Remove</button>`+
  `<p class="error-text radio-row-error" hidden></p>`+
  `</div>`;}
function radioPanelBody(r){
 if(r.unsupported)return `<p class="muted">Stations are stored on the device and asked for by word.</p>${unsupported(r.unsupported)}`;
 const max=Number(r.max_stations)||RADIO_MAX_STATIONS;
 const stations=Array.isArray(r.stations)?r.stations:[];
 const seeded=!stations.length;
 return `<p class="muted">Each station has a <strong>word</strong> you say to ask for it — “play groove” — plus a display name and a stream URL. Words must be unique; up to ${max} stations.</p>`+
  (r.playback_supported?'':unsupported('Stations are saved but cannot be played yet — stream playback is not on this image.'))+
  (seeded?`<div class="notice"><strong>Example station</strong><span>The list is empty, so one example is filled in below: SomaFM Groove Salad. Treat the URL as an example to verify rather than a guaranteed-live stream — save it once you have checked it, or replace it with your own.</span></div>`:'')+
  `<div class="radio-list" id="radio-list">${(seeded?[RADIO_EXAMPLE]:stations).map(radioRowHtml).join('')}</div>`+
  `<div class="button-row">${action('Add station','radio-add')}`+
  `${r.playback_supported?action('Stop','radio-stop'):''}<span class="muted" id="radio-count"></span></div>`+
  `<p class="muted" id="radio-now" aria-live="polite"></p>`+
  saveButton('save-radio');}
function bindRadio(r){
 const list=$('#radio-list'),save=$('#save-radio');
 if(!list||!save)return;
 const max=Number(r.max_stations)||RADIO_MAX_STATIONS,stations=Array.isArray(r.stations)?r.stations:[];
 const rows=()=>$$('[data-radio-row]',list);
 const read=row=>({word:$('.radio-word',row).value.trim(),name:$('.radio-name',row).value.trim(),url:$('.radio-url',row).value.trim(),enabled:$('.radio-enabled',row).checked});
 function validate(){
  const current=rows(),values=current.map(read),words=values.map(v=>v.word);
  let first='';
  current.forEach((row,i)=>{
   const v=values[i],message=radioWordError(v.word,i,words)||radioUrlError(v.url)||radioNameError(v.name),box=$('.radio-row-error',row);
   box.textContent=message;box.hidden=!message;row.classList.toggle('invalid',!!message);
   if(message&&!first)first=message;});
  $('#radio-count').textContent=`${current.length} of ${max} stations`;
  return first;}
 /* Rows are added and removed, so the dirty listener goes on the container:
    input events from the fields inside it bubble up to it. */
 bindDirty(['#radio-list'],'#save-radio');
 list.addEventListener('input',validate);
 list.addEventListener('click',e=>{const b=e.target.closest('.radio-remove');if(!b)return;b.closest('[data-radio-row]').remove();save.disabled=false;validate()});
 /*
  * Playback resolves a station by the word the device has stored, not by what
  * is typed in the row -- so an edited or newly added row cannot be played
  * until it is saved, and saying so beats a 404 the user has to interpret.
  */
 const saved=new Set(stations.filter(st=>st.enabled!==false).map(st=>st.word));
 function showNow(data){
  const now=$('#radio-now'),stop=$('#radio-stop');
  if(!now)return;
  const playing=data&&data.playing,url=(data&&data.playing_url)||'';
  const match=stations.find(st=>st.url===url);
  now.textContent=playing?('Playing '+(match?match.name||match.word:url)):'';
  if(stop)stop.hidden=!playing;}
 if(r.playback_supported)showNow(r);
 list.addEventListener('click',async e=>{
  const b=e.target.closest('.radio-play');
  if(!b)return;
  const row=b.closest('[data-radio-row]'),v=read(row);
  if(!saved.has(v.word)){toast(v.word&&stations.some(st=>st.word===v.word)?'That station is switched off':'Save the station before playing it',true);return}
  if(state.busy)return;
  setBusy(true);
  try{showNow(await api('/integrations/radio/play',{method:'POST',body:JSON.stringify({word:v.word})}))}
  catch(err){toast(err.message,true)}
  finally{setBusy(false)}});
 const stopButton=$('#radio-stop');
 if(stopButton)stopButton.onclick=async()=>{
  if(state.busy)return;
  setBusy(true);
  try{showNow(await api('/integrations/radio/stop',{method:'POST',body:'{}'}))}
  catch(err){toast(err.message,true)}
  finally{setBusy(false)}};
 $('#radio-add').onclick=()=>{
  if(rows().length>=max){toast(`At most ${max} stations`,true);return}
  list.insertAdjacentHTML('beforeend',radioRowHtml({enabled:true},rows().length));
  save.disabled=false;validate();
  const added=rows().pop();if(added)$('.radio-word',added).focus();};
 save.onclick=async()=>{
  const problem=validate();
  if(problem){toast(problem,true);return}
  const values=rows().map(read),body={station_count:values.length};
  values.forEach((v,i)=>{body['station_'+i+'_word']=v.word;body['station_'+i+'_name']=v.name;body['station_'+i+'_url']=v.url;body['station_'+i+'_enabled']=v.enabled});
  if(state.busy)return;
  setBusy(true);
  try{await api('/integrations/radio',{method:'PUT',body:JSON.stringify(body)});toast(values.length===1?'1 station saved':values.length+' stations saved');await integrationsPage()}
  catch(e){toast(e.message,true)}
  finally{setBusy(false)}};
 /* The seeded example is not on the device yet, so it is unsaved work and the
    save button starts enabled; a list that came from the device is not. */
 if(!stations.length)save.disabled=false;
 validate();}
async function radioPanel(host){
 if($('.radio-stations',host))return;
 const r=await api('/integrations/radio').catch(e=>({unsupported:e.message}));
 if(!host.isConnected)return;
 host.insertAdjacentHTML('beforeend',collapsiblePanel('Internet radio',radioPanelBody(r),'radio-stations wide'));
 bindRadio(r);}
/*
 * integrations-ui.js renders the live Integrations page and never draws the
 * home-location card, so on the shipped UI there is nowhere to set a home
 * address at all. Add it here, high on the page, when the renderer that ran
 * did not already produce one.
 */
async function homeLocationPanel(host){
 if($('.weather-provider',host))return;
 const a=await api('/assistant').catch(e=>({unsupported:e.message}));
 const card=weatherCard(a);
 if(!card||!host.isConnected)return;
 const anchor=$('.voice-assistants',host);
 if(anchor)anchor.insertAdjacentHTML('afterend',card);else host.insertAdjacentHTML('afterbegin',card);
 bindWeather(a);}
/*
 * Both panels fetch before they insert, and a re-render can start during that
 * fetch -- the sign-in poll on this page does exactly that. Two invocations
 * would then both see no panel and both append one. Claim the grid
 * synchronously, before the first await, and drop the result if the grid it
 * was meant for has since been replaced.
 */
async function integrationsExtras(){
 const grid=$('.integration-grid',content),host=grid||content;
 if(!host)return;
 if(grid){if(grid.dataset.extras)return;grid.dataset.extras='1'}
 await homeLocationPanel(host);
 await radioPanel(host);}
/*
 * The Integrations page is rendered by integrations-ui.js, which loads after
 * this file and replaces the integrationsPage defined below. Its own handlers
 * re-render by calling integrationsPage() again, so appending these panels
 * once from render() would lose them on the next save. Wrap whichever
 * definition won instead, on first use: the inner re-renders resolve the same
 * global and so come back through the wrapper.
 */
function installIntegrationsExtras(){
 if(integrationsPage.extrasWrapped)return;
 const inner=integrationsPage,wrapped=async function(){const out=await inner.apply(this,arguments);await integrationsExtras();return out};
 wrapped.extrasWrapped=true;
 globalThis.integrationsPage=wrapped;}
/*
 * Per-integration copy. Spotify Connect needs its own line because the thing
 * that usually goes wrong is not the toggle: it needs a Premium account, and
 * the device only appears in the Spotify app once the daemon is running and on
 * the same network.
 *
 * Tidal is named here deliberately. It has no open Connect protocol -- it is
 * licensed to partner hardware and there is no public SDK -- so there is no
 * honest toggle to offer. Saying that plainly, next to the two transports that
 * do work for it, is more useful than a switch that could never function.
 */
function integrationBlurb(x){
 if(x.id==='rest')return 'Versioned local device API.';
 if(x.id==='spotify')return 'Appear as a speaker in the Spotify app. Requires a Spotify Premium account; playback is handled on the device and audio never leaves your network.';
 if(x.id==='airplay2')return 'Appear as an AirPlay 2 speaker. This is also how Tidal, Apple Music and anything else without an open protocol can play here \u2014 cast from the app.';
 if(x.id==='bluetooth')return 'Pair a phone or laptop and play to this device directly. A second route for apps with no open casting protocol, including Tidal.';
 return 'Optional local integration; no cloud connection required.';
}
function integrationStatus(x){
 if(x.installed===false)return 'Not installed in this image';
 return x.enabled?'Enabled':'Not configured';
}
async function integrationsPage(){const [d,a]=await Promise.all([api('/integrations'),api('/assistant').catch(e=>({unsupported:e.message}))]);content.innerHTML=`<div class="integration-grid">${assistantCard(a)}${weatherCard(a)}${d.items.map(x=>collapsiblePanel(x.name,`<p class="muted">${integrationBlurb(x)}</p>${x.installed===false?'':toggle('Enabled',x.enabled,'int-'+x.id)}<div class="status-line"><span class="status-dot ${x.enabled?'ok':''}"></span><span>${integrationStatus(x)}</span></div>${x.installed===false?'':saveButton('save-int-'+x.id)}`)).join('')}</div>`;d.items.forEach(x=>{if(x.installed===false)return;bindDirty(['#int-'+x.id],'#save-int-'+x.id);$('#save-int-'+x.id).onclick=()=>mutate('/integrations/'+x.id,{enabled:$('#int-'+x.id).checked},x.name+' changes saved')});bindWeather(a);
if(!a.unsupported){bindDirty(['#assistant-enabled','#assistant-model','#assistant-prompt'],'#save-assistant');$('#save-assistant').onclick=()=>mutate('/assistant',{enabled:$('#assistant-enabled').checked,provider:'openai-codex',model:$('#assistant-model').value.trim(),prompt:$('#assistant-prompt').value.trim()},'Voice assistant settings saved');
bindDirty(['#local-enabled','#local-base-url','#local-model','#local-api-key'],'#save-local');
/* Blank key means "keep whatever is stored": api_key is omitted from the body
 * entirely rather than sent empty, which agentd would read as a clear. */
if($('#save-local'))$('#save-local').onclick=()=>{const url=$('#local-base-url').value.trim(),model=$('#local-model').value.trim(),key=$('#local-api-key').value;if(!/^https?:\/\//.test(url))return toast('Endpoint URL must start with http:// or https://',true);if(!model)return toast('Enter the model name that endpoint serves',true);const body={enabled:$('#local-enabled').checked,provider:'openai-compatible',base_url:url,model:model};if(key)body.api_key=key;return mutate('/assistant',body,'Local LLM settings saved')};
if($('#local-test'))$('#local-test').onclick=()=>post('/assistant/respond',{text:'Say hello in one short sentence.'},'Test response queued');
if($('#local-clear-key'))$('#local-clear-key').onclick=()=>mutate('/assistant',{api_key:''},'API key cleared');if($('#assistant-auth-start'))$('#assistant-auth-start').onclick=()=>assistantAction('/assistant/auth/start','ChatGPT device sign-in started');if($('#assistant-auth-poll'))$('#assistant-auth-poll').onclick=()=>assistantAction('/assistant/auth/poll','Sign-in status checked');if($('#assistant-logout'))$('#assistant-logout').onclick=()=>assistantAction('/assistant/logout','ChatGPT disconnected');if($('#assistant-test'))$('#assistant-test').onclick=()=>post('/assistant/respond',{text:$('#assistant-test-text').value},'Test response queued');if(a.auth_state==='waiting')state.timer=setTimeout(async()=>{if(state.page!=='Integrations')return;try{await api('/assistant/auth/poll',{method:'POST',body:'{}'});await integrationsPage()}catch(e){toast(e.message,true)}},3000)}}
async function copyTextCompat(text){
  if(navigator.clipboard&&window.isSecureContext){await navigator.clipboard.writeText(text);return;}
  const area=document.createElement('textarea');area.value=text;area.setAttribute('readonly','');area.style.position='fixed';area.style.opacity='0';document.body.appendChild(area);area.select();
  if(!document.execCommand('copy'))throw new Error('Clipboard access is unavailable');
  document.body.removeChild(area);
}
/*
 * The upload cap is the daemon's, read from max_upload_bytes rather than
 * repeated here: a hard-coded "32 MiB" would go quietly wrong the day the
 * daemon's limit moved. The size shown beside it is the file currently
 * selected for upload -- the device does not report the size of the image
 * already installed in a slot, so that is not what this row claims.
 */
function updateSizeText(file,ota){
 const cap=Number(ota&&ota.max_upload_bytes)||0;
 const ceiling=Number(ota&&ota.max_upload_ceiling_bytes)||0;
 /* When the two differ it is free space doing the limiting, not the ceiling,
    and saying which one is the difference between "this build is too big" and
    "this device needs clearing out first". */
 const why=cap&&ceiling&&cap<ceiling?' — free space on the device, under the '+mib(ceiling)+' ceiling':'';
 const limit=cap?' of '+mib(cap)+' limit'+why:'';
 return (file?mib(file.size):'No file selected')+limit;}
function updateSizeShow(file,ota){
 const el=$('#update-size'); if(!el)return;
 const cap=Number(ota&&ota.max_upload_bytes)||0;
 el.textContent=updateSizeText(file,ota);
 el.className=file&&cap&&file.size>cap?'error-text':'';}
async function systemPage(){
  const [s,d,ota,features]=await Promise.all([api('/system'),api('/device'),api('/system/update'),api('/system/features').catch(e=>({unsupported:e.message}))]);
  updateVersionDisplay(d,ota);
  const syncTime=s.last_sync_epoch?new Date(s.last_sync_epoch*1000).toLocaleString():'Never';
  const checkTime=ota.last_check_epoch?new Date(ota.last_check_epoch*1000).toLocaleString():'Not checked';
  const successTime=ota.last_success_epoch?new Date(ota.last_success_epoch*1000).toLocaleString():'Never';
  const checkLabels={'not-checked':'Not checked',checking:'Checking',downloading:'Downloading','up-to-date':'Up to date','update-available':'Update available','update-held-after-rollback':'Held after rollback','reboot-pending':'Ready to restart',error:'Check failed'};
  const errorLabels={asset_missing:'Asset missing',asset_access_denied:'Asset access denied',download_transport:'Network error',downloaded_package_invalid:'Invalid signature or package',install_failed:'Installation failed'};
  const checkResult=ota.check_status==='error'?(errorLabels[ota.check_error]||ota.check_error||'Check failed'):(checkLabels[ota.check_status]||ota.check_status);
  const sourceStatus=ota.source_reachable==='true'?'Reachable':ota.source_reachable==='false'?'Unreachable':'Not checked';
  const sourceName=ota.source==='github-releases'?'GitHub Releases':ota.source||'Not configured';
  const installedVersion=ota.installed_version||d.os_version;
  const updateControls=ota.supported?`<input id="update-file" type="file" accept="application/x-tar,.tar" hidden>${select('Update channel',ota.channel||'stable','update-channel',['stable','dev'])}${toggle('Install new updates automatically',ota.automatic_updates,'automatic-updates')}<p class="muted">Automatic installation is disabled by default and never restarts the device.</p>${saveButton('save-update-settings')}${ota.allow_unsigned?toggle('Allow unsigned image (side-load a locally-built package)',false,'allow-unsigned')+`<p class="muted">Only applies to a manually selected update file; fetched updates always verify signatures.</p>`:''}<div class="button-row">${!ota.pending_reboot?action('Check for updates','check-update'):''}${ota.check_status==='update-available'&&!ota.pending_reboot?action('Install update','install-update','primary-btn'):''}${action('Select update file','select-update')}${ota.pending_reboot?action('Restart into update','restart-update','primary-btn'):''}</div><p id="update-name" class="muted">${ota.pending_version?'Version '+esc(ota.pending_version)+' is ready to boot.':ota.check_status==='update-available'?'Version '+esc(ota.latest_version)+' is available from GitHub Releases.':'No update selected.'}</p>`:unsupported('Signed updates are unavailable on this development image.');
  const featuresPanel=panel('Features',features.unsupported?unsupported(features.unsupported):
    toggle('Simulation',!!features.simulation,'feature-simulation')+
    `<p class="muted">Simulation renders a phrase with the device's own text-to-speech and plays it into the <strong>microphone</strong> path, so wake word detection, speech-to-text and the assistant handle it exactly as though it had been spoken in the room. Nothing is recorded and nothing leaves the device.</p><p class="muted">It is off by default and is meant for testing. Switching it on adds the Simulation page to the menu; switching it off hides that page again and the device refuses simulated audio.</p>`+
    toggle('HTTPS',!!features.https,'feature-https')+
    `<p class="muted">Serves the interface over TLS on port ${features.https_port||8443} using a certificate the device generates for itself and keeps in its own configuration folder. Plain HTTP stays available on the usual port, so a certificate problem can never lock you out.</p>`+
    (features.https?(features.https_active?
      `<dl class="facts"><dt>Status</dt><dd class="connected">Serving on port ${features.https_port}</dd><dt>Certificate expires</dt><dd>${esc(features.https_expires||'unknown')}</dd><dt>SHA-256 fingerprint</dt><dd class="mono wrap">${esc(features.https_fingerprint||'unknown')}</dd></dl>
       <p class="muted">Because the certificate is self-signed, browsers will warn on first visit. Check the fingerprint above matches the one the browser shows before accepting it &mdash; that is what confirms you are talking to this device and not something in between.</p>`:
      `<p class="muted">Switched on, but not yet serving: HTTPS binds its port when the daemon starts, so this takes effect after the next restart.</p>`):'')+
    (features.usb_role_supported?toggle('USB storage mode (experimental)',!!features.usb_host,'feature-usb-host')+
    `<p class="muted"><strong>Experimental, but it no longer costs the microphone.</strong> The pin that puts the port in host mode is also the enable line for the microphone array, and an earlier kernel drove it low, which left the codecs disabled. This firmware never drives it low, so a mounted drive and the wake word work at the same time &mdash; measured on hardware, with a full spoken turn while a drive was mounted.</p><p class="muted">Host mode reads a drive; device mode serves ADB, the only way in if the network drops. The port cannot do both.</p>`+
    `<p class="muted">The setting is remembered, but device mode is held for the first minute after every boot so ADB is always reachable; storage mode resumes after that. Applies immediately &mdash; the Save button below is for Simulation only.</p>`+
    `<p class="muted">A drive already plugged in when storage mode turns on cannot be woken: it is already powered and only re-announces itself on a power cycle, which this board cannot perform. Turn storage mode on first, then insert the drive.</p>`+
    `<div id="usb-storage" class="usb-storage"></div>`:'')+
    saveButton('save-features'));
  content.innerHTML=`<div class="settings-grid">${panel('Software',`<dl class="facts"><dt>OS version</dt><dd>${esc(d.os_version)}</dd><dt>Kernel</dt><dd>${esc(d.kernel)}</dd><dt>Time zone</dt><dd>${esc(s.timezone)}</dd><dt>NTP</dt><dd class="${s.ntp?'connected':''}">${s.ntp?'Synchronized':esc(s.ntp_state||'Unavailable')}</dd><dt>Last synchronization</dt><dd>${esc(syncTime)}</dd><dt>Clock source</dt><dd>${esc(s.clock_source||'unknown')}</dd><dt>RTC</dt><dd class="${s.rtc_available?'connected':''}">${s.rtc_available?(s.rtc_persisted?'Available · synchronized':'Available'):'Unavailable'}</dd><dt>Time servers</dt><dd>${esc(s.ntp_servers||'Not configured')}</dd></dl>`)}${panel('System update',`<dl class="facts"><dt>Installed version</dt><dd>${esc(installedVersion)}</dd><dt>Latest version</dt><dd>${esc(ota.latest_version||'Unknown')}</dd><dt>Channel</dt><dd>${esc(ota.channel||s.update_channel)}</dd><dt>Source</dt><dd>${esc(sourceName)}</dd><dt>Source status</dt><dd class="${ota.source_reachable==='true'?'connected':''}">${esc(sourceStatus)}</dd><dt>Check result</dt><dd>${esc(checkResult)}</dd><dt>Last checked</dt><dd>${esc(checkTime)}</dd><dt>Last successful check</dt><dd>${esc(successTime)}</dd><dt>Automatic updates</dt><dd>${ota.automatic_updates?'Enabled':'Off'}</dd><dt>Current slot</dt><dd>${esc(ota.current_slot.toUpperCase())}</dd><dt>Inactive slot</dt><dd>${esc(ota.inactive_slot.toUpperCase())}</dd><dt>Install state</dt><dd>${esc(ota.state)}</dd><dt>Update image</dt><dd id="update-size">${updateSizeText(null,ota)}</dd><dt>Rollback</dt><dd>${ota.rollback_available?'Available':'Unavailable'}</dd>${ota.rollback_version?`<dt>Last rollback</dt><dd>${esc(ota.rollback_version)}</dd>`:''}</dl><progress class="progress" max="100" value="${ota.progress}" aria-label="Update progress"></progress>${updateControls}`)}${featuresPanel}${panel('Configuration and diagnostics',`<input id="restore-file" type="file" accept="application/json,.json" hidden><div class="button-row">${action('Download configuration','backup','primary-btn')}${action('Restore configuration','restore')}${action('Download diagnostic bundle','export-diag','primary-btn')}${action('Copy health summary','copy-diag')}</div><p class="muted">Configuration exports are versioned JSON and exclude Wi-Fi passwords, authentication tokens, logs and live telemetry. Diagnostic bundles are bounded JSON with release identity and health evidence; SSIDs, addresses, owner identifiers, tokens, private paths, media metadata and signing material are omitted or redacted.</p>`,'wide')}</div>`;
  if($('#save-update-settings')){bindDirty(['#automatic-updates','#update-channel'],'#save-update-settings');$('#save-update-settings').onclick=async()=>{const channel=$('#update-channel').value,automatic=$('#automatic-updates').checked;if(state.busy)return;setBusy(true);try{if(channel!==(ota.channel||'stable'))await api('/system/update/channel',{method:'PUT',body:JSON.stringify({channel})});if(automatic!==ota.automatic_updates)await api('/system/update/automatic',{method:'PUT',body:JSON.stringify({enabled:automatic})});toast('Update settings saved');await render()}catch(e){toast(e.message,true);await render()}finally{setBusy(false)}}}
  if($('#save-features')){bindDirty(['#feature-simulation','#feature-https'],'#save-features');$('#save-features').onclick=async()=>{if(state.busy)return;setBusy(true);try{const body={simulation:$('#feature-simulation').checked};const h=$('#feature-https');const hadHttps=!!state.features.https;if(h)body.https=h.checked;const saved=await api('/system/features',{method:'PUT',body:JSON.stringify(body)});applyFeatures(saved);toast(h&&h.checked!==hadHttps?(h.checked?'HTTPS enabled \u2014 restart to start serving it':'HTTPS disabled \u2014 takes effect after restart'):(saved.simulation?'Simulation enabled':'Simulation disabled'));await render()}catch(e){toast(e.message,true);await render()}finally{setBusy(false)}}}
  /* Immediate, and deliberately outside the Save flow: the role is a live
     hardware switch the daemon applies now, not a stored preference. */
  /* Turning host mode on costs ADB and now survives reboots, so a stray click
     would leave the device unreachable over USB on every boot until it is
     turned off again. Confirm that, and say how to undo it without the UI. */
  if($('#feature-usb-host'))$('#feature-usb-host').onchange=()=>{
   const el=$('#feature-usb-host');
   if(el.checked&&!confirm('Switch the USB port to storage mode? (experimental)\n\n'
     +'The microphone keeps working: this firmware never drives the shared pin low, '
     +'so a mounted drive and the wake word coexist.\n\n'
     +'ADB does stop, because the port cannot read a drive and serve ADB at once. '
     +'This setting is remembered, so it applies on every boot after the first minute.\n\n'
     +'To undo it without the UI, hold any button on the device while it boots.')){
    el.checked=false;return}
   mutate('/system/features',{usb_host:el.checked},el.checked?'USB port switched to host mode':'USB port switched to device mode');};
  /*
   * The drive panel. Rendered only while host mode is on, because in device
   * mode there is nothing to read and saying "no disk" would suggest a fault
   * rather than the port simply doing its other job.
   */
  async function usbStorageRender(rel){
   const host=$('#usb-storage');
   if(!host)return;
   if(!$('#feature-usb-host')||!$('#feature-usb-host').checked){host.innerHTML='';return}
   host.innerHTML='<p class="muted">Looking for a drive…</p>';
   let d;
   try{ d=await api('/storage/usb'+(rel?'?path='+encodeURIComponent(rel):'')) }
   catch(e){ host.innerHTML=`<p class="error-text">${esc(e.message)}</p>`; return }
   if(!host.isConnected)return;
   if(!d.present){host.innerHTML=`<p class="muted">${esc(d.message||'No drive detected.')}</p>`;return}
   if(!d.mounted){
    host.innerHTML=`<div class="status-line"><span class="status-dot warn"></span><span>${esc(d.device||'disk')} found, but no filesystem could be mounted.</span></div>`;
    return}
   const total=Number(d.size_bytes)||0,used=Number(d.used_bytes)||0;
   const pct=total?Math.min(100,Math.round(used/total*100)):0;
   const crumbs=[];const parts=(d.rel_path||'').split('/').filter(Boolean);
   crumbs.push(`<button class="link-btn usb-crumb" data-rel="">Drive</button>`);
   parts.forEach((seg,i)=>crumbs.push(`<span class="usb-sep">/</span><button class="link-btn usb-crumb" data-rel="${esc(parts.slice(0,i+1).join('/'))}">${esc(seg)}</button>`));
   /* radiod decodes Layer III only, so a Play button appears for .mp3 and
      anything else audio-looking says why rather than failing silently. */
   const AUDIO=/\.(mp3|aac|m4a|flac|wav|ogg|opus|wma)$/i;
   const rows=(d.entries||[]).slice().sort((a,b)=>(b.directory-a.directory)||a.name.localeCompare(b.name)).map(e=>{
    const sz=e.directory?'':bytes(e.size_bytes);
    const rel=(d.rel_path?d.rel_path+'/':'')+e.name;
    const nm=e.directory
      ? `<button class="link-btn usb-open" data-rel="${esc(rel)}">${esc(e.name)}/</button>`
      : esc(e.name);
    let act='';
    if(!e.directory&&/\.mp3$/i.test(e.name))act=`<button class="secondary-btn usb-play" data-rel="${esc(rel)}">Play</button>`;
    else if(!e.directory&&AUDIO.test(e.name))act=`<span class="muted" title="This image decodes MP3 only">not playable</span>`;
    return `<tr><td>${nm}</td><td class="num">${sz}</td><td class="usb-act">${act}</td></tr>`}).join('');
   /* Keep the open/closed state across re-renders: browsing into a folder
      re-renders, and a panel that snapped shut each time would be unusable. */
   const wasOpen=host.dataset.open==='1'||!host.dataset.open;
   host.innerHTML=
    `<details class="panel setting-panel usb-drive"${wasOpen?' open':''}>`+
    `<summary><h3>${esc(d.device||'Drive')} · ${bytes(total)}</h3>`+
    `<span class="integration-toggle" aria-hidden="true">Show details</span></summary>`+
    `<div class="integration-section-body">`+
    `<dl class="facts"><dt>Device</dt><dd>${esc(d.device||'')} · ${esc(d.partition||'')} · ${esc(d.filesystem||'')}</dd>`+
    `<dt>Capacity</dt><dd>${bytes(total)} total · ${bytes(used)} used · ${bytes(d.free_bytes)} free</dd></dl>`+
    `<div class="usb-bar"><span style="width:${pct}%"></span></div>`+
    `<p class="muted">Playable format: <strong>MP3</strong>. Other audio files are listed but cannot be decoded on this image yet.</p>`+
    `<div class="usb-path">${crumbs.join('')}</div>`+
    (rows?`<table class="usb-list"><thead><tr><th>Name</th><th class="num">Size</th><th></th></tr></thead><tbody>${rows}</tbody></table>`
         :'<p class="muted">This folder is empty.</p>')+
    `</div></details>`;
   const det=$('details',host);
   if(det)det.ontoggle=()=>{host.dataset.open=det.open?'1':'0'};
   $$('.usb-open,.usb-crumb',host).forEach(b=>b.onclick=()=>usbStorageRender(b.dataset.rel));
   $$('.usb-play',host).forEach(b=>b.onclick=async()=>{
    if(state.busy)return;
    setBusy(true);
    try{ await api('/storage/usb/play',{method:'POST',body:JSON.stringify({path:b.dataset.rel})});
         toast('Playing '+b.dataset.rel.split('/').pop()); }
    catch(err){ toast(err.message,true) }
    finally{ setBusy(false) }});
  }
  if($('#feature-usb-host')){
   const prev=$('#feature-usb-host').onchange;
   $('#feature-usb-host').onchange=e=>{prev(e);setTimeout(()=>usbStorageRender(''),1200)};
   usbStorageRender('');
  }
  if($('#check-update'))$('#check-update').onclick=()=>post('/system/update/check',{},'Update check completed');
  if($('#install-update'))$('#install-update').onclick=()=>confirm(`Install signed update ${ota.latest_version} to slot ${ota.inactive_slot.toUpperCase()}?`)&&post('/system/update/apply',{},'Update verified and installed');
  if($('#select-update'))$('#select-update').onclick=()=>$('#update-file').click();
  if($('#update-file'))$('#update-file').onchange=async e=>{
    const file=e.target.files[0];
    if(!file)return;
    $('#update-name').textContent=file.name;
    updateSizeShow(file,ota);
    if(ota.max_upload_bytes&&file.size>ota.max_upload_bytes){
      toast('Update is '+mib(file.size)+', over the '+mib(ota.max_upload_bytes)+' limit',true);
      e.target.value='';return}
    const unsigned=$('#allow-unsigned')?.checked;
    if(!confirm(`${unsigned?'Install unsigned update':'Install signed update'} ${file.name} to slot ${ota.inactive_slot.toUpperCase()}?${unsigned?' This bypasses signature verification.':''}`)){e.target.value='';return}
    setBusy(true);
    try{
      const headers={'Content-Type':'application/x-tar','X-LibreEcho-CSRF':state.csrf,...(state.token?{'Authorization':'Bearer '+state.token}:{}),...(unsigned?{'X-LibreEcho-Allow-Unsigned':'1'}:{})};
      const response=await fetch('/api/v1/system/update/upload',{method:'POST',headers,body:file});
      const body=await response.json();
      if(!response.ok||!body.ok){const detail=body.error?.reason?`${body.error.message||'Update installation failed'} (reason: ${body.error.reason})`:body.error?.message||'Update installation failed';throw new Error(detail);}
      toast('Update verified and installed');
      await systemPage();
    }catch(error){toast(error.message,true)}
    finally{setBusy(false);e.target.value=''}
  };
  if($('#restart-update'))$('#restart-update').onclick=()=>power('reboot','Restart into the update');
  $('#backup').onclick=async()=>{try{const config=await api('/config/export'),blob=new Blob([JSON.stringify(config,null,2)+'\n'],{type:'application/json'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='libreecho-config.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);toast(config.partial?'Partial configuration downloaded; unsupported fields were omitted':'Configuration downloaded')}catch(e){toast(e.message,true)}};
  $('#restore').onclick=()=>$('#restore-file').click();
  $('#restore-file').onchange=async e=>{const file=e.target.files[0];if(!file)return;try{const config=JSON.parse(await file.text());if(!confirm('Restore this configuration? Current settings will be replaced.'))return;await post('/config/import',config,'Configuration restored');showPage('System')}catch(error){toast(error.message||'Invalid configuration file',true)}finally{e.target.value=''}};
  $('#export-diag').onclick=async()=>{try{const bundle=await api('/diagnostics/export',{method:'POST',body:'{}'});state.data.diagnosticBundle=bundle;const blob=new Blob([JSON.stringify(bundle,null,2)+'\n'],{type:'application/json'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='libreecho-diagnostic-bundle.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);toast('Redacted diagnostic bundle downloaded')}catch(e){toast(e.message,true)}};
  $('#copy-diag').onclick=async()=>{try{const bundle=state.data.diagnosticBundle||await api('/diagnostics/export',{method:'POST',body:'{}'});state.data.diagnosticBundle=bundle;await copyTextCompat(bundle.summary||'LibreEcho diagnostic summary unavailable');toast('Health summary copied')}catch(e){toast(e.message,true)}};
}
/*
 * A check is not simply ok or broken. The daemon reports "degraded" when an
 * adapter is running but unhealthy, "unavailable" when the hardware is absent
 * from this image, and "development" on the mock backend where the question
 * does not apply. Only "ok" ever set a class, so the other three rendered
 * identically in the default red -- which made a mock backend look like a
 * failing device. Grade them, and repeat the worst grade on the heading so the
 * panel reads at a glance instead of row by row.
 */
const DIAGNOSTIC_GRADES={ok:'ok',development:'idle',degraded:'warn',unavailable:'warn'};
function diagnosticGrade(status){const g=DIAGNOSTIC_GRADES[status];return g===undefined?'bad':g}
function diagnosticsPanel(d){
 const checks=Array.isArray(d&&d.checks)?d.checks:[];
 const count=g=>checks.filter(x=>diagnosticGrade(x.status)===g).length;
 const bad=count('bad'),warn=count('warn');
 const grade=bad?'bad':warn?'warn':checks.length?'ok':'idle';
 const summary=bad?`${bad} failing`:warn?`${warn} needs attention`:checks.length?'all healthy':'no checks reported';
 return panel(`<span class="status-dot ${grade}"></span>Diagnostic checks<small class="diag-summary">${esc(summary)}</small>`,
  checks.map(x=>`<div class="status-line"><span class="status-dot ${diagnosticGrade(x.status)}"></span><span>${esc(x.name)}</span><strong>${esc(x.status)}</strong></div>`).join('')
  ||'<p class="muted">The device reported no diagnostic checks.</p>');
}
async function logsPage(){const [l,d]=await Promise.all([api('/logs'),api('/diagnostics')]);const clock=l.entries.some(x=>Number(x.timestamp)<1577836800)?'Device clock unset; relative boot time is shown where available':'Device clock available';content.innerHTML=`${panel('Service logs',`<div class="log-toolbar"><input id="log-filter" placeholder="Filter logs" aria-label="Filter logs">${action('Refresh','log-refresh')}${action('Download kernel log','kernel-log-download')}</div><div class="log-viewer" id="log-viewer">${l.entries.length?l.entries.map(x=>`<div data-log="${esc(x.message.toLowerCase())}"><time>${logTime(x.timestamp,x.boot_seconds)}</time><span class="level ${x.level}">${x.level}</span><span>${esc(x.message)}</span></div>`).join(''):'<p class="muted">No logs available.</p>'}</div><p class="muted">Source: ${esc(l.source||'web-memory')}; bounded to ${l.capacity} entries. ${clock}. Relative time is from boot; secrets are redacted before logging.</p>`)}<div class="settings-grid diagnostics">${diagnosticsPanel(d)}${panel('Mock fault injection',state.data.status?.simulated?`<p class="muted">Use <code>tools/mockctl.sh</code> with <code>--dev-controls</code> to inject deterministic faults.</p>${action('Fail next Wi-Fi scan','fail-wifi','danger-btn')}`:unsupported('Fault injection is only available in mock development builds.'))}</div>`;$('#log-filter').oninput=e=>$$('[data-log]').forEach(x=>x.hidden=!x.dataset.log.includes(e.target.value.toLowerCase()));$('#log-refresh').onclick=render;$('#kernel-log-download').onclick=async()=>{try{const res=await fetch('/api/v1/diagnostics/kernel.log',{headers:{...(state.token?{'Authorization':'Bearer '+state.token}:{})}});if(!res.ok)throw new Error('Kernel log unavailable ('+res.status+')');const text=await res.text();const blob=new Blob([text],{type:'text/plain'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download='libreecho-kernel-'+new Date().toISOString().replace(/[:.]/g,'-')+'.log';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);toast('Kernel log downloaded ('+(text?text.split(String.fromCharCode(10)).length-1:0)+' lines)')}catch(e){toast(e.message,true)}};if($('#fail-wifi'))$('#fail-wifi').onclick=()=>post('/dev/mock',{action:'fail-next',value:'wifi-scan'},'Next Wi-Fi scan will fail')}
function aboutPage(){content.innerHTML=`<div class="settings-grid">${panel('LibreEcho',`<img class="about-mark" src="/assets/mark.svg" alt="LibreEcho mark"><p>Open source voice-assistant software built for privacy, repairability and local control.</p><dl class="facts"><dt>Web API</dt><dd>v1</dd><dt>Frontend</dt><dd>Dependency-free HTML, CSS and JavaScript</dd><dt>Daemon</dt><dd>Portable C99</dd><dt>Licence</dt><dd>MIT</dd></dl>`)}${panel('Hardware independence',`<p class="muted">The same frontend API works with both the realistic mock backend and the conservative Linux hardware adapter.</p><div class="privacy-callout">Open. Private. Yours.</div>`)}${panel('Supported devices',`<p class="muted">Confirmed models have been booted and used with LibreEcho on real hardware. Likely models share the MT8163 platform and the Amonet exploit path, but have not been tried — treat them as untested.</p>`+specTable(['Device','Codename','SoC','Status'],[ ['Echo (2nd generation)','radar_puffin','MT8163','Confirmed'],['Echo Dot (2nd generation)','biscuit_puffin','MT8163','Confirmed'],['Echo Plus (1st generation)','knight_puffin','MT8163','Likely, untested'],['Echo Show 5 (1st generation)','crown_puffin','MT8163','Likely, untested'],['Echo Spot','pumpkin_puffin','MT8163','Likely, untested'],['Echo Input','gemini_puffin','MT8163','Likely, untested']])+`<p class="muted">Nothing outside this list is supported. Other Echo generations use different SoCs and have no known install path, so LibreEcho cannot be put on them today.</p>`)}</div>`}
async function render(){clearTimeout(state.timer);content.innerHTML='<div class="panel loading">Loading device state…</div>';try{if(state.page==='Overview')await overview();else if(state.page==='Device')await devicePage();else if(state.page==='Users')await usersPage();else if(state.page==='Audio')await audioPage();else if(state.page==='Baby Monitor')await babyMonitorPage();else if(state.page==='Wake Word')await wakePage();else if(state.page==='Simulation')await simulationPage();else if(state.page==='LED & Buttons')await ledPage();else if(state.page==='Network')await networkPage();else if(state.page==='Bluetooth')await bluetoothPage();else if(state.page==='Privacy')await privacyPage();else if(state.page==='Integrations'){installIntegrationsExtras();await integrationsPage()}else if(state.page==='System')await systemPage();else if(state.page==='Logs')await logsPage();else aboutPage()}catch(e){errorView(e)}applyCssVars(content);if(state.page==='Overview')state.timer=setTimeout(refreshOverview,5000)}
function showPage(name,updateRoute=true){let corrected=false;if(!descriptions[name]||!navItems().some(([n])=>n===name)){name='Overview';corrected=true}if(state.page==='Baby Monitor'&&name!=='Baby Monitor')stopBabyStream();state.page=name;const path='/'+pageSlug(name);
 /* A route to a page that is not in the menu is not a route. Replace it, so a
    reload or a back button does not land on it again. */
 if(location.pathname!==path){if(corrected)history.replaceState(null,'',path);else if(updateRoute)history.pushState(null,'',path)}$$('.nav-item').forEach(x=>x.classList.toggle('active',x.dataset.page===name));$('#page-title').textContent=name;$('#page-subtitle').textContent=descriptions[name];document.title=`${name} · LibreEcho`;document.body.classList.remove('nav-open');render()}
renderNav();
$('#reboot').onclick=()=>power('reboot','Reboot');$('#theme').onclick=()=>{const light=document.body.classList.toggle('light');localStorage.setItem('libreecho-theme',light?'light':'dark');$('#theme').textContent=light?'☾':'☼'};$('#menu').onclick=()=>document.body.classList.toggle('nav-open');
$('#update-available').onclick=()=>showPage('System');
window.addEventListener('popstate',()=>showPage(pageFromLocation(),false));
if(localStorage.getItem('libreecho-theme')==='light'){document.body.classList.add('light');$('#theme').textContent='☾'}
function updateAuthControl(){const b=$('#auth-control');if(!b)return;if(state.authMode==='development-disabled'){b.textContent='Dev access';b.title='Authentication is disabled for this development image';b.disabled=true;return}b.disabled=false;b.title=state.token?'Sign out':'Sign in';b.textContent=state.token?`${state.username||'Signed in'} · Sign out`:'Sign in'}
function clearSession(){state.token='';state.username='';sessionStorage.removeItem('libreecho-token');sessionStorage.removeItem('libreecho-username');updateAuthControl()}
function redirectToLogin(){clearSession();if(location.pathname!=='/login')location.replace('/login')}
async function ensureAuth(c){state.authMode=c.authentication;updateAuthControl();if(c.authentication!=='users'&&c.authentication!=='bearer-token'){document.body.classList.remove('auth-pending');return}if(!state.token){redirectToLogin();throw new Error('Sign in is required')}try{const current=await api('/auth');state.username=current.username||state.username||'token';sessionStorage.setItem('libreecho-username',state.username);updateAuthControl();document.body.classList.remove('auth-pending')}catch(_){redirectToLogin();throw new Error('Your session is no longer valid')}}
async function signOut(){if(!state.token){redirectToLogin();return}try{await api('/auth/logout',{method:'POST',body:'{}'})}catch(_){/* The local session is cleared even if the server is unreachable. */}finally{redirectToLogin()}}
$('#auth-control').onclick=()=>state.token?signOut():redirectToLogin();
if(location.hash.length>1){const legacy=decodeURIComponent(location.hash.slice(1));if(items.some(([n])=>pageSlug(n)===legacy))history.replaceState(null,'','/'+legacy);}
api('/config').then(async c=>{state.csrf=c.csrf_token;await ensureAuth(c);return Promise.all([api('/status'),api('/device'),api('/system/update').catch(()=>({supported:false,check_status:'not-checked'})),api('/system/features').catch(()=>({simulation:false}))])}).then(([,d,ota,features])=>{applyFeatures(features);updateVersionDisplay(d,ota);return showPage(pageFromLocation(),false)}).catch(error=>{if(state.authMode==='users'||state.authMode==='bearer-token')redirectToLogin();else{document.body.classList.remove('auth-pending');errorView(error)}});
