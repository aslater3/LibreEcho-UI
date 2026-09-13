'use strict';
const fs = require('fs'), vm = require('vm'), assert = require('assert');
function deferred() {
  let resolve, reject;
  const promise = new Promise((ok, fail) => {resolve=ok; reject=fail;});
  return {promise, resolve, reject};
}
function element() {
  const classes = new Set();
  return {innerHTML:'',textContent:'',dataset:{},disabled:false,
    classList:{add:c=>classes.add(c),remove:c=>classes.delete(c),contains:c=>classes.has(c),
      toggle(c,on){if(on===undefined)on=!classes.has(c);if(on)classes.add(c);else classes.delete(c);return on;}},
    style:{setProperty(){}},setAttribute(){},appendChild(){},addEventListener(){},
    querySelectorAll(){return []},closest(){return null;},focus(){}};
}
function storage() {const data=new Map();return {getItem:k=>data.get(k)||null,setItem:(k,v)=>data.set(k,String(v)),removeItem:k=>data.delete(k)};}
const scripts=[...fs.readFileSync('web/index.html','utf8').matchAll(/<script\s+src="(\/js\/[^"?]+)(?:\?[^" ]+)?"/g)].map(m=>m[1]);
assert(scripts.includes('/js/app.js'));
function target(mode) {
  const elements=new Map(), body=element(), config=deferred(), page=deferred(), redirects=[];
  const node=s=>{if(!elements.has(s))elements.set(s,element());return elements.get(s);};
  node('#device-online').innerHTML='<span></span>Connecting…';
  const context={console,URL,TextEncoder,TextDecoder,
    document:{body,querySelector:node,querySelectorAll:()=>[],createElement:element,addEventListener(){}},
    window:{addEventListener(){}},localStorage:storage(),sessionStorage:storage(),
    location:{pathname:'/device',hash:'',host:'fixture',replace:p=>redirects.push(p)},
    history:{pushState(){},replaceState(){}},setTimeout:()=>0,clearTimeout(){},
    confirm:()=>false,prompt:()=>'',
    async fetch(path) {
      if(path==='/api/v1/config')await config.promise;
      if(mode==='connection-fails'&&path==='/api/v1/status')throw new Error('Device unreachable');
      const data=path.endsWith('/config')?{authentication:mode==='unauthenticated'?'users':'development-disabled',csrf_token:'fixture'}:
        path.endsWith('/device')?{os_version:'0.14.0'}:{};
      return {ok:true,status:200,json:async()=>({ok:true,data})};
    }
  };
  vm.createContext(context);
  for(const script of scripts)vm.runInContext(fs.readFileSync('web'+script,'utf8'),context,{filename:script});
  // Keep the actual startup chain, navigation and render; delay only page data.
  context.devicePage=async()=>{await page.promise;node('#content').innerHTML='Device page ready';};
  return {body,node,config,page,redirects};
}
const flush=()=>new Promise(setImmediate);
(async()=>{
  for(const mode of ['success','render-fails','connection-fails','unauthenticated']) {
    const t=target(mode);
    assert(t.body.classList.contains('auth-pending'),'shell exposed before config/auth');
    t.config.resolve();await flush();
    if(mode==='unauthenticated') {
      assert(t.redirects.includes('/login'));
      assert(t.body.classList.contains('auth-pending'),'unauthenticated shell exposed');
      continue;
    }
    if(mode!=='connection-fails') {
      assert(t.body.classList.contains('auth-pending'),'shell exposed before first render completed');
      if(mode==='render-fails')t.page.reject(new Error('Initial page failed'));
      else t.page.resolve();
      await flush();
    }
    assert(!t.body.classList.contains('auth-pending'),'startup never revealed its final state');
    if(mode==='success')assert.equal(t.node('#content').innerHTML,'Device page ready');
    else {
      assert(t.node('#content').innerHTML.includes('Unable to load this section'));
      assert(t.node('#device-online').innerHTML.includes('Unavailable'));
      assert(!t.node('#device-online').innerHTML.includes('Connecting'));
      assert(t.node('#device-online').classList.contains('unavailable'));
    }
    assert.equal(t.redirects.length,0);
  }
  console.log('startup state: deferred first render, honest failures and authentication gate: ok');
})().catch(error=>{console.error(error);process.exitCode=1;});
