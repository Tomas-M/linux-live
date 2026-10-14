let token=new URLSearchParams(location.hash.slice(1)).get('token') || sessionStorage.getItem('slax-ai-capability');
if (token) sessionStorage.setItem('slax-ai-capability',token);
history.replaceState(null,'',location.pathname);
const element=id=>document.getElementById(id);
let state={chats:[],account:null},selected='',sequence=0,authMode='login',workspace='/root',polling=false;
const escape=text=>String(text ?? '').replace(/[&<>"']/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));

// A launcher reopening a fixed-port tab may navigate within the same document.
window.addEventListener('hashchange',()=>
{
   const capability=new URLSearchParams(location.hash.slice(1)).get('token');
   if (!capability) return;
   token=capability;sessionStorage.setItem('slax-ai-capability',token);
   history.replaceState(null,'',location.pathname);refresh().catch(error=>toast(error.message));
});

// Keep all model text escaped; the small renderer supports readable prose and fenced code.
function markdown(text)
{
   return String(text).split(/```[^\n]*\n([\s\S]*?)```/g).map((part,index)=>index%2 ? '<pre><code>'+escape(part)+'</code></pre>' : '<p>'+escape(part).replace(/\*\*([^*]+)\*\*/g,'<strong>$1</strong>').replace(/`([^`\n]+)`/g,'<code>$1</code>')+'</p>').join('');
}

// Native state owns the remote session token; JavaScript gets only this launch's capability.
async function api(path,body)
{
   const response=await fetch('/api/'+path,{method:body===undefined ? 'GET' : 'POST',headers:{Authorization:'Bearer '+token,'Content-Type':'application/json'},body:body===undefined ? undefined : JSON.stringify(body)});
   const data=await response.json();
   if (!response.ok) throw new Error(data.error || 'Request failed.');
   return data;
}

// A brief status remains available without interrupting the conversation.
function toast(text)
{
   element('toast').textContent=text;element('toast').hidden=false;
   clearTimeout(toast.timer);toast.timer=setTimeout(()=>element('toast').hidden=true,4500);
}

// Refresh a consistent snapshot after account, chat, and configuration changes.
async function refresh()
{
   state=await api('state');sequence=state.sequence;
   render();
}

// Render navigation and account information without exposing provider credentials.
function render()
{
   const account=state.account?.account;
   element('credit-balance').textContent=account ? Number(account.credits).toLocaleString(undefined,{maximumFractionDigits:2})+' credits' : '—';
   element('account-name').textContent=account?.email || 'Your Slax AI account';
   element('account-description').textContent=account ? account.verified ? 'Signed in' : 'Verify your email' : 'Sign in to get started';
   element('connection-label').textContent=state.backend ? 'Local agent ready' : 'Set up your server';
   element('model-label').textContent=state.account?.model || 'Slax AI';
   element('permission-label').textContent=state.permissions==='root' ? 'root' : '';
   element('stop').hidden=!state.active;
   element('new-chat').disabled=Boolean(state.active);
   element('composer').querySelector('.send').disabled=state.connected && !account;
   element('message').placeholder=state.active ? 'Add an instruction while Slax AI works…' : 'Ask Slax AI anything about this computer…';
   element('history').innerHTML=[...state.chats].reverse().map(chat=>'<div class="history-row '+(chat.id===selected ? 'selected' : '')+'"><button data-chat="'+escape(chat.id)+'"><svg><use href="#chat"/></svg><span>'+escape(chat.title)+'</span></button><button class="delete-chat" data-delete="'+escape(chat.id)+'" aria-label="Delete chat">×</button></div>').join('');
   renderChat();
}

// Render approval previews as literal text; no output can inject markup or execute code.
function renderChat()
{
   const chat=state.chats.find(chat=>chat.id===selected),box=element('conversation');
   const openCards=new Set([...box.querySelectorAll('details[open][data-card]')].map(card=>card.dataset.card));
   const atBottom=box.scrollHeight-box.scrollTop-box.clientHeight<100;
   element('welcome').hidden=Boolean(chat);box.hidden=!chat;
   element('workspace-label').textContent=chat?.workspace || workspace;
   if (!chat) return;
   box.innerHTML=chat.items.map(item=>
   {
      if (item.kind==='user') return '<div class="message user"><div class="message-text">'+escape(item.text)+'</div></div>';
      if (item.kind==='assistant')
      {
         const working=state.active?.chat_id===selected && chat.items.at(-1)?.id===item.id;
         if (!item.text && !item.reasoning && !working) return '';
         return '<div class="message"><div class="assistant-label"><svg><use href="#spark"/></svg>Slax AI</div>'+(item.reasoning ? '<details class="reasoning" data-card="reasoning-'+escape(item.id)+'"><summary>Activity summary</summary><p>'+escape(item.reasoning)+'</p></details>' : '')+(item.text || working ? '<div class="message-text">'+(item.text ? markdown(item.text) : '')+'</div>' : '')+'</div>';
      }
      if (item.kind==='error') return '<div class="error-card">'+escape(item.text)+'</div>';
      if (item.kind==='tool')
      {
         let status='interrupted';
         if (item.output!==undefined) status='done';
         else if (state.active?.chat_id===selected) status='running';
         return '<details class="tool-card" data-card="tool-'+escape(item.id)+'"><summary><svg><use href="#terminal"/></svg>'+escape(item.name.replaceAll('_',' '))+'<span class="tool-status">'+status+'</span></summary><pre>'+escape(item.arguments)+'</pre>'+(item.output ? '<pre>'+escape(item.output)+'</pre>' : '')+'</details>';
      }
      if (item.kind==='approval')
      {
         const preview=item.preview;
         const pending=state.active?.approval_id===item.id && item.accepted===undefined;
         let status='interrupted';
         if (item.accepted!==undefined) status=item.accepted ? 'approved' : 'declined';
         return '<div class="approval-card"><div class="approval-heading">'+(item.tool==='write_file' ? 'Review file change' : 'Review command')+'</div><p>'+escape(preview.path || preview.workspace)+'</p>'+(preview.command ? '<pre>'+escape(preview.command)+'</pre>' : '<pre><span class="diff-before">'+escape((preview.before ?? '').split('\n').map(line=>'- '+line).join('\n'))+'</span>\n<span class="diff-after">'+escape(preview.after.split('\n').map(line=>'+ '+line).join('\n'))+'</span></pre>')+'<p>'+escape(preview.note)+'</p>'+(pending ? '<div class="approval-actions"><button data-approve="'+escape(item.id)+'">Approve</button><button data-decline="'+escape(item.id)+'">Decline</button></div>' : '<small>'+status+'</small>')+'</div>';
      }
      return '';
   }).join('');
   box.querySelectorAll('details[data-card]').forEach(card=>card.open=openCards.has(card.dataset.card));
   if (atBottom) box.scrollTop=box.scrollHeight;
}

// Apply streamed events to materialized cards, preserving the user's current chat selection.
function eventUpdate(event)
{
   const chat=state.chats.find(chat=>chat.id===event.chat_id);
   if (event.type==='balance') {state.account={...state.account,account:event.account};return;}
   if (event.type==='finished') {state.active=null;element('activity').textContent='';return;}
   if (event.type==='status') {element('activity').textContent=event.message==='Thinking' ? 'Slax AI is working…' : event.message;return;}
   if (!chat) return;
   const item=chat.items.find(item=>item.id===event.id);
   if (event.type==='message_start') chat.items.push({kind:'assistant',id:event.id,text:'',reasoning:''});
   if (['text','reasoning'].includes(event.type) && item) item[event.type==='text' ? 'text' : 'reasoning']+=event.delta;
   if (event.type==='message_done' && item) item.text=event.text;
   if (event.type==='tool_start') chat.items.push({kind:'tool',id:event.id,name:event.name,arguments:event.arguments});
   if (event.type==='tool_done' && item) item.output=event.output;
   if (event.type==='approval') {chat.items.push({kind:'approval',id:event.id,tool:event.tool,preview:event.preview});state.active.approval_id=event.id;element('activity').textContent='An action needs your approval.';}
   if (event.type==='approval_done' && item) {item.accepted=event.accepted;if (state.active) state.active.approval_id=null;}
   if (event.type==='error') chat.items.push({kind:'error',text:event.message});
}

// Poll incremental local events; the native service consumes the provider's SSE stream.
async function poll()
{
   if (polling) return;
   polling=true;
   try
   {
      const data=await api('events?after='+sequence);
      if (data.sequence<sequence || data.events[0]?.sequence>sequence+1) await refresh();
      else if (data.events.length) {data.events.forEach(eventUpdate);sequence=data.sequence;render();if (data.events.some(event=>event.type==='finished')) await refresh();}
   }
   catch (error) {element('activity').textContent=error.message;}
   finally {polling=false;setTimeout(poll,state.active ? 350 : 1500);}
}

// Account flows share a single form and require email verification before free credits.
function accountDialog(mode='login')
{
   authMode=mode;
   const verified=state.account?.account?.verified;
   if (state.connected && mode==='login') authMode=verified ? 'account' : 'verify';
   const titles={login:'Welcome to Slax AI',register:'Your first step starts here',verify:'Check your email',forgot:'Reset your password',reset:'Choose a new password',account:'Your account'};
   element('auth-title').textContent=titles[authMode];
   element('auth-description').textContent=authMode==='register' ? 'Create an account. Verify your email to receive introductory AI credits while available.' : authMode==='verify' ? 'Enter the six-digit code sent to your email address.' : 'Chats are saved locally; selected context is sent to your Slax AI server and its AI provider.';
   element('auth-form').hidden=authMode==='account';element('signed-in').hidden=authMode!=='account';
   element('email-label').hidden=['verify','account'].includes(authMode);element('email').required=!element('email-label').hidden;
   element('password-label').hidden=['verify','forgot','account'].includes(authMode);element('password').required=!element('password-label').hidden;
   element('password').autocomplete=authMode==='login' ? 'current-password' : 'new-password';
   element('code-label').hidden=!['verify','reset'].includes(authMode);element('code').required=!element('code-label').hidden;
   element('auth-submit').textContent=({login:'Sign in',register:'Create account',verify:'Verify email',forgot:'Send reset code',reset:'Reset password'})[authMode] || '';
   element('auth-switch').hidden=!['login','register','forgot','reset'].includes(authMode);element('auth-switch').textContent=authMode==='login' ? 'New here? Create an account' : 'Back to sign in';
   element('forgot-password').hidden=authMode!=='login';element('resend-code').hidden=authMode!=='verify';element('auth-error').textContent='';
   element('signed-email').textContent=state.account?.account?.email || '';
   if (!element('account-dialog').open) element('account-dialog').showModal();
}

// Sending during an active task queues steering without discarding completed work.
element('composer').addEventListener('submit',async event=>
{
   event.preventDefault();
   if (!state.backend) {element('settings-button').click();return;}
   if (!state.connected || !state.account?.account?.verified) {accountDialog();return;}
   const text=element('message').value.trim();if (!text) return;
   try {const result=await api('chat',{chat_id:selected,text,workspace});selected=result.chat_id;element('message').value='';await refresh();element('conversation').scrollTop=element('conversation').scrollHeight;if (result.steered) toast('Instruction queued for the next agent step.');}
   catch (error) {toast(error.message);}
});
element('message').addEventListener('keydown',event=>{if (event.key==='Enter' && !event.shiftKey && !event.isComposing) {event.preventDefault();element('composer').requestSubmit();}});
element('new-chat').addEventListener('click',()=>{selected='';element('activity').textContent='';render();element('message').focus();});
element('stop').addEventListener('click',async()=>{try {await api('stop',{});element('activity').textContent='Stopping. Current model usage will still be settled.';}catch (error) {toast(error.message);}});
element('history').addEventListener('click',async event=>
{
   const chat=event.target.closest('[data-chat]'),remove=event.target.closest('[data-delete]');
   if (chat) {selected=chat.dataset.chat;render();element('conversation').scrollTop=element('conversation').scrollHeight;}
   if (remove && confirm('Delete this local chat history? Files on the computer will stay unchanged.')) {try {await api('delete',{chat_id:remove.dataset.delete});if (selected===remove.dataset.delete) selected='';await refresh();}catch (error) {toast(error.message);}}
});
element('conversation').addEventListener('click',async event=>
{
   const button=event.target.closest('[data-approve],[data-decline]');if (!button) return;
   button.disabled=true;
   try {await api('approve',{id:button.dataset.approve || button.dataset.decline,accept:Boolean(button.dataset.approve)});}catch (error) {toast(error.message);button.disabled=false;}
});
document.querySelectorAll('[data-prompt]').forEach(button=>button.addEventListener('click',()=>{element('message').value=button.dataset.prompt;element('message').focus();}));
document.querySelectorAll('.close-dialog').forEach(button=>button.addEventListener('click',()=>button.closest('dialog').close()));
element('account-button').addEventListener('click',()=>{if (!state.backend) element('settings-button').click();else accountDialog();});
element('auth-switch').addEventListener('click',()=>accountDialog(authMode==='login' ? 'register' : 'login'));
element('forgot-password').addEventListener('click',()=>accountDialog('forgot'));
element('auth-form').addEventListener('submit',async event=>
{
   event.preventDefault();element('auth-submit').disabled=true;
   try
   {
      await api('auth/'+authMode,{email:element('email').value,password:element('password').value,code:element('code').value});
      element('password').value='';element('code').value='';
      if (authMode==='forgot') {accountDialog('reset');toast('If this email is registered, a reset code has been sent.');}
      else {await api('account',{});await refresh();if (state.account.account.verified) {element('account-dialog').close();toast('You are signed in.');}else accountDialog('verify');}
   }
   catch (error) {element('auth-error').textContent=error.message;}
   finally {element('auth-submit').disabled=false;}
});
element('resend-code').addEventListener('click',async()=>{try {await api('auth/resend',{});toast('A new code has been sent.');}catch (error) {element('auth-error').textContent=error.message;}});
element('sign-out').addEventListener('click',async()=>{try {await api('auth/logout',{});await refresh();accountDialog('login');}catch (error) {toast(error.message);}});
element('refresh-account').addEventListener('click',async()=>{try {await api('account',{});await refresh();toast('Credits refreshed.');}catch (error) {toast(error.message);}});
element('settings-button').addEventListener('click',()=>{element('backend-url').value=state.backend || '';element('workspace').value=workspace;element('settings-error').textContent='';element('settings-dialog').showModal();});
element('workspace-button').addEventListener('click',()=>element('settings-button').click());
element('settings-form').addEventListener('submit',async event=>{event.preventDefault();try {await api('backend',{url:element('backend-url').value});workspace=element('workspace').value;await refresh();element('settings-dialog').close();}catch (error) {element('settings-error').textContent=error.message;}});
element('add-credits').addEventListener('click',()=>
{
   if (!state.connected || !state.account?.account?.verified) {accountDialog();return;}
   element('checkout-note').textContent=state.account.checkout_enabled ? 'Checkout opens in a separate tab. Return here after payment and refresh your credits.' : 'Online top-ups have not been enabled by the server operator yet.';
   document.querySelectorAll('[data-cents]').forEach(button=>button.disabled=!state.account.checkout_enabled);
   element('credits-dialog').showModal();
});
document.querySelectorAll('[data-cents]').forEach(button=>button.addEventListener('click',async()=>{button.disabled=true;const tab=window.open('about:blank','_blank');try {const result=await api('checkout',{cents:Number(button.dataset.cents)});if (tab) {tab.opener=null;tab.location=result.url;}else throw new Error('Allow a new tab to open checkout.');}catch (error) {if (tab) tab.close();element('checkout-error').textContent=error.message;}finally {button.disabled=false;}}));
element('credits-refresh').addEventListener('click',async()=>{try {await api('account',{});await refresh();element('credits-dialog').close();toast('Credits refreshed.');}catch (error) {element('checkout-error').textContent=error.message;}});
refresh().then(async()=>{workspace=state.chats.at(-1)?.workspace || state.default_workspace;if (state.connected) {try {await api('account',{});await refresh();}catch {await refresh();accountDialog();}}poll();}).catch(error=>toast(error.message));
