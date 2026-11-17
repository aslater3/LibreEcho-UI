/*
 * Interface language (LibreEcho-UI control centre).
 *
 * Message catalogs live in one file per language (web/js/i18n-<code>.js). Each
 * one calls LeI18n.register(code, name, catalog); English is the source
 * catalog and the fallback for any key a language does not translate yet.
 *
 * The choice is remembered per browser in localStorage. With no saved choice
 * the first matching browser language is used, otherwise English. Static
 * markup opts in with data-i18n (text) and data-i18n-aria (aria-label); script
 * code calls LeI18n.t(key). Cosmetic only: no failure here blocks a page.
 */
(function(){
 'use strict';
 var KEY='libreecho-lang';
 var DEFAULT='en';
 var languages=[];
 var catalogs={};
 function store(key,value){try{localStorage.setItem(key,value)}catch(e){}}
 function hint(){try{return localStorage.getItem(KEY)}catch(e){return null}}
 function known(code){return catalogs.hasOwnProperty(code)}
 function browserLanguage(){
  var nav=typeof navigator!=='undefined'?navigator:null;
  var list=(nav&&nav.languages&&nav.languages.length)?nav.languages:[nav&&nav.language];
  for(var i=0;i<list.length;i++){var base=String(list[i]||'').toLowerCase().split('-')[0];if(known(base))return base}
  return DEFAULT;
 }
 function clearSaved(){try{localStorage.removeItem(KEY)}catch(e){}}
 function current(){var saved=hint();return known(saved)?saved:browserLanguage()}
 function lookup(code,key){var table=catalogs[code];return table&&Object.prototype.hasOwnProperty.call(table,key)?table[key]:undefined}
 function t(key){var value=lookup(current(),key);if(value===undefined)value=lookup(DEFAULT,key);return value===undefined?key:value}
 function apply(){
  if(typeof document==='undefined'||!document.documentElement)return;
  document.documentElement.lang=current();
  if(!document.querySelectorAll)return;
  var texts=document.querySelectorAll('[data-i18n]');
  for(var i=0;i<texts.length;i++)texts[i].textContent=t(texts[i].getAttribute('data-i18n'));
  var labels=document.querySelectorAll('[data-i18n-aria]');
  for(var j=0;j<labels.length;j++)labels[j].setAttribute('aria-label',t(labels[j].getAttribute('data-i18n-aria')));
 }
 var LeI18n={
  register:function(code,name,catalog){
   if(!code||!catalog||typeof catalog!=='object')return;
   catalogs[code]=catalog;
   if(!languages.some(function(l){return l.code===code}))languages.push({code:code,name:String(name||code)});
   apply();
  },
  languages:function(){return languages.slice()},
  current:current,
  t:t,
  apply:apply,
  /* '' clears the saved choice, so the browser language applies again. */
  set:function(code){if(code==='')return clearSaved();if(known(code))store(KEY,code)},
  /* The saved choice only, so a settings control can show "follow browser". */
  saved:function(){var saved=hint();return known(saved)?saved:''}
 };
 window.LeI18n=LeI18n;
 if(typeof document!=='undefined'&&document.readyState==='loading'&&document.addEventListener)document.addEventListener('DOMContentLoaded',apply);
})();
