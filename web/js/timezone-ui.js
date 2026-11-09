/* Time zone picker helpers (#371), shared by Device settings and the setup wizard.
   window.LE_TIMEZONES (timezones.js) maps IANA names to the POSIX TZ strings the device stores. */
(function(){
  const table=()=>Array.isArray(window.LE_TIMEZONES)?window.LE_TIMEZONES:[];
  const escape=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  /* "UTC" (the factory default) and "UTC0" (tzdata's spelling) are the same zone. */
  const same=(a,b)=>(a==='UTC'?'UTC0':a)===(b==='UTC'?'UTC0':b);
  function posixFor(name){const row=table().find(r=>r[0]===name);return row?row[1]:''}
  /* The IANA name to show for a stored zone: the saved name when it still matches, else the first name with the same rules. */
  function nameFor(posix,name){if(name&&same(posixFor(name),posix))return name;const row=table().find(r=>same(r[1],posix));return row?row[0]:''}
  function browserName(){try{return Intl.DateTimeFormat().resolvedOptions().timeZone||''}catch(_){return ''}}
  function label(name){return name.replace(/_/g,' ')}
  /* <option>s grouped by region; an unknown stored zone stays selectable as itself. */
  function options(selected,posix){const groups=new Map();for(const [name] of table()){const region=name.includes('/')?name.split('/')[0]:'Other';if(!groups.has(region))groups.set(region,[]);groups.get(region).push(name)}
    let html='';if(!selected&&posix)html+=`<option value="" selected>Custom: ${escape(posix)}</option>`;
    for(const [region,names] of groups)html+=`<optgroup label="${escape(region)}">${names.map(n=>`<option value="${escape(n)}"${n===selected?' selected':''}>${escape(label(n.includes('/')?n.slice(region.length+1):n))}</option>`).join('')}</optgroup>`;
    return html}
  window.LETimezone={posixFor,nameFor,browserName,options,label,same};
})();
