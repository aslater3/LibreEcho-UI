'use strict';
const $=(s,r=document)=>r.querySelector(s), $$=(s,r=document)=>[...r.querySelectorAll(s)];
const state={page:'Overview',csrf:'',token:sessionStorage.getItem('libreecho-token')||'',username:sessionStorage.getItem('libreecho-username')||'',authMode:'development-disabled',timer:null,data:{},busy:false};
const items=[['Overview','home'],['Device','device'],['Users','shield'],['Audio','audio'],['Baby Monitor','mic'],['Wake Word','mic'],['Simulation','mic'],['LED & Buttons','sun'],['Network','wifi'],['Bluetooth','bluetooth'],['Privacy','shield'],['Integrations','puzzle'],['System','gear'],['Logs','log'],['About','info']];
const descriptions={Overview:'Your LibreEcho at a glance',Device:'Identity, hardware and power controls',Users:'Manage local accounts and access',Audio:'Playback, microphone and volume controls','Baby Monitor':'Listen to selected microphones locally','Wake Word':'Configure local wake-word detection',Simulation:'Speak test phrases into the microphone path','LED & Buttons':'Customise light-ring behaviour and controls',Network:'Wi-Fi, addressing and connectivity',Bluetooth:'Scan, pair and manage nearby devices',Privacy:'Local processing and data retention controls',Integrations:'Connect services and home automation',System:'Updates, backup and advanced settings',Logs:'Diagnostics and troubleshooting',About:'Project, licences and version information'};
const nav=$('#nav'),content=$('#content');
document.body.classList.add('auth-pending');
function esc(v){return String(v??'').replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]))}
async function api(path,opt={}){const headers={'Accept':'application/json',...(state.token?{'Authorization':'Bearer '+state.token}:{}),...(opt.body?{'Content-Type':'application/json','X-LibreEcho-CSRF':state.csrf}:{}),...(opt.headers||{})};const res=await fetch('/api/v1'+path,{...opt,headers});let body;try{body=await res.json()}catch(_){throw new Error('The device returned an unreadable response')}if(!res.ok||!body.ok||body.data?.unavailable===true)throw new Error(body.error?.message||body.data?.message||`Request failed (${res.status})`);return body.data}
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
function storageValue(s){const total=Number(s?.storage_total_mb);if(!Number.isFinite(total)||total<=0)return'Unavailable';if(s.storage_available&&Number.isFinite(Number(s.storage_used_mb)))return`${Number(s.storage_used_mb)} / ${total} MB`;return`${total} MB capacity · usage unavailable`}
function metric(icon,label,value,percent,connected=false,powerLed=false){if(label==='Storage')value=storageDisplay(value);const numericPercent=Number(percent),hasPercent=Number.isFinite(numericPercent);return `<div class="metric ${powerLed?'binary-metric':''}"><svg><use href="#${icon}"></use></svg><span>${label}</span><span class="value ${connected?'connected':''}">${esc(value)}</span>${powerLed?`<span class="power-led ${connected?'on':'off'}" role="img" aria-label="${connected?'Available':'Unavailable'}"></span>`:hasPercent?`<progress class="bar" max="100" value="${Math.max(0,Math.min(100,numericPercent))}" aria-label="${esc(label)}: ${esc(value)}"></progress>`:''}</div>`}
function cpuDashboard(s){const cores=Array.isArray(s.cpus)?s.cpus:(Array.isArray(s.cpus?.cores)?s.cpus.cores:[]);return `<section class="panel cpu-panel" id="cpu-dashboard"><div class="cpu-heading"><h3>CPU cores</h3><span>${cores.filter(c=>c.online).length}/${cores.length} online</span></div><div class="cpu-core-grid">${cores.map(c=>`<article class="cpu-core ${c.online?'online':'offline'}"><div><strong>CPU${esc(c.id)}</strong><small>${c.online?'Up':'Down'} · ${c.frequency_khz?Math.round(c.frequency_khz/1000)+' MHz':'—'}</small></div><b>${c.online?Number(c.utilization_percent??c.utilization??0)+'%':'—'}</b><progress class="bar" max="100" value="${c.online?Math.max(0,Math.min(100,Number(c.utilization_percent??c.utilization??0))):0}" aria-label="CPU${c.id} utilization"></progress></article>`).join('')}</div></section>`}
function playbackSource(source){return ({airplay2:'AirPlay 2',bluetooth:'Bluetooth',media:'Media',system:'System',announcement:'Announcement',alarm:'Alarm'})[source]||source||'LibreEcho'}
function nowPlaying(p={},l={}){const active=p.state&&p.state!=='idle',metadata=p.metadata||{},levels=Array.isArray(l.visualizer_levels)&&l.visualizer_levels.length===12?l.visualizer_levels:Array(12).fill(0);let title='Nothing playing',detail='Your audio sources will appear here',source='Idle';if(p.state==='playing'){title=metadata.title||`${playbackSource(p.source)} audio`;detail=[metadata.artist,metadata.album].filter(Boolean).join(' · ')||'Playing now';source=playbackSource(p.source)}else if(p.state==='announcing'){title='Announcement in progress';detail='Voice and notification audio has priority';source='Announcement'}else if(p.state==='alarm'){title='Alarm active';detail='Alarm audio has priority';source='Alarm'}else if(p.state==='system'){title='System audio';detail='LibreEcho is playing a system sound';source='System'}return `<section class="panel now-playing ${active?'active':'idle'}" id="now-playing"><div class="now-playing-mark" aria-hidden="true"><svg viewBox="0 0 100 100"><circle cx="50" cy="50" r="36"></circle><circle cx="50" cy="50" r="8"></circle></svg></div><div class="now-playing-copy"><span class="source-pill">${esc(source)}</span><h3>${esc(title)}</h3><p>${esc(detail)}</p></div><div class="spectrum-mini" aria-label="${l.visualizer_active?'Live 12-band music spectrum':'Audio spectrum inactive'}">${levels.map((v,i)=>`<i data-level="${Math.max(4,Math.min(100,Math.round(Number(v)||0)))}" data-band="${i}"></i>`).join('')}</div><div class="now-playing-state"><span class="status-dot ${active?'ok':''}"></span>${esc(p.state||'idle')}</div></section>`}
function uptime(s){s=Math.max(0,Number(s)||0);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return `${d?d+'d ':''}${h}h ${m}m`}
function logTime(t,boot){const seconds=Number(t),relative=Number(boot);if(relative>0)return `Boot +${relative}s`;return seconds>=1577836800?new Date(seconds*1000).toLocaleTimeString():'Clock unset'}
function rgb(c){return `rgb(${c.r}, ${c.g}, ${c.b})`}
function networkLabel(n){return n.ssid?`${n.state} · ${n.ssid}`:n.state}
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
async function overview(){const [s,n,a,l,d,p,ota]=await Promise.all([api('/status'),api('/network'),api('/audio').catch(e=>({unsupported:e.message})),api('/led').catch(e=>({unsupported:e.message})),api('/device'),api('/playback').catch(()=>({state:'idle',source:null,metadata:{available:false}})),api('/system/update').catch(()=>({supported:false,check_status:'not-checked'}))]);state.data.status=s;$('#backend-badge').textContent=s.backend+(s.simulated?' · simulated':'');$('#backend-badge').className='backend-badge '+s.backend;$('#device-online').innerHTML=`<span></span>${esc(s.device_state)}`;$('#sidebar-uptime').textContent='Uptime: '+uptime(s.uptime_seconds);updateVersionDisplay(d,ota);content.innerHTML=`<div class="grid-top"><div class="panel hero"><div class="sim-label" id="hero-device-label">${esc(d.hostname||d.name||'LibreEcho')}</div><h2>LibreEcho</h2><p>Open source voice assistant<br>built for privacy and freedom.</p><img class="device-img" src="/assets/device.png" alt="Amazon Echo device"><div class="hero-actions">${action('Device details','device-details','primary-btn')}${linkAction('API','/api/v1')}${linkAction('Swagger','/swagger.html')}</div></div><div class="panel status-panel"><h3>System Status</h3>${metric('device','CPU Load',s.cpu_percent+'%',s.cpu_percent)}${metric('device','Memory',`${s.memory_used_mb} / ${s.memory_total_mb} MB`,s.memory_percent)}${metric('device','Storage',storageValue(s),s.storage_available?s.storage_percent:null)}${metric('sun','Temperature',s.temperature_c+' °C',s.temperature_c)}${metric('wifi','Wi-Fi',networkLabel(n),n.signal,n.state==='connected',true)}${metric('info','Internet',n.internet?'Reachable':'Unavailable',0,n.internet,true)}</div></div>${nowPlaying(p,l)}${cpuDashboard(s)}<div class="cards">${items.slice(2,10).map(([name,icon],i)=>`<button class="panel shortcut" data-page="${name}"><svg class="${['green','purple','blue','sky','green','orange','grey','orange'][i]}"><use href="#${icon}"></use></svg><span><strong>${name}</strong><small>${descriptions[name]}</small></span><span class="arrow">›</span></button>`).join('')}</div><div class="panel community"><img src="/assets/mark.svg" alt="" class="community-mark"><div><h3>Open Source. Community Driven.</h3><p>Configuration stays on your device. ${s.simulated?'This development session uses deterministic mock-capable hardware state.':'Values shown come from the Linux backend.'}</p></div></div>`;$$('[data-page]').forEach(b=>b.onclick=()=>showPage(b.dataset.page));$('#device-details').onclick=()=>showPage('Device')}
function updateOverviewMetric(label,value,percent,connected){const row=$$('.status-panel .metric').find(x=>x.querySelector('span')?.textContent===label);if(!row)return;if(label==='Storage')value=storageDisplay(value);const output=row.querySelector('.value');output.textContent=value;if(connected!==undefined)output.classList.toggle('connected',connected);const bar=row.querySelector('progress');if(bar)bar.value=Math.max(0,Math.min(100,percent));const led=row.querySelector('.power-led');if(led){led.classList.toggle('on',!!connected);led.classList.toggle('off',!connected);led.setAttribute('aria-label',connected?'Available':'Unavailable')}}
async function refreshOverview(){if(state.page!=='Overview')return;let delay=5000;try{const [s,n,d,p,l]=await Promise.all([api('/status'),api('/network'),api('/device'),api('/playback'),api('/led').catch(()=>({}))]);if(state.page!=='Overview')return;state.data.status=s;$('#backend-badge').textContent=s.backend+(s.simulated?' · simulated':'');$('#backend-badge').className='backend-badge '+s.backend;$('#device-online').innerHTML=`<span></span>${esc(s.device_state)}`;$('#sidebar-uptime').textContent='Uptime: '+uptime(s.uptime_seconds);$('#sidebar-version').textContent=d.os_version;if(Date.now()-(state.data.otaCheckedAt||0)>=60000)updateVersionDisplay(d,await api('/system/update').catch(()=>state.data.ota));const deviceLabel=$('#hero-device-label');if(deviceLabel)deviceLabel.textContent=d.hostname||d.name||'LibreEcho';updateOverviewMetric('CPU Load',s.cpu_percent+'%',s.cpu_percent);updateOverviewMetric('Memory',`${s.memory_used_mb} / ${s.memory_total_mb} MB`,s.memory_percent);updateOverviewMetric('Storage',storageValue(s),s.storage_available?s.storage_percent:null);updateOverviewMetric('Temperature',s.temperature_c+' °C',s.temperature_c);updateOverviewMetric('Wi-Fi',networkLabel(n),0,n.state==='connected');updateOverviewMetric('Internet',n.internet?'Reachable':'Unavailable',0,n.internet);const playing=$('#now-playing');if(playing)playing.outerHTML=nowPlaying(p,l);const cpu=$('#cpu-dashboard');if(cpu)cpu.outerHTML=cpuDashboard(s);applyCssVars(content);if(p.state!=='idle'||l.visualizer_active)delay=1000}catch(_){/* Preserve the last good telemetry when a background refresh fails. */}finally{if(state.page==='Overview')state.timer=setTimeout(refreshOverview,delay)}}
async function usersPage(){const u=await api('/auth/users');const rows=u.users.map(x=>`<div class="status-line"><span class="status-dot ok"></span><span>${esc(x.username)}${x.username===state.username?' <small>(current session)</small>':''}</span>${u.users.length>1&&x.username!==state.username?action('Remove','remove-user-'+esc(x.username),'danger-btn'):''}</div>`).join('');content.innerHTML=`<div class="settings-grid">${panel('Local accounts',`<p class="muted">These accounts can sign in to the LibreEcho control centre. Passwords are stored as salted hashes and are never displayed.</p><div class="user-list">${rows||'<p class="muted">No users configured.</p>'}</div>`)}${panel('Add user',field('Username','','new-user-name','text','autocomplete="username" maxlength="31" pattern="[A-Za-z0-9._-]+"')+field('Password','','new-user-password','password','autocomplete="new-password" minlength="8" maxlength="128"')+field('Confirm password','','new-user-confirm','password','autocomplete="new-password" minlength="8" maxlength="128"')+`<div class="button-row">${action('Add user','add-user','primary-btn')}</div>`)} </div>`;u.users.filter(x=>x.username!==state.username).forEach(x=>{const b=$('#remove-user-'+x.username);if(b)b.onclick=async()=>{if(!confirm(`Remove user ${x.username}?`))return;try{await api('/auth/users/'+encodeURIComponent(x.username),{method:'DELETE'});toast('User removed');await usersPage()}catch(e){toast(e.message,true)}}});$('#add-user').onclick=async()=>{const username=$('#new-user-name').value.trim(),password=$('#new-user-password').value,confirmPassword=$('#new-user-confirm').value;if(password!==confirmPassword){toast('Passwords do not match',true);return}try{await api('/auth/users',{method:'POST',body:JSON.stringify({username,password,password_confirm:confirmPassword})});toast('User added');await usersPage()}catch(e){toast(e.message,true)}}}
async function devicePage(){const d=await api('/device');content.innerHTML=`<div class="settings-grid">${panel('Device identity',field('Device name',d.name,'device-name','text','disabled')+field('Hostname',d.hostname,'hostname')+field('Model',d.model,'model','text','disabled')+field('Serial / development ID',d.serial,'serial','text','disabled')+saveButton('save-device'))}${panel('Platform',`<dl class="facts"><dt>OS version</dt><dd>${esc(d.os_version)}</dd><dt>Kernel</dt><dd>${esc(d.kernel)}</dd><dt>Hardware revision</dt><dd>${esc(d.hardware_revision)}</dd><dt>Backend</dt><dd>${esc(d.backend)}</dd></dl>`)}${panel('Power controls',`<p class="muted">These actions require a confirmation token and are rate limited.</p><div class="button-row">${action('Reboot','power-reboot','outline-btn')}${action('Shut down','power-shutdown','danger-btn')}${action('Factory reset','power-reset','danger-btn')}</div>`,'wide')}</div>`;bindDirty(['#hostname'],'#save-device');$('#save-device').onclick=()=>mutate('/network',{hostname:$('#hostname').value},'Device changes saved');$('#power-reboot').onclick=()=>power('reboot','Reboot');$('#power-shutdown').onclick=()=>power('shutdown','Shut down');$('#power-reset').onclick=()=>power('factory-reset','Factory reset')}
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
 ['Self-correction','Alexa, set a five, no, ten minute timer']];
const SIM_HISTORY_KEY='libreecho-simulation-history';
const SIM_HISTORY_MAX=100;
function simHistory(){
 try{const raw=localStorage.getItem(SIM_HISTORY_KEY);const v=raw?JSON.parse(raw):[];
     return Array.isArray(v)?v:[]}catch(_){return []}}
function simHistorySave(list){
 try{localStorage.setItem(SIM_HISTORY_KEY,JSON.stringify(list.slice(0,SIM_HISTORY_MAX)))}catch(_){/* private mode, quota */}}
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
 for(const e of entries){
  if(e.boot_seconds<since)continue;
  const m=e.message||'';
  let g;
  if((g=m.match(/audio_ms=(\d+)\s+processing_ms=(\d+)\s+total_ms=(\d+)/))){
   out.audio_ms=+g[1]; out.processing_ms=+g[2]; out.total_ms=+g[3];
   out.transcript_at=e.boot_seconds;
   const chars=m.match(/text_chars=(\d+)/); if(chars)out.text_chars=+chars[1];
  }
  if(/speak requested/.test(m)&&out.speak_at===undefined)out.speak_at=e.boot_seconds;
  if(/streaming first PCM chunk/.test(m)&&out.first_audio_at===undefined)out.first_audio_at=e.boot_seconds;
  if(/Simulated audio queued/.test(m)&&out.queued_at===undefined)out.queued_at=e.boot_seconds;
 }
 if(out.queued_at!==undefined&&out.transcript_at!==undefined)
  out.queue_to_transcript_ms=(out.transcript_at-out.queued_at)*1000;
 if(out.transcript_at!==undefined&&out.first_audio_at!==undefined)
  out.transcript_to_audio_ms=(out.first_audio_at-out.transcript_at)*1000;
 if(out.queued_at!==undefined&&out.first_audio_at!==undefined)
  out.queue_to_first_audio_ms=(out.first_audio_at-out.queued_at)*1000;
 return out;
}
function simRow(r){
 const cap=r.audio_ms&&r.max_utterance_ms&&r.audio_ms>=r.max_utterance_ms-100;
 return `<tr><td>${esc(new Date(r.at).toLocaleTimeString())}</td>`+
  `<td class="${r.wake?'connected':''}">${r.wake?'yes':'no'}</td>`+
  `<td>${esc(r.text.length>34?r.text.slice(0,34)+'…':r.text)}</td>`+
  `<td>${ms(r.wake_latency_ms)}</td>`+
  `<td class="${cap?'error-text':''}">${ms(r.audio_ms)}${cap?' (cap)':''}</td>`+
  `<td>${ms(r.processing_ms)}</td>`+
  `<td>${ms(r.queue_to_first_audio_ms)}</td></tr>`;
}
function simRender(){
 const h=simHistory();
 const el=$('#sim-history'); if(!el)return;
 if(!h.length){el.innerHTML='<p class="muted">No runs yet.</p>';return}
 el.innerHTML='<div class="table-scroll"><table class="sim-table"><thead><tr>'+
  '<th>time</th><th>wake</th><th>phrase</th><th>wake latency</th>'+
  '<th>turn audio</th><th>stt+llm</th><th>to first audio</th></tr></thead><tbody>'+
  h.map(simRow).join('')+'</tbody></table></div>';
 const c=$('#sim-count-runs'); if(c)c.textContent=h.length+' of '+SIM_HISTORY_MAX;
}
/*
 * One simulated utterance, start to finish. Shared by the single-phrase button
 * and the run-all sweep so both record exactly the same fields -- a history
 * where some rows were measured differently is worse than no history.
 */
async function simRun(phrase,cap,onStatus){
 const pre=await api('/logs').catch(()=>({entries:[]}));
 const mark=(pre.entries||[]).reduce((m,e)=>Math.max(m,e.boot_seconds||0),0);
 const before=(await api('/wake-word').catch(()=>({}))).detected_count??0;
 const t0=performance.now();
 await api('/audio/simulate',{method:'POST',body:JSON.stringify({text:phrase})});
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
 simHistorySave([entry,...simHistory()]);
 simRender();
 return entry;
}
function simTimingHtml(entry,cap){
 const capped=entry.audio_ms&&cap&&entry.audio_ms>=cap-100;
 return '<dt>Wake</dt><dd class="'+(entry.wake?'connected':'')+'">'+(entry.wake?'detected':'not detected')+'</dd>'+
  '<dt>Wake latency</dt><dd>'+ms(entry.wake_latency_ms)+'</dd>'+
  '<dt>Turn audio</dt><dd class="'+(capped?'error-text':'')+'">'+ms(entry.audio_ms)+
    (capped?' — hit the utterance cap, endpointing did not fire':'')+'</dd>'+
  '<dt>STT + model</dt><dd>'+ms(entry.processing_ms)+'</dd>'+
  '<dt>Queue to transcript</dt><dd>'+ms(entry.queue_to_transcript_ms)+'</dd>'+
  '<dt>Transcript to audio</dt><dd>'+ms(entry.transcript_to_audio_ms)+'</dd>'+
  '<dt>Queue to first audio</dt><dd>'+ms(entry.queue_to_first_audio_ms)+'</dd>';
}
async function simulationPage(){
 const w=await api('/wake-word').catch(()=>({}));
 const vp=await api('/voice-pipeline').catch(()=>({}));
 const cap=vp?.listening?.max_utterance_ms;
 content.innerHTML=`<div class="settings-grid">${panel('Speak a phrase',
   `<label class="field"><span>Phrase</span><select id="sim-preset">`+
   SIM_PHRASES.map(([l],i)=>`<option value="${i}">${esc(l)}</option>`).join('')+
   `<option value="custom">Custom…</option></select></label>`+
   field('Text','Alexa, what time is it?','sim-text')+
   `<p class="muted">Rendered by the device's own text-to-speech and played into the microphone path. Nothing is recorded and nothing leaves the device.</p>`+
   `<div class="button-row">${action('Speak into the microphone','sim-send','primary-btn')}${action('Run all '+SIM_PHRASES.length+' phrases','sim-run-all')}<button class="secondary-btn" id="sim-stop-all" hidden>Stop</button></div>`+
   `<dl class="facts"><dt>Wake word</dt><dd>${esc(w.wake_word||'—')}</dd><dt>Sensitivity</dt><dd>${w.sensitivity??'—'}</dd><dt>Utterance cap</dt><dd>${ms(cap)}</dd></dl>`)}
   ${panel('Last run',`<dl class="facts" id="sim-timing"><dt>Status</dt><dd>Nothing sent yet</dd></dl>`)}
   </div>
   ${panel('History',`<div class="button-row"><span class="muted" id="sim-count-runs">0 of ${SIM_HISTORY_MAX}</span>`+
     action('Download JSON','sim-download')+action('Clear','sim-clear')+`</div><div id="sim-history"></div>`,'sim-history-panel')}`;
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
 $('#sim-clear').onclick=()=>{ simHistorySave([]); simRender(); toast('History cleared') };
 $('#sim-send').onclick=async()=>{
  const phrase=text.value.trim();
  if(!phrase){toast('Enter a phrase first',true);return}
  if(state.simBusy)return;
  const timing=$('#sim-timing');
  state.simBusy=true;
  timing.innerHTML='<dt>Status</dt><dd>Rendering and playing…</dd>';
  try{
   const entry=await simRun(phrase,cap,t=>{timing.innerHTML='<dt>Status</dt><dd>'+esc(t)+'</dd>'});
   timing.innerHTML=simTimingHtml(entry,cap);
   toast(entry.wake?'Wake detected':'No wake detected',!entry.wake);
  }catch(e){ timing.innerHTML='<dt>Status</dt><dd>'+esc(e.message)+'</dd>'; toast(e.message,true); }
  finally{ state.simBusy=false; }
 };
 /*
  * Run every preset in turn. Each utterance makes the device answer out loud,
  * so leave a gap between them: starting the next phrase while the speaker is
  * still going would be testing barge-in, not the phrase.
  */
 $('#sim-run-all').onclick=async()=>{
  if(state.simBusy)return;
  state.simBusy=true; state.simStop=false;
  const timing=$('#sim-timing'),btn=$('#sim-run-all'),stop=$('#sim-stop-all');
  btn.disabled=true; if(stop)stop.hidden=false;
  let done=0,woke=0;
  try{
   for(const [label,phrase] of SIM_PHRASES){
    if(state.simStop){toast('Stopped after '+done+' phrases');break}
    timing.innerHTML='<dt>Running</dt><dd>'+esc(label)+' ('+(done+1)+' of '+SIM_PHRASES.length+')</dd>';
    try{ const e=await simRun(phrase,cap); if(e.wake)woke++; }
    catch(err){ /* record nothing; keep going so one failure does not end the sweep */ }
    done++;
    if(!state.simStop&&done<SIM_PHRASES.length)await new Promise(r=>setTimeout(r,3000));
   }
   timing.innerHTML='<dt>Sweep complete</dt><dd>'+woke+' of '+done+' phrases woke the device</dd>';
   toast(woke+' of '+done+' woke the device');
  } finally {
   state.simBusy=false; btn.disabled=false; if(stop)stop.hidden=true;
  }
 };
 if($('#sim-stop-all'))$('#sim-stop-all').onclick=()=>{state.simStop=true};

}
async function wakePage(){let w;try{w=await api('/wake-word')}catch(e){if(/not available|not supported/i.test(e.message)){content.innerHTML=`<div class="settings-grid">${panel('Wake-word engine',unsupported('The wake-word adapter is not installed in this image. No microphone processing is running.'))}</div>`;return}throw e}const approved=['LibreEcho','Computer','Echo','Custom model'],current=String(w.wake_word||''),wakeOptions=approved.includes(current)?approved:[current+' (current)',...approved];content.innerHTML=`<div class="settings-grid">${panel('Wake-word engine',`<dl class="facts"><dt>Detection</dt><dd class="${w.enabled?'connected':''}">${w.enabled?'Enabled':'Disabled'}</dd></dl><p class="muted">Detection enablement is reported by the live wake-word adapter and is not configurable through this API.</p>`+select('Active wake word',current,'wake-word',wakeOptions)+range('Detection sensitivity',w.sensitivity,'sensitivity')+`<dl class="facts"><dt>Detection cooldown</dt><dd>${w.cooldown_ms} ms</dd></dl>`+`<div class="button-row">${saveButton('save-wake')}${action('Simulate detection','wake-test')}</div>`)}${panel('Local processing',`<div class="privacy-callout">✓ Audio remains on this device</div><dl class="facts"><dt>Model status</dt><dd class="connected">${esc(w.model_status)}</dd><dt>Detections</dt><dd>${w.detected_count}</dd><dt>Estimated CPU cost</dt><dd>${w.cpu_cost_percent}%</dd><dt>Estimated memory cost</dt><dd>${w.memory_cost_mb} MB</dd></dl>`)}</div>`;bindRange();bindDirty(['#wake-word','#sensitivity'],'#save-wake');$('#save-wake').onclick=()=>mutate('/wake-word',{wake_word:$('#wake-word').value.replace(/ \(current\)$/,''),sensitivity:+$('#sensitivity').value},'Wake-word changes saved');$('#wake-test').onclick=()=>post('/wake-word/test',{},'Wake word detected')}
function ledRing(l){const physical=Array.isArray(l.pixels)&&l.pixels.length===12,colours=physical?l.pixels:Array.from({length:24},()=>l.colour),count=colours.length,opacity=physical?1:Math.max(0,Math.min(1,Number(l.brightness??0)/100));const dots=colours.map((colour,i)=>{const angle=(i/count)*Math.PI*2-Math.PI/2,x=50+38*Math.cos(angle),y=50+38*Math.sin(angle);return `<circle class="led-pixel" cx="${x.toFixed(2)}" cy="${y.toFixed(2)}" r="${physical?'6.4':'5.5'}" fill="${rgb(colour)}" opacity="${opacity.toFixed(2)}"></circle>`}).join('');const label=l.pattern_active?`Pattern · ${esc(l.pattern||'pattern')}`:l.visualizer_active?`Music visualizer · ${esc(l.visualizer_mood||'balanced')}`:l.animation_active?`Animating · ${esc(l.animation_profile||'profile')}`:'Steady',moving=l.animation_active||l.pattern_active;return `<div class="led-ring-view${moving?' animating':''}${l.visualizer_active?' reactive':''}" role="img" aria-label="${esc(label)} LED ring"><svg viewBox="0 0 100 100" aria-hidden="true"><circle class="led-ring-track" cx="50" cy="50" r="38"></circle>${dots}<circle class="led-ring-centre" cx="50" cy="50" r="24"></circle></svg><span>${esc(label)}</span></div>`}
function updateLedVisual(l){const ring=$('.led-ring-view');if(ring)ring.outerHTML=ledRing(l);const preview=$('.led-preview');if(preview)preview.style.setProperty('--led',rgb(l.colour))}
async function refreshLed(){if(state.page!=='LED & Buttons')return;let delay=1000;try{const l=await api('/led');updateLedVisual(l);if(l.visualizer_active)delay=200}catch(_){/* Preserve the last good LED state during a transient adapter failure. */}finally{if(state.page==='LED & Buttons')state.timer=setTimeout(refreshLed,delay)}}
async function ledPage(){const [l,b]=await Promise.all([api('/led'),api('/buttons')]);const hex='#'+[l.colour.r,l.colour.g,l.colour.b].map(n=>n.toString(16).padStart(2,'0')).join('');const names={listening:'Listening',thinking:'Thinking',error:'Error',dnd:'Do not disturb',night:'Night'};const n=l.night||{enabled:false,active:false,start_minute:1320,end_minute:420};const hhmm=m=>String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');const mins=t=>{const p=String(t||'').split(':');return p.length===2?(+p[0])*60+(+p[1]):NaN};const profiles=Object.entries(l.profiles||{}).map(([k,v])=>{const colour=typeof v==='string'?v:'#'+[v.r,v.g,v.b].map(n=>Number(n).toString(16).padStart(2,'0')).join('');const brightness=typeof v==='object'&&v?Number(v.brightness??l.brightness):l.brightness;return `<div class="led-profile"><label class="field"><span>${names[k]||k}</span><input id="led-profile-${k}" data-profile="${k}" type="color" value="${colour}"></label><div class="swatch-row"><code id="led-profile-hex-${k}">${esc(colour)}</code></div>${range('Brightness',brightness,`led-profile-brightness-${k}`)}<button class="secondary-btn profile-save" data-profile="${k}">Save ${names[k]||k}</button></div>`}).join('');content.innerHTML=`<div class="settings-grid">${panel('Light ring',`<label class="field"><span>Current colour</span><input id="led-colour" type="color" value="${hex}"></label>`+range('Brightness',l.brightness,'led-brightness')+toggle('Music visualizer',l.visualizer_enabled!==false,'led-visualizer')+`<p class="muted">Show reactive equalizer animations on the ring while music is playing.</p>`+ledRing(l)+`<div class="led-preview" data-led="${rgb(l.colour)}"><i></i><span>Live preview</span></div><div class="button-row">${saveButton('save-led')}${action('Run LED test','led-test')}</div>`)}${panel('State themes',`<p class="muted">Choose the ring colour and brightness used while LibreEcho is listening, thinking, reporting an error, or in do-not-disturb mode.</p><div class="led-profiles">${profiles}</div>`)}${panel('Night mode',`<p class="muted">Between these times the ring is capped to the night brightness. Colours are kept, so the device still shows what it is doing -- just dimly. Times are the device's local time.</p>${toggle('Enable night mode',n.enabled,'night-enabled')}<div class="settings-grid">${field('Starts','','night-start','time')}${field('Ends','','night-end','time')}</div><div class="status-line"><span class="status-dot ${n.active?'ok':''}"></span><span>${n.active?'Night mode is active now':'Not active right now'}</span></div>${saveButton('save-night')}`)}${panel('Buttons',select('Short press',b.short_press,'short-action',['Start listening','Play / pause','Run automation','Disabled'])+select('Long press',b.long_press,'long-action',['Open pairing mode','Toggle privacy mode','Reboot device'])+toggle('Hardware mute state',b.hardware_mute,'hw-mute',true)+saveButton('save-buttons'),'wide')}</div>`;bindRange();if($('#night-start')){$('#night-start').value=hhmm(n.start_minute);$('#night-end').value=hhmm(n.end_minute);bindDirty(['#night-enabled','#night-start','#night-end'],'#save-night');$('#save-night').onclick=()=>{const s2=mins($('#night-start').value),e2=mins($('#night-end').value);if(!Number.isFinite(s2)||!Number.isFinite(e2)){toast('Enter both times as HH:MM',true);return}mutate('/led/night',{enabled:$('#night-enabled').checked,start_minute:s2,end_minute:e2},'Night mode saved')}}
bindDirty(['#led-colour','#led-brightness','#led-visualizer'],'#save-led');bindDirty(['#short-action','#long-action'],'#save-buttons');$('#save-led').onclick=()=>{const h=$('#led-colour').value;mutate('/led',{r:parseInt(h.slice(1,3),16),g:parseInt(h.slice(3,5),16),b:parseInt(h.slice(5,7),16),brightness:+$('#led-brightness').value,visualizer_enabled:$('#led-visualizer').checked},'LED changes saved')};$$('input[type=color][data-profile]').forEach(i=>{const out=$('#led-profile-hex-'+i.dataset.profile);if(out)i.oninput=()=>{out.textContent=i.value}});
$$('.profile-save').forEach(button=>button.onclick=()=>{const k=button.dataset.profile;const h=$(`#led-profile-${k}`).value;const brightnessInput=$(`#led-profile-brightness-${k}`);mutate('/led/profile',{name:k,r:parseInt(h.slice(1,3),16),g:parseInt(h.slice(3,5),16),b:parseInt(h.slice(5,7),16),brightness:+brightnessInput.value},`${names[k]||k} theme saved`)});$('#save-buttons').onclick=()=>mutate('/buttons',{short_press:$('#short-action').value,long_press:$('#long-action').value},'Button changes saved');$('#led-test').onclick=()=>post('/led/test',{},'LED test started');state.timer=setTimeout(refreshLed,1000)}
async function networkPage(){const n=await api('/network'),lanEffective=n.api_lan_effective??n.api_lan,lanForced=!!n.api_lan_forced,healthy=n.connectivity==='healthy',healthDetail=n.recovery_stage&&n.recovery_stage!=='none'?`Recovery: ${n.recovery_stage}`:`Gateway: ${n.gateway_reachable===true?'reachable':n.gateway_reachable===false?'unreachable':'not yet verified'}`;content.innerHTML=`<div class="settings-grid">${panel('Wireless network',`<div class="status-line"><span class="status-dot ${healthy?'ok':''}"></span><div><strong>${esc(n.state)} · ${esc(n.connectivity||'unknown')}</strong><small>${esc(n.ssid||'No network')} · ${esc(healthDetail)}</small></div><span>${n.signal||0}%</span></div><div id="wifi-results" class="wifi-results"><p class="muted">Scan to discover nearby networks.</p></div><div class="button-row">${action('Scan Wi-Fi','wifi-scan','primary-btn')}${action('Disconnect','wifi-disconnect')}</div>`)}${panel('Addressing',field('Hostname',n.hostname,'net-hostname')+toggle('Use DHCP',n.dhcp,'dhcp',true)+`<dl class="facts"><dt>IP address</dt><dd>${esc(n.ip||'—')}</dd><dt>Gateway</dt><dd>${esc(n.gateway||'—')}</dd><dt>DNS</dt><dd>${esc(n.dns||'—')}</dd><dt>Internet</dt><dd class="${n.internet?'connected':''}">${n.internet?'Reachable':'Unavailable'}</dd></dl>`+saveButton('save-network'))}${panel('Local access',toggle('LAN API access',lanEffective,'api-lan',lanForced)+toggle('SSH',n.ssh,'ssh')+`<p class="muted">${lanForced?'LAN API access is forced by the development image binding. Authentication is intentionally disabled for this development build.':'Enabling LAN access exposes the API beyond loopback. Configure authentication before using this on an untrusted network.'}</p>`+saveButton('save-access'),'wide')}</div>`;bindDirty(['#net-hostname'],'#save-network');bindDirty(['#api-lan','#ssh'],'#save-access');$('#save-network').onclick=()=>mutate('/network',{hostname:$('#net-hostname').value},'Network changes saved');$('#save-access').onclick=()=>mutate('/network',{api_lan:$('#api-lan').checked,ssh:$('#ssh').checked},'Local access changes saved');$('#wifi-scan').onclick=async()=>{const box=$('#wifi-results');box.innerHTML='<p class="muted">Scanning…</p>';try{const s=await api('/network/wifi/scan');box.innerHTML=s.networks.length?s.networks.map(x=>`<button class="wifi-network" data-ssid="${esc(x.ssid)}" data-security="${esc(x.security)}"><span><strong>${esc(x.ssid)}</strong><small>${esc(x.security)}</small></span><span>${x.signal}%</span></button>`).join(''):'<p class="muted">No networks found.</p>';$$('.wifi-network').forEach(b=>b.onclick=()=>connectWifi(b.dataset.ssid,b.dataset.security))}catch(e){box.innerHTML=unsupported(e.message)}};$('#wifi-disconnect').onclick=()=>post('/network/wifi/disconnect',{},'Wi-Fi disconnected')}
async function connectWifi(ssid,security){let password='';if(security!=='open'){password=prompt(`Password for ${ssid}`)||'';if(!password)return}await post('/network/wifi/connect',{ssid,password,security},`Connecting to ${ssid}`)}
async function privacyPage(){const p=await api('/privacy');content.innerHTML=`<div class="settings-grid">${panel('Processing',toggle('Local speech recognition only',p.local_only,'local-only')+toggle('Retain microphone audio',p.audio_retention!=='none','audio-retention')+toggle('Diagnostic telemetry',p.diagnostic_telemetry,'telemetry')+toggle('Crash reports',p.crash_reports,'crash-reports')+saveButton('save-privacy'))}${panel('Retention',select('Log retention',String(p.log_retention_hours)+' hours','retention',['24 hours','168 hours','720 hours'])+`<div class="privacy-callout">No cloud dependency is enabled by default.</div><div class="button-row">${saveButton('save-retention')}${action('Reset privacy settings','privacy-reset','danger-btn')}</div>`)}</div>`;bindDirty(['#local-only','#audio-retention','#telemetry','#crash-reports'],'#save-privacy');bindDirty(['#retention'],'#save-retention');$('#save-privacy').onclick=()=>mutate('/privacy',{local_only:$('#local-only').checked,audio_retention:$('#audio-retention').checked?'24h':'none',diagnostic_telemetry:$('#telemetry').checked,crash_reports:$('#crash-reports').checked},'Privacy changes saved');$('#save-retention').onclick=()=>mutate('/privacy',{log_retention_hours:parseInt($('#retention').value,10)},'Retention changes saved');$('#privacy-reset').onclick=()=>confirm('Reset privacy settings?')&&toast('Privacy defaults restored')}
function assistantCard(a){const local=collapsiblePanel('Local LLM',`<div class="assistant-heading"><div><span class="source-pill">On-device</span><h4>Local LLM</h4><p class="muted">A local language model can run without a subscription or metered API billing. Provider setup will appear here when a reviewed model is installed.</p></div><div class="assistant-state"><span class="status-dot"></span>Not configured</div></div>`,'assistant-provider');if(a.unsupported)return `<section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${local}${collapsiblePanel('ChatGPT',unsupported(a.unsupported),'assistant-provider')}</section>`;const signedIn=a.authenticated,waiting=a.auth_state==='waiting',latency=Number(a.last_speech_end_to_first_pcm_ms||0),chatgpt=collapsiblePanel('ChatGPT',`<div class="assistant-heading"><div><span class="source-pill">Subscription</span><h4>${esc(a.provider_name||'ChatGPT')}</h4><p class="muted">Uses your ChatGPT subscription with device login. LibreEcho never asks for or stores an API key, and does not fall back to metered API billing.</p></div><div class="assistant-state"><span class="status-dot ${signedIn?'ok':''}"></span>${signedIn?'Connected':waiting?'Waiting for sign-in':'Not connected'}</div></div>${waiting?`<div class="device-code"><span>Enter this code</span><strong>${esc(a.user_code)}</strong><a class="primary-btn action-link" href="${esc(a.verification_url)}" target="_blank" rel="noopener">Open ChatGPT sign-in</a></div>`:''}<div class="settings-grid assistant-settings"><div>${toggle('Enable wake-to-reply voice loop',a.enabled,'assistant-enabled',!signedIn)}${field('Provider',a.provider_name||a.provider,'assistant-provider','text','disabled')}${field('Model',a.model,'assistant-model')}<label class="field"><span>Voice response prompt</span><textarea id="assistant-prompt" rows="8">${esc(a.prompt)}</textarea></label>${saveButton('save-assistant')}</div><div><dl class="facts"><dt>Wake audio</dt><dd class="${a.wake_connected?'connected':''}">${a.wake_connected?'Connected':'Unavailable'}</dd><dt>Post-AEC stream</dt><dd class="${a.audio_connected?'connected':''}">${a.audio_connected?'Connected':'Unavailable'}</dd><dt>Local STT</dt><dd>${a.recognizing?'Recognizing':'Ready'}</dd><dt>Completed voice turns</dt><dd>${Number(a.completed_transcripts||0)}</dd><dt>Last STT processing</dt><dd>${Number(a.last_stt_processing_ms||0)||'—'}${a.last_stt_processing_ms?' ms':''}</dd><dt>Speech end → first PCM</dt><dd class="${latency&&latency<=3000?'connected':''}">${latency?latency+' ms':'Not measured'}</dd><dt>Target</dt><dd>≤ ${Number(a.latency_target_ms||3000)} ms</dd><dt>Target violations</dt><dd>${Number(a.latency_violations||0)}</dd></dl><div class="button-row">${!signedIn&&!waiting?action('Connect ChatGPT','assistant-auth-start','primary-btn'):''}${waiting?action('Check sign-in','assistant-auth-poll','primary-btn'):''}${signedIn?action('Disconnect','assistant-logout','danger-btn'):''}</div>${signedIn?`<label class="field"><span>Test prompt</span><input id="assistant-test-text" value="Say hello in one short sentence."></label>${action('Speak test response','assistant-test')}`:''}</div></div>`,'assistant-provider',waiting);return `<section class="panel setting-panel voice-assistants wide"><h3>Voice Assistants</h3>${local}${chatgpt}</section>`}
async function assistantAction(path,message){if(state.busy)return;setBusy(true);try{await api(path,{method:'POST',body:'{}'});toast(message);await integrationsPage()}catch(e){toast(e.message,true);await integrationsPage()}finally{setBusy(false)}}
const WX_PROVIDERS=[['open-meteo','Open-Meteo'],['met-no','MET Norway'],['off','Off']];
function wxLabel(id){const m=WX_PROVIDERS.find(p=>p[0]===id);return m?m[1]:'Open-Meteo'}
function wxId(label){const m=WX_PROVIDERS.find(p=>p[1]===label);return m?m[0]:'open-meteo'}
function weatherCard(a){if(a.unsupported)return '';
return collapsiblePanel('Weather',`<p class="muted">Lets the assistant answer questions about conditions where the device is. Both providers are free and need no account; only the coordinates below are sent, and only when a location is set.</p><div class="settings-grid">${select('Provider',wxLabel(a.weather_provider),'wx-provider',WX_PROVIDERS.map(p=>p[1]))}${field('Location name','','wx-location','text','placeholder="Austin, Texas"')}${field('Latitude','','wx-lat','text','placeholder="30.2672"')}${field('Longitude','','wx-lon','text','placeholder="-97.7431"')}${saveButton('save-wx')}</div>`,'weather-provider',true)}
async function integrationsPage(){const [d,a]=await Promise.all([api('/integrations'),api('/assistant').catch(e=>({unsupported:e.message}))]);content.innerHTML=`<div class="integration-grid">${assistantCard(a)}${weatherCard(a)}${d.items.map(x=>collapsiblePanel(x.name,`<p class="muted">${x.id==='rest'?'Versioned local device API.':'Optional local integration; no cloud connection required.'}</p>${toggle('Enabled',x.enabled,'int-'+x.id)}<div class="status-line"><span class="status-dot ${x.enabled?'ok':''}"></span><span>${x.enabled?'Enabled':'Not configured'}</span></div>${saveButton('save-int-'+x.id)}`)).join('')}</div>`;d.items.forEach(x=>{bindDirty(['#int-'+x.id],'#save-int-'+x.id);$('#save-int-'+x.id).onclick=()=>mutate('/integrations/'+x.id,{enabled:$('#int-'+x.id).checked},x.name+' changes saved')});if(!a.unsupported&&$('#wx-provider')){$('#wx-location').value=a.home_location||'';$('#wx-lat').value=a.latitude||'';$('#wx-lon').value=a.longitude||'';bindDirty(['#wx-provider','#wx-location','#wx-lat','#wx-lon'],'#save-wx');$('#save-wx').onclick=()=>mutate('/assistant',{weather_provider:wxId($('#wx-provider').value),home_location:$('#wx-location').value.trim(),latitude:$('#wx-lat').value.trim(),longitude:$('#wx-lon').value.trim()},'Weather settings saved')}
if(!a.unsupported){bindDirty(['#assistant-enabled','#assistant-model','#assistant-prompt'],'#save-assistant');$('#save-assistant').onclick=()=>mutate('/assistant',{enabled:$('#assistant-enabled').checked,provider:'openai-codex',model:$('#assistant-model').value.trim(),prompt:$('#assistant-prompt').value.trim()},'Voice assistant settings saved');if($('#assistant-auth-start'))$('#assistant-auth-start').onclick=()=>assistantAction('/assistant/auth/start','ChatGPT device sign-in started');if($('#assistant-auth-poll'))$('#assistant-auth-poll').onclick=()=>assistantAction('/assistant/auth/poll','Sign-in status checked');if($('#assistant-logout'))$('#assistant-logout').onclick=()=>assistantAction('/assistant/logout','ChatGPT disconnected');if($('#assistant-test'))$('#assistant-test').onclick=()=>post('/assistant/respond',{text:$('#assistant-test-text').value},'Test response queued');if(a.auth_state==='waiting')state.timer=setTimeout(async()=>{if(state.page!=='Integrations')return;try{await api('/assistant/auth/poll',{method:'POST',body:'{}'});await integrationsPage()}catch(e){toast(e.message,true)}},3000)}}
async function systemPage(){
  const [s,d,ota]=await Promise.all([api('/system'),api('/device'),api('/system/update')]);
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
  content.innerHTML=`<div class="settings-grid">${panel('Software',`<dl class="facts"><dt>OS version</dt><dd>${esc(d.os_version)}</dd><dt>Kernel</dt><dd>${esc(d.kernel)}</dd><dt>Time zone</dt><dd>${esc(s.timezone)}</dd><dt>NTP</dt><dd class="${s.ntp?'connected':''}">${s.ntp?'Synchronized':esc(s.ntp_state||'Unavailable')}</dd><dt>Last synchronization</dt><dd>${esc(syncTime)}</dd><dt>Clock source</dt><dd>${esc(s.clock_source||'unknown')}</dd><dt>RTC</dt><dd class="${s.rtc_available?'connected':''}">${s.rtc_available?(s.rtc_persisted?'Available · synchronized':'Available'):'Unavailable'}</dd><dt>Time servers</dt><dd>${esc(s.ntp_servers||'Not configured')}</dd></dl>`)}${panel('System update',`<dl class="facts"><dt>Installed version</dt><dd>${esc(installedVersion)}</dd><dt>Latest version</dt><dd>${esc(ota.latest_version||'Unknown')}</dd><dt>Channel</dt><dd>${esc(ota.channel||s.update_channel)}</dd><dt>Source</dt><dd>${esc(sourceName)}</dd><dt>Source status</dt><dd class="${ota.source_reachable==='true'?'connected':''}">${esc(sourceStatus)}</dd><dt>Check result</dt><dd>${esc(checkResult)}</dd><dt>Last checked</dt><dd>${esc(checkTime)}</dd><dt>Last successful check</dt><dd>${esc(successTime)}</dd><dt>Automatic updates</dt><dd>${ota.automatic_updates?'Enabled':'Off'}</dd><dt>Current slot</dt><dd>${esc(ota.current_slot.toUpperCase())}</dd><dt>Inactive slot</dt><dd>${esc(ota.inactive_slot.toUpperCase())}</dd><dt>Install state</dt><dd>${esc(ota.state)}</dd><dt>Rollback</dt><dd>${ota.rollback_available?'Available':'Unavailable'}</dd>${ota.rollback_version?`<dt>Last rollback</dt><dd>${esc(ota.rollback_version)}</dd>`:''}</dl><progress class="progress" max="100" value="${ota.progress}" aria-label="Update progress"></progress>${updateControls}`)}${panel('Configuration and diagnostics',`<input id="restore-file" type="file" accept="application/json,.json" hidden><div class="button-row">${action('Download configuration','backup','primary-btn')}${action('Restore configuration','restore')}${action('Export diagnostics','export-diag')}</div><p class="muted">Configuration exports are versioned JSON and exclude Wi-Fi passwords, authentication tokens, logs and live telemetry. Restore validates the complete file before applying it.</p>`,'wide')}</div>`;
  if($('#save-update-settings')){bindDirty(['#automatic-updates','#update-channel'],'#save-update-settings');$('#save-update-settings').onclick=async()=>{const channel=$('#update-channel').value,automatic=$('#automatic-updates').checked;if(state.busy)return;setBusy(true);try{if(channel!==(ota.channel||'stable'))await api('/system/update/channel',{method:'PUT',body:JSON.stringify({channel})});if(automatic!==ota.automatic_updates)await api('/system/update/automatic',{method:'PUT',body:JSON.stringify({enabled:automatic})});toast('Update settings saved');await render()}catch(e){toast(e.message,true);await render()}finally{setBusy(false)}}}
  if($('#check-update'))$('#check-update').onclick=()=>post('/system/update/check',{},'Update check completed');
  if($('#install-update'))$('#install-update').onclick=()=>confirm(`Install signed update ${ota.latest_version} to slot ${ota.inactive_slot.toUpperCase()}?`)&&post('/system/update/apply',{},'Update verified and installed');
  if($('#select-update'))$('#select-update').onclick=()=>$('#update-file').click();
  if($('#update-file'))$('#update-file').onchange=async e=>{
    const file=e.target.files[0];
    if(!file)return;
    $('#update-name').textContent=file.name;
    if(file.size>ota.max_upload_bytes){toast('Update exceeds 32 MiB',true);e.target.value='';return}
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
  $('#export-diag').onclick=()=>post('/diagnostics/export',{},'Redacted diagnostics prepared');
}
async function logsPage(){const [l,d]=await Promise.all([api('/logs'),api('/diagnostics')]);const clock=l.entries.some(x=>Number(x.timestamp)<1577836800)?'Device clock unset; relative boot time is shown where available':'Device clock available';content.innerHTML=`${panel('Service logs',`<div class="log-toolbar"><input id="log-filter" placeholder="Filter logs" aria-label="Filter logs">${action('Refresh','log-refresh')}</div><div class="log-viewer" id="log-viewer">${l.entries.length?l.entries.map(x=>`<div data-log="${esc(x.message.toLowerCase())}"><time>${logTime(x.timestamp,x.boot_seconds)}</time><span class="level ${x.level}">${x.level}</span><span>${esc(x.message)}</span></div>`).join(''):'<p class="muted">No logs available.</p>'}</div><p class="muted">Source: ${esc(l.source||'web-memory')}; bounded to ${l.capacity} entries. ${clock}. Relative time is from boot; secrets are redacted before logging.</p>`)}<div class="settings-grid diagnostics">${panel('Diagnostic checks',d.checks.map(x=>`<div class="status-line"><span class="status-dot ${x.status==='ok'?'ok':''}"></span><span>${esc(x.name)}</span><strong>${esc(x.status)}</strong></div>`).join(''))}${panel('Mock fault injection',state.data.status?.simulated?`<p class="muted">Use <code>tools/mockctl.sh</code> with <code>--dev-controls</code> to inject deterministic faults.</p>${action('Fail next Wi-Fi scan','fail-wifi','danger-btn')}`:unsupported('Fault injection is only available in mock development builds.'))}</div>`;$('#log-filter').oninput=e=>$$('[data-log]').forEach(x=>x.hidden=!x.dataset.log.includes(e.target.value.toLowerCase()));$('#log-refresh').onclick=render;if($('#fail-wifi'))$('#fail-wifi').onclick=()=>post('/dev/mock',{action:'fail-next',value:'wifi-scan'},'Next Wi-Fi scan will fail')}
function aboutPage(){content.innerHTML=`<div class="settings-grid">${panel('LibreEcho',`<img class="about-mark" src="/assets/mark.svg" alt="LibreEcho mark"><p>Open source voice-assistant software built for privacy, repairability and local control.</p><dl class="facts"><dt>Web API</dt><dd>v1</dd><dt>Frontend</dt><dd>Dependency-free HTML, CSS and JavaScript</dd><dt>Daemon</dt><dd>Portable C99</dd><dt>Licence</dt><dd>MIT</dd></dl>`)}${panel('Hardware independence',`<p class="muted">The same frontend API works with both the realistic mock backend and the conservative Linux hardware adapter.</p><div class="privacy-callout">Open. Private. Yours.</div>`)}</div>`}
async function render(){clearTimeout(state.timer);content.innerHTML='<div class="panel loading">Loading device state…</div>';try{if(state.page==='Overview')await overview();else if(state.page==='Device')await devicePage();else if(state.page==='Users')await usersPage();else if(state.page==='Audio')await audioPage();else if(state.page==='Baby Monitor')await babyMonitorPage();else if(state.page==='Wake Word')await wakePage();else if(state.page==='Simulation')await simulationPage();else if(state.page==='LED & Buttons')await ledPage();else if(state.page==='Network')await networkPage();else if(state.page==='Bluetooth')await bluetoothPage();else if(state.page==='Privacy')await privacyPage();else if(state.page==='Integrations')await integrationsPage();else if(state.page==='System')await systemPage();else if(state.page==='Logs')await logsPage();else aboutPage()}catch(e){errorView(e)}applyCssVars(content);if(state.page==='Overview')state.timer=setTimeout(refreshOverview,5000)}
function showPage(name,updateRoute=true){if(!descriptions[name])name='Overview';if(state.page==='Baby Monitor'&&name!=='Baby Monitor')stopBabyStream();state.page=name;if(updateRoute&&location.pathname!=='/'+pageSlug(name))history.pushState(null,'','/'+pageSlug(name));$$('.nav-item').forEach(x=>x.classList.toggle('active',x.dataset.page===name));$('#page-title').textContent=name;$('#page-subtitle').textContent=descriptions[name];document.title=`${name} · LibreEcho`;document.body.classList.remove('nav-open');render()}
items.forEach(([name,icon],i)=>{const b=document.createElement('button');b.className='nav-item'+(i?'':' active');b.dataset.page=name;b.innerHTML=`<svg><use href="#${icon}"></use></svg><span>${name}</span>`;b.onclick=()=>showPage(name);nav.appendChild(b)});
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
api('/config').then(async c=>{state.csrf=c.csrf_token;await ensureAuth(c);return Promise.all([api('/status'),api('/device'),api('/system/update').catch(()=>({supported:false,check_status:'not-checked'}))])}).then(([,d,ota])=>{updateVersionDisplay(d,ota);return showPage(pageFromLocation(),false)}).catch(error=>{if(state.authMode==='users'||state.authMode==='bearer-token')redirectToLogin();else{document.body.classList.remove('auth-pending');errorView(error)}});
