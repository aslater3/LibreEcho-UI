'use strict';

const ref=document.querySelector('#api-reference'),filter=document.querySelector('#endpoint-filter');

let operations=[],apiToken=sessionStorage.getItem('libreecho-token')||'',csrfToken='',configReady=Promise.resolve();

const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

const examples={'POST /api/v1/setup':{hostname:'libreecho',ssid:'Home WiFi',security:'wpa2',password:'password',volume:64,wake_word:'Alexa',wake_sensitivity:68,local_only:true,diagnostic_telemetry:false},'PUT /api/v1/audio':{volume:50,microphone_gain:65,microphone_muted:false,tts_voice:'southern-female'},'POST /api/v1/audio/test':{},'POST /api/v1/audio/announce':{text:'Now playing Don’t Look Back in Anger by Oasis'},'POST /api/v1/audio/announce/stop':{},'PUT /api/v1/led':{r:72,g:216,b:118,brightness:70},'POST /api/v1/led/test':{},'PUT /api/v1/buttons':{short_press:'Start listening',long_press:'Open pairing mode'},'PUT /api/v1/wake-word':{wake_word:'LibreEcho',sensitivity:68},'POST /api/v1/wake-word/test':{},'PUT /api/v1/network':{hostname:'libreecho-dev',api_lan:false,ssh:false},'POST /api/v1/network/wifi/connect':{ssid:'LibreNet-IoT',password:'',security:'wpa2'},'POST /api/v1/network/wifi/disconnect':{},'PUT /api/v1/privacy':{local_only:true,audio_retention:'none',diagnostic_telemetry:false,log_retention_hours:24,crash_reports:false},'PUT /api/v1/integrations/{id}':{enabled:true},'POST /api/v1/system/reboot':{},'POST /api/v1/system/shutdown':{},'POST /api/v1/system/factory-reset':{},'POST /api/v1/diagnostics/export':{},'POST /api/v1/dev/mock':{action:'set-temperature',value:'55'}};

function operationHtml(x){const mutable=x.method!=='GET',destructive=x.path.includes('/system/reboot')||x.path.includes('/system/shutdown')||x.path.includes('/system/factory-reset');
return `<details class="operation ${x.method.toLowerCase()}"><summary><span class="method">${x.method}</span><code>${esc(x.path)}</code><span>${esc(x.summary)}</span><b>⌄</b></summary><div class="operation-body"><p>${esc(x.description||x.summary)}</p>${x.headers.length?`<h3>Required headers</h3><ul>${x.headers.map(h=>`<li><code>${esc(h)}</code></li>`).join('')}</ul>`:''}<div class="try-box"><div class="try-heading"><h3>Try it</h3><span>Runs against this device</span></div><label>Request URL<input class="try-url" value="${esc(x.path)}" spellcheck="false"></label>${mutable?`<label>JSON body<textarea class="try-body" rows="6" spellcheck="false">${esc(JSON.stringify(examples[x.method+' '+x.path]||{},null,2))}</textarea></label>`:''}${destructive?'<label class="confirm-run"><input class="try-confirm" type="checkbox"> I understand this is a destructive device action</label>':''}<div class="try-actions"><button class="execute" data-operation="${x.id}">Execute</button><button class="clear-response" data-operation="${x.id}">Clear</button></div><div class="try-response" id="response-${x.id}" hidden><div><strong>Response</strong><span class="response-status"></span></div><pre></pre></div></div><h3>Responses</h3><div class="responses">${x.responses.map(r=>`<span><b>${r.code}</b> ${esc(r.description)}</span>`).join('')}</div></div></details>`}
function render(){const query=filter.value.trim().toLowerCase(),shown=operations.filter(x=>`${x.method} ${x.path} ${x.summary} ${x.tag}`.toLowerCase().includes(query)),groups=new Map();
shown.forEach(x=>{if(!groups.has(x.tag))groups.set(x.tag,[]);
groups.get(x.tag).push(x)});
ref.innerHTML=[...groups].map(([tag,items])=>`<section class="tag"><h2>${esc(tag)}</h2>${items.map(operationHtml).join('')}</section>`).join('')||'<div class="empty">No matching endpoints.</div>';
document.querySelector('#endpoint-count').textContent=`${shown.length} endpoint${shown.length===1?'':'s'}`;
document.querySelectorAll('.execute').forEach(b=>b.onclick=()=>executeOperation(Number(b.dataset.operation)));
document.querySelectorAll('.clear-response').forEach(b=>b.onclick=()=>{const box=document.querySelector('#response-'+b.dataset.operation);
box.hidden=true;
box.querySelector('pre').textContent=''})}
async function executeOperation(id){await configReady;const op=operations.find(x=>x.id===id),button=document.querySelector(`.execute[data-operation="${id}"]`),box=document.querySelector('#response-'+id),scope=button.closest('.try-box'),url=scope.querySelector('.try-url').value.trim();
if(!url.startsWith('/api/v1')){box.hidden=false;
box.querySelector('.response-status').textContent='Blocked';
box.querySelector('pre').textContent='Only same-device /api/v1 paths are allowed.';
return}if(url.includes('{')){box.hidden=false;
box.querySelector('.response-status').textContent='Path required';
box.querySelector('pre').textContent='Replace path placeholders such as {id} before executing.';
return}const confirmBox=scope.querySelector('.try-confirm');
if(confirmBox&&!confirmBox.checked){box.hidden=false;
box.querySelector('.response-status').textContent='Confirmation required';
box.querySelector('pre').textContent='Tick the destructive-action confirmation before executing.';
return}const headers={Accept:'application/json'};if(apiToken)headers.Authorization='Bearer '+apiToken;
let body;
if(op.method!=='GET'){headers['Content-Type']='application/json';
headers['X-LibreEcho-CSRF']=csrfToken;
if(confirmBox)headers['X-LibreEcho-Confirm']='confirm-device-action';
const editor=scope.querySelector('.try-body');
try{body=JSON.stringify(JSON.parse(editor.value))}catch(e){box.hidden=false;
box.querySelector('.response-status').textContent='Invalid JSON';
box.querySelector('pre').textContent=e.message;
return}}button.disabled=true;
button.textContent='Executing…';
box.hidden=false;
box.querySelector('.response-status').textContent='Waiting';
box.querySelector('pre').textContent='';
try{const response=await fetch(url,{method:op.method,headers,body}),text=await response.text();
let formatted=text;
try{formatted=JSON.stringify(JSON.parse(text),null,2)}catch(_){}box.querySelector('.response-status').textContent=`${response.status} ${response.statusText}`;
box.querySelector('.response-status').className='response-status '+(response.ok?'success':'failure');
box.querySelector('pre').textContent=formatted||'(empty response)'}catch(e){box.querySelector('.response-status').textContent='Request failed';
box.querySelector('.response-status').className='response-status failure';
box.querySelector('pre').textContent=e.message}finally{button.disabled=false;
button.textContent='Execute'}}
configReady=fetch('/api/v1/config').then(r=>r.json()).then(body=>{const status=document.querySelector('#auth-status');if(body.data){csrfToken=body.data.csrf_token||'';if(body.data.authentication==='development-disabled')status.textContent='Development access: authentication is disabled.';else if(apiToken)status.textContent='Session token available from the Control Centre.';else status.innerHTML='Authentication is enabled. <a href="/">Sign in from the Control Centre</a> before executing requests.'}}).catch(()=>{const status=document.querySelector('#auth-status');if(status)status.textContent='Authentication status unavailable.'});
fetch('/openapi.json').then(r=>{if(!r.ok)throw new Error('Specification unavailable');
return r.json()}).then(spec=>{document.querySelector('#api-title').textContent=spec.info.title;
document.querySelector('#api-version').textContent=spec.info.version;
document.querySelector('#api-description').textContent=spec.info.description;
document.querySelector('#base-url').textContent=spec.servers[0].url;
Object.entries(spec.paths).forEach(([path,item])=>Object.entries(item).forEach(([method,op])=>{if(!['get','put','post','delete','patch'].includes(method))return;
operations.push({id:operations.length,method:method.toUpperCase(),path:spec.servers[0].url+(path==='/'?'':path),summary:op.summary||'',description:op.description||'',tag:(op.tags||['Other'])[0],headers:(op.parameters||[]).map(p=>p.$ref?p.$ref.split('/').pop():p.name).filter(Boolean),requestBody:!!op.requestBody,responses:Object.entries(op.responses||{}).map(([code,r])=>({code,description:r.description||'Response'}))})}));
render()}).catch(e=>{ref.innerHTML=`<div class="empty error">${esc(e.message)}</div>`});

filter.addEventListener('input',render);
document.querySelector('#copy-base').onclick=()=>navigator.clipboard.writeText(location.origin+document.querySelector('#base-url').textContent);
