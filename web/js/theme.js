/*
 * Appearance (#188). Loaded synchronously in <head> so the saved theme applies
 * before first paint: the per-browser hint is used at once, then the device's
 * saved choice (public GET /api/v1/config) wins and the hint is rewritten.
 * color_mode is empty until the owner picks one, so a browser keeps its own
 * light/dark choice until then. Cosmetic only; no failure here blocks a page.
 */
(function(){
 'use strict';
 try{
 var root=document.documentElement;
 function store(key,value){try{localStorage.setItem(key,value)}catch(e){}}
 function hint(key){try{return localStorage.getItem(key)}catch(e){return null}}
 function setThemeColor(){
  var meta=document.querySelector&&document.querySelector('meta[name="theme-color"]');
  if(!meta||!document.body||!window.getComputedStyle)return;
  var bg=window.getComputedStyle(document.body).getPropertyValue('--bg').trim();
  if(bg)meta.setAttribute('content',bg);
 }
 function applyMode(mode){
  if(mode!=='light'&&mode!=='dark')return;
  var go=function(){
   if(document.body.classList)document.body.classList.toggle('light',mode==='light');
   var button=document.getElementById&&document.getElementById('theme');
   if(button)button.textContent=mode==='light'?'☾':'☼';
   setThemeColor();
  };
  if(document.body)go();else if(document.addEventListener)document.addEventListener('DOMContentLoaded',go);
 }
 function applyTheme(theme){
  if(theme!=='setup'&&theme!=='classic')return;
  if(!root||!root.setAttribute||!root.removeAttribute)return;
  if(theme==='setup')root.setAttribute('data-ui-theme','setup');else root.removeAttribute('data-ui-theme');
  if(document.body)setThemeColor();
 }
 window.leApplyAppearance=function(prefs){
  if(!prefs)return;
  if(prefs.ui_theme==='setup'||prefs.ui_theme==='classic'){applyTheme(prefs.ui_theme);store('libreecho-ui-theme',prefs.ui_theme)}
  if(prefs.color_mode==='light'||prefs.color_mode==='dark'){applyMode(prefs.color_mode);store('libreecho-theme',prefs.color_mode)}
 };
 applyTheme(hint('libreecho-ui-theme')||'classic');
 if(document.addEventListener)document.addEventListener('DOMContentLoaded',setThemeColor);
 if(window.fetch)fetch('/api/v1/config',{credentials:'same-origin'}).then(function(r){return r.json()}).then(function(body){
  if(body&&body.ok&&body.data)window.leApplyAppearance(body.data);
 }).catch(function(){});
 }catch(e){/* Cosmetic only: never break the page. */}
})();
