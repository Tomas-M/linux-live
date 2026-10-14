mod engine;
mod tools;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{collections::VecDeque, fs, io::Read, path::PathBuf, process::Command, sync::{Arc, Condvar, Mutex}, thread, time::Duration};
use std::os::unix::{fs::PermissionsExt, io::AsRawFd};
use tiny_http::{Header, Method, Request, Response, Server, StatusCode};
use uuid::Uuid;

#[derive(Clone,Serialize,Deserialize)]
pub struct ToolCall { id:String,name:String,arguments:String }
#[derive(Clone,Serialize,Deserialize)]
pub struct Message {
    role:String,text:String,
    #[serde(default,skip_serializing_if="Option::is_none")] call_id:Option<String>,
    #[serde(default)] tool_calls:Vec<ToolCall>,
    #[serde(default)] provider_state:Vec<Value>,
}
impl Message {
    // User messages and tool results share a provider-independent history format.
    fn user(text:String) -> Self { Self {role:"user".into(),text,call_id:None,tool_calls:vec![],provider_state:vec![]} }
}
#[derive(Clone,Serialize,Deserialize)]
pub struct Chat { id:String,title:String,workspace:String,messages:Vec<Message>,items:Vec<Value> }
#[derive(Default,Serialize,Deserialize)]
pub struct LocalState {
    backend:String,token:String,chats:Vec<Chat>,
    #[serde(skip)] active:Option<Active>,
    #[serde(skip)] account:Value,
    #[serde(skip)] events:VecDeque<Value>,
    #[serde(skip)] sequence:u64,
}
pub struct Active { chat_id:String,stopped:bool,pending:Vec<String>,approval:Option<(String,Option<bool>)> }
pub struct App { state:Mutex<LocalState>,account_lock:Mutex<()>,wake:Condvar,private:PathBuf,capability:String,origin:String,http:ureq::Agent,allow_http:bool }

impl App {
    // Atomic local persistence uses owner-only files; provider tokens never reach browser JavaScript.
    fn save(&self,state:&LocalState) {
        let temporary=self.private.join("state.json.new");
        let result=serde_json::to_vec(state).map_err(std::io::Error::other).and_then(|data|fs::write(&temporary,data)).and_then(|_|fs::rename(&temporary,self.private.join("state.json")));
        if let Err(error)=result { eprintln!("Unable to save Slax AI history: {error}"); }
    }

    // Keep materialized chat cards for reloads while delivering incremental events to the UI.
    fn emit(&self,chat_id:&str,mut event:Value) {
        let mut state=self.state.lock().unwrap();
        state.sequence+=1;
        event["sequence"]=json!(state.sequence);
        event["chat_id"]=json!(chat_id);
        let kind=event["type"].as_str().unwrap_or("").to_string();
        if kind=="balance" { state.account["account"]=event["account"].clone(); }
        if let Some(chat)=state.chats.iter_mut().find(|chat|chat.id==chat_id) {
            let id=event["id"].clone();
            match kind.as_str() {
                "message_start"=>chat.items.push(json!({"kind":"assistant","id":id,"text":"","reasoning":""})),
                "text"|"reasoning"=>if let Some(item)=chat.items.iter_mut().find(|item|item["id"]==id) {
                    let key=if kind=="text" {"text"} else {"reasoning"};
                    let value=format!("{}{}",item[key].as_str().unwrap_or(""),event["delta"].as_str().unwrap_or(""));
                    item[key]=json!(value);
                },
                "message_done"=>if let Some(item)=chat.items.iter_mut().find(|item|item["id"]==id) { item["text"]=event["text"].clone(); },
                "tool_start"=>chat.items.push(json!({"kind":"tool","id":id,"name":event["name"],"arguments":event["arguments"]})),
                "tool_done"=>if let Some(item)=chat.items.iter_mut().find(|item|item["id"]==id) {item["output"]=event["output"].clone();},
                "approval"=>chat.items.push(json!({"kind":"approval","id":id,"tool":event["tool"],"preview":event["preview"]})),
                "approval_done"=>if let Some(item)=chat.items.iter_mut().find(|item|item["id"]==id) {item["accepted"]=event["accepted"].clone();},
                "error"=>chat.items.push(json!({"kind":"error","id":Uuid::new_v4().to_string(),"text":event["message"]})),
                _=>{}
            }
        }
        state.events.push_back(event);
        while state.events.len()>1024 { state.events.pop_front(); }
        if !["text","reasoning","status","balance"].contains(&kind.as_str()) { self.save(&state); }
        self.wake.notify_all();
    }

    // Proxy only the account API; the local bearer credential remains in private native state.
    fn remote(&self,path:&str,body:Option<&Value>) -> Result<Value,String> {
        let (backend,token)={let state=self.state.lock().unwrap();(state.backend.clone(),state.token.clone())};
        if backend.is_empty() { return Err("Set your Slax AI server address first.".into()); }
        let url=format!("{backend}/v1/{path}");
        let mut response=if let Some(body)=body {
            self.http.post(&url).header("Authorization",format!("Bearer {token}")).send_json(body)
        } else {self.http.get(&url).header("Authorization",format!("Bearer {token}")).call()}.map_err(|error|error.to_string())?;
        let status=response.status();
        let mut data: Value=response.body_mut().read_json().map_err(|error|error.to_string())?;
        if !status.is_success() {
            if status.as_u16()==401 {
                let mut state=self.state.lock().unwrap();
                if state.backend==backend && state.token==token {state.token.clear();state.account=Value::Null;self.save(&state);}
            }
            return Err(data["error"].as_str().unwrap_or("The server rejected the request.").into());
        }
        let mut state=self.state.lock().unwrap();
        if state.backend!=backend || state.token!=token {return Err("Account settings changed while this request was running. Try again.".into());}
        if let Some(token)=data["token"].as_str() {state.token=token.to_string();data.as_object_mut().unwrap().remove("token");self.save(&state);}
        if !data["account"].is_null() {state.account=data.clone();}
        Ok(data)
    }
}

// Check the backend address before any credential is sent; plain HTTP is development-only.
fn backend_address(value:&str,allow_http:bool) -> Result<String,String> {
    let value=value.trim().trim_end_matches('/');
    let https=value.starts_with("https://");
    let development=allow_http && (value.starts_with("http://127.0.0.1:") || value.starts_with("http://localhost:"));
    if !(https || development) || value.contains(['@','?','#','\r','\n']) {return Err("Use an HTTPS server address. Local HTTP requires --allow-http.".into());}
    Ok(value.into())
}

// Reply with local API errors without serializing authentication or filesystem internals.
fn json_response(request:Request,value:Value,status:u16) {
    let response=Response::from_string(value.to_string()).with_status_code(StatusCode(status))
        .with_header(Header::from_bytes("Content-Type","application/json; charset=utf-8").unwrap())
        .with_header(Header::from_bytes("Cache-Control","no-store").unwrap());
    let _=request.respond(response);
}

// Restrict local actions to our loopback origin and per-launch capability token.
fn handle(app:Arc<App>,mut request:Request) {
    let header=|name:&'static str| request.headers().iter().find(|header|header.field.equiv(name)).map(|header|header.value.as_str()).unwrap_or("").to_string();
    if format!("http://{}",header("Host"))!=app.origin || (!header("Origin").is_empty() && header("Origin")!=app.origin) {
        return json_response(request,json!({"error":"Invalid local origin."}),403);
    }
    let url=request.url().to_string();
    if request.method()==&Method::Get && ["/","/app.js","/app.css","/icon.svg"].contains(&url.as_str()) {
        let (body,mime)=match url.as_str() {"/app.js"=>(include_str!("../ui/app.js"),"text/javascript"),"/app.css"=>(include_str!("../ui/app.css"),"text/css"),"/icon.svg"=>(include_str!("../ui/icon.svg"),"image/svg+xml"),_=>(include_str!("../ui/index.html"),"text/html")};
        let response=Response::from_string(body).with_header(Header::from_bytes("Content-Type",format!("{mime}; charset=utf-8")).unwrap())
            .with_header(Header::from_bytes("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'").unwrap())
            .with_header(Header::from_bytes("Referrer-Policy","no-referrer").unwrap());
        let _=request.respond(response);return;
    }
    if header("Authorization")!=format!("Bearer {}",app.capability) {return json_response(request,json!({"error":"Local authorization required. Open Slax AI from its launcher."}),401);}
    let result=(|| -> Result<Value,String> {
        if request.method()==&Method::Get && url=="/api/state" {
            let state=app.state.lock().unwrap();
            return Ok(json!({"backend":state.backend,"connected":!state.token.is_empty(),"account":state.account,"chats":state.chats,"sequence":state.sequence,
                "active":state.active.as_ref().map(|active|json!({"chat_id":active.chat_id,"stopped":active.stopped,"approval_id":active.approval.as_ref().map(|approval|&approval.0)})),"default_workspace":std::env::var("HOME").unwrap_or_else(|_|"/".into()),"permissions":if unsafe {libc::geteuid()}==0 {"root"} else {"user"}}));
        }
        if request.method()==&Method::Get && url.starts_with("/api/events?") {
            let after=url.split("after=").nth(1).and_then(|value|value.parse::<u64>().ok()).unwrap_or(0);
            let state=app.state.lock().unwrap();
            return Ok(json!({"events":state.events.iter().filter(|event|event["sequence"].as_u64().unwrap_or(0)>after).collect::<Vec<_>>(),"sequence":state.sequence}));
        }
        if request.method()!=&Method::Post {return Err("Not found.".into());}
        let mut bytes=Vec::new();
        request.as_reader().take(1000001).read_to_end(&mut bytes).map_err(|error|error.to_string())?;
        if bytes.len()>1000000 {return Err("Request is too large.".into());}
        let body: Value=serde_json::from_slice(&bytes).map_err(|_|"Invalid JSON.")?;
        if url=="/api/backend" {
            let _account=app.account_lock.lock().unwrap();
            let backend=backend_address(body["url"].as_str().unwrap_or(""),app.allow_http)?;
            let mut state=app.state.lock().unwrap();
            if state.active.is_some() {return Err("Stop the current task before changing servers.".into());}
            if backend!=state.backend {state.backend=backend;state.token.clear();state.account=Value::Null;app.save(&state);}
            return Ok(json!({"ok":true}));
        }
        if url=="/api/account" {return app.remote("account",None);}
        if url=="/api/checkout" {return app.remote("checkout",Some(&body));}
        if url.starts_with("/api/auth/") {
            let _account=app.account_lock.lock().unwrap();
            let operation=url.trim_start_matches("/api/auth/");
            if !["register","login","verify","resend","forgot","reset","logout"].contains(&operation) {return Err("Unknown account operation.".into());}
            if app.state.lock().unwrap().active.is_some() {return Err("Stop the current task before changing accounts.".into());}
            let result=app.remote(&format!("auth/{operation}"),Some(&body))?;
            if operation=="logout" {let mut state=app.state.lock().unwrap();state.token.clear();state.account=Value::Null;app.save(&state);}
            return Ok(result);
        }
        if url=="/api/chat" {
            let _account=app.account_lock.lock().unwrap();
            let text=body["text"].as_str().unwrap_or("").trim().to_string();
            if text.is_empty() || text.len()>16000 {return Err("Enter a message under 16 KB.".into());}
            let mut state=app.state.lock().unwrap();
            if state.token.is_empty() {return Err("Sign in before starting a chat.".into());}
            if let Some(active)=state.active.as_mut() {
                if body["chat_id"]!=active.chat_id {return Err("Another chat is running. Stop it first.".into());}
                if active.stopped {return Err("The current task is stopping. Try again shortly.".into());}
                active.pending.push(text.clone());
                let chat_id=active.chat_id.clone();
                state.chats.iter_mut().find(|chat|chat.id==chat_id).unwrap().items.push(json!({"kind":"user","id":Uuid::new_v4().to_string(),"text":text}));
                app.save(&state);app.wake.notify_all();
                return Ok(json!({"chat_id":chat_id,"steered":true}));
            }
            let supplied=body["chat_id"].as_str().unwrap_or("");
            let chat_id=if state.chats.iter().any(|chat|chat.id==supplied) { supplied.to_string() } else {
                let workspace=fs::canonicalize(body["workspace"].as_str().unwrap_or("/root")).map_err(|_|"Select an existing workspace directory.")?;
                if !workspace.is_dir() || workspace.starts_with(&app.private) {return Err("Select a directory outside private Slax AI state.".into());}
                let id=Uuid::new_v4().to_string();
                state.chats.push(Chat {id:id.clone(),title:text.chars().take(48).collect(),workspace:workspace.to_string_lossy().into_owned(),messages:vec![],items:vec![]});
                id
            };
            let chat=state.chats.iter_mut().find(|chat|chat.id==chat_id).unwrap();
            chat.messages.push(Message::user(text.clone()));
            chat.items.push(json!({"kind":"user","id":Uuid::new_v4().to_string(),"text":text}));
            state.active=Some(Active {chat_id:chat_id.clone(),stopped:false,pending:vec![],approval:None});
            app.save(&state);
            let worker=app.clone();let worker_id=chat_id.clone();
            thread::spawn(move ||engine::run(worker,worker_id));
            return Ok(json!({"chat_id":chat_id,"steered":false}));
        }
        if url=="/api/stop" {
            if let Some(active)=app.state.lock().unwrap().active.as_mut() {active.stopped=true;}
            app.wake.notify_all();return Ok(json!({"ok":true}));
        }
        if url=="/api/approve" {
            let mut state=app.state.lock().unwrap();
            let approval=state.active.as_mut().and_then(|active|active.approval.as_mut()).ok_or("No action is awaiting approval.")?;
            if body["id"]!=approval.0 || !body["accept"].is_boolean() {return Err("Approval no longer matches the pending action.".into());}
            if approval.1.is_some() {return Err("This approval was already answered.".into());}
            approval.1=body["accept"].as_bool();app.wake.notify_all();
            return Ok(json!({"ok":true}));
        }
        if url=="/api/delete" {
            let mut state=app.state.lock().unwrap();
            if state.active.as_ref().is_some_and(|active|body["chat_id"]==active.chat_id) {return Err("Stop this chat before deleting it.".into());}
            state.chats.retain(|chat|body["chat_id"]!=chat.id);app.save(&state);
            return Ok(json!({"ok":true}));
        }
        Err("Not found.".into())
    })();
    match result {Ok(value)=>json_response(request,value,200),Err(error)=>json_response(request,json!({"error":error}),400)}
}

// Launch the existing sandboxed Chromium application window, never a root Chromium instance.
fn open_browser(url:&str) -> Result<(),String> {
    let mut command=if unsafe {libc::geteuid()}==0 {let mut command=Command::new("fbliveapp");command.arg("chromium");command} else {Command::new("chromium")};
    command.arg(format!("--app={url}")).spawn().map(|_|()).map_err(|error|error.to_string())
}

// The shipped program embeds its UI and requires no Rust, Cargo, Python or Node runtime.
fn main() -> Result<(),Box<dyn std::error::Error + Send + Sync>> {
    unsafe {libc::umask(0o077);}
    let args:Vec<String>=std::env::args().collect();
    if args.iter().any(|arg|arg=="--help") {println!("Slax AI 0.1.0\nUsage: slax-ai [--open] [--backend HTTPS_URL] [--state-dir DIR] [--port PORT] [--allow-http]\n--allow-http permits localhost HTTP for development only.");return Ok(());}
    let option=|name:&str| args.iter().position(|arg|arg==name).and_then(|index|args.get(index+1)).cloned();
    let private=option("--state-dir").map(PathBuf::from).unwrap_or_else(||PathBuf::from(std::env::var("XDG_STATE_HOME").unwrap_or_else(|_|format!("{}/.local/state",std::env::var("HOME").unwrap()))).join("slax-ai"));
    fs::create_dir_all(&private)?;fs::set_permissions(&private,fs::Permissions::from_mode(0o700))?;
    let private=fs::canonicalize(private)?;
    let lock=fs::OpenOptions::new().create(true).truncate(false).write(true).open(private.join("lock"))?;
    if unsafe {libc::flock(lock.as_raw_fd(),libc::LOCK_EX | libc::LOCK_NB)}!=0 {
        let runtime:Value=serde_json::from_slice(&fs::read(private.join("runtime.json"))?)?;
        if args.iter().any(|arg|arg=="--open") {open_browser(runtime["url"].as_str().ok_or("Invalid runtime file")?)?;}
        return Ok(());
    }
    let mut state=if private.join("state.json").exists() {serde_json::from_slice::<LocalState>(&fs::read(private.join("state.json"))?)?} else {LocalState::default()};
    // Pending approvals from a previous process are never accepted after restart.
    for chat in &mut state.chats {
        let missing:Vec<String>=chat.messages.iter().flat_map(|message|message.tool_calls.iter()).filter(|call|!chat.messages.iter().any(|message|message.call_id.as_ref()==Some(&call.id))).map(|call|call.id.clone()).collect();
        for call_id in missing {chat.messages.push(Message {role:"tool".into(),text:"The local process stopped before this tool's result was recorded. Inspect the system before retrying any change.".into(),call_id:Some(call_id),tool_calls:vec![],provider_state:vec![]});}
    }
    let allow_http=args.iter().any(|arg|arg=="--allow-http");
    if let Some(backend)=option("--backend").or_else(||std::env::var("SLAX_AI_SERVER").ok()) {let backend=backend_address(&backend,allow_http)?;if backend!=state.backend {state.token.clear();state.backend=backend;}}
    let port=option("--port").unwrap_or("0".into()).parse::<u16>()?;
    let server=Server::http(format!("127.0.0.1:{port}"))?;
    let origin=format!("http://{}",server.server_addr().to_ip().unwrap());
    let capability=format!("{}{}",Uuid::new_v4().simple(),Uuid::new_v4().simple());
    let url=format!("{origin}/#token={capability}");
    let http=ureq::Agent::config_builder().timeout_global(Some(Duration::from_secs(150))).http_status_as_error(false).build().new_agent();
    let app=Arc::new(App {state:Mutex::new(state),account_lock:Mutex::new(()),wake:Condvar::new(),private,capability,origin:origin.clone(),http,allow_http});
    app.save(&app.state.lock().unwrap());
    fs::write(app.private.join("runtime.json"),json!({"url":url,"pid":std::process::id()}).to_string())?;
    println!("Slax AI is listening on {origin}. Open it using slax-ai --open.");
    if args.iter().any(|arg|arg=="--open") {open_browser(&url)?;}
    for request in server.incoming_requests() {let app=app.clone();thread::spawn(move ||handle(app,request));}
    Ok(())
}
