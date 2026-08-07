'use strict';
const form=document.querySelector('#account-form');
const error=document.querySelector('#setup-error');
let csrf='';
function showError(message){error.textContent=message;error.hidden=false}
async function load(){
  try{
    const response=await fetch('/api/v1/config',{headers:{Accept:'application/json'}});
    const body=await response.json();
    if(!response.ok||!body.ok)throw new Error(body.error?.message||'Unable to read device setup state');
    csrf=body.data.csrf_token;
    if(!body.data.bootstrap_required){location.replace('/');return}
  }catch(e){showError(e.message)}
}
form.addEventListener('submit',async event=>{
  event.preventDefault();
  error.hidden=true;
  const username=document.querySelector('#username').value.trim();
  const password=document.querySelector('#password').value;
  const confirm=document.querySelector('#password-confirm').value;
  if(!/^[A-Za-z0-9._-]+$/.test(username)){showError('Choose a valid username.');return}
  if(password.length<8){showError('Choose a password with at least 8 characters.');return}
  if(password!==confirm){showError('The passwords do not match.');return}
  const submit=form.querySelector('button');submit.disabled=true;
  try{
    const response=await fetch('/api/v1/auth/bootstrap',{method:'POST',headers:{Accept:'application/json','Content-Type':'application/json','X-LibreEcho-CSRF':csrf},body:JSON.stringify({username,password,password_confirm:confirm})});
    const body=await response.json();
    if(!response.ok||!body.ok)throw new Error(body.error?.message||'Account creation failed');
    sessionStorage.setItem('libreecho-token',body.data.token);
    sessionStorage.setItem('libreecho-username',body.data.username);
    location.replace('/');
  }catch(e){showError(e.message);submit.disabled=false}
});
load();
