use crate::{App, Chat, Message, ToolCall};
use crate::tools;
use serde_json::{json, Value};
use std::{fs, io::{BufRead, BufReader}, path::Path, sync::Arc, time::Duration};
use uuid::Uuid;

// Append local history and persist before the next model or tool step.
fn append(app: &App, chat_id: &str, message: Message) {
    let mut state=app.state.lock().unwrap();
    state.chats.iter_mut().find(|chat| chat.id==chat_id).unwrap().messages.push(message);
    app.save(&state);
}

// Apply queued steering at the next boundary; never interrupt an approved filesystem write.
fn steering(app: &App, chat_id: &str) -> bool {
    let mut state=app.state.lock().unwrap();
    let active=state.active.as_mut().unwrap();
    if active.stopped { return false; }
    let pending=std::mem::take(&mut active.pending);
    for text in pending {
        state.chats.iter_mut().find(|chat| chat.id==chat_id).unwrap().messages.push(Message::user(text));
    }
    app.save(&state);
    true
}

// Wait for a matching local approval, a new instruction, cancellation, or a five-minute expiry.
fn approval(app: &App, chat_id: &str, call: &ToolCall, preview: Value) -> Result<(), String> {
    let id=Uuid::new_v4().to_string();
    {
        let mut state=app.state.lock().unwrap();
        let active=state.active.as_mut().unwrap();
        if active.stopped || !active.pending.is_empty() { return Err("Action skipped because the user stopped or updated the instructions.".into()); }
        active.approval=Some((id.clone(),None));
    }
    app.emit(chat_id,json!({"type":"approval","id":id,"tool":call.name,"preview":preview}));
    let deadline=std::time::Instant::now()+Duration::from_secs(300);
    let mut state=app.state.lock().unwrap();
    let accepted=loop {
        let active=state.active.as_ref().unwrap();
        if active.stopped || !active.pending.is_empty() || std::time::Instant::now()>deadline { break false; }
        if let Some(decision)=active.approval.as_ref().and_then(|value|value.1) { break decision; }
        state=app.wake.wait_timeout(state,Duration::from_secs(1)).unwrap().0;
    };
    state.active.as_mut().unwrap().approval=None;
    drop(state);
    app.emit(chat_id,json!({"type":"approval_done","id":id,"accepted":accepted}));
    if accepted { Ok(()) } else { Err("The action was declined, cancelled, or superseded by new instructions.".into()) }
}

// Execute the typed local tool contract; every shell command and file write has a preview.
fn execute(app: &App, chat: &Chat, call: &ToolCall) -> Result<Value, String> {
    let arguments: Value=serde_json::from_str(&call.arguments).map_err(|_| "Invalid tool arguments.")?;
    let workspace=Path::new(&chat.workspace);
    let stopped=|| app.state.lock().unwrap().active.as_ref().is_none_or(|active|active.stopped);
    if stopped() { return Err("User stopped the task.".into()); }
    let text=|key: &str| arguments[key].as_str().map(String::from).ok_or_else(||format!("Missing {key}"));
    match call.name.as_str() {
        "diagnostics"=>tools::diagnose(&text("section")?,stopped),
        "list_directory"=>{
            let path=tools::resolve(workspace,&app.private,&text("path")?)?;
            let entries: Result<Vec<_>,_>=fs::read_dir(path).map_err(|error|error.to_string())?.take(128).map(|entry|entry.map(|value|value.file_name().to_string_lossy().into_owned())).collect();
            Ok(json!({"entries":entries.map_err(|error|error.to_string())?,"limit":128}))
        },
        "read_file"=>Ok(json!({"text":tools::read_text(&tools::resolve(workspace,&app.private,&text("path")?)?)?})),
        "write_file"=>{
            let name=text("path")?;
            let path=tools::resolve(workspace,&app.private,&name)?;
            let content=text("content")?;
            if content.len()>16384 { return Err("File exceeds the edit limit.".into()); }
            let original=if path.exists() { Some(tools::read_text(&path)?) } else { None };
            approval(app,&chat.id,call,json!({"path":path,"before":original,"after":content,"note":"Only this file change has a backup. Other operations may be irreversible."}))?;
            if tools::resolve(workspace,&app.private,&name)?!=path { return Err("The target path changed after approval.".into()); }
            tools::write_text(&path,original.as_deref(),&content,&app.private)
        },
        "run_command"=>{
            let command=text("command")?;
            if command.len()>8000 { return Err("Command exceeds the limit.".into()); }
            approval(app,&chat.id,call,json!({"command":command,"workspace":chat.workspace,"note":"Runs with this Slax AI process's permissions. Shell and disk changes cannot be automatically undone."}))?;
            tools::command("/bin/sh",&["-c",&command],workspace,stopped)
        },
        _=>Err("Unknown local tool.".into())
    }
}

// Stream normalized gateway events; do not retry paid requests under a new identifier.
fn generate(app: &App, chat: &Chat, message_id: &str) -> Result<Message, String> {
    let (backend,token)={let state=app.state.lock().unwrap();(state.backend.clone(),state.token.clone())};
    if token.is_empty() { return Err("Sign in before using Slax AI.".into()); }
    let instructions=format!("{}\nSelected workspace: {}\nLocal operating system: Linux.\nDo not read the private Slax AI state directory.",include_str!("../knowledge.md"),chat.workspace);
    let mut response=app.http.post(format!("{backend}/v1/generate")).header("Authorization",format!("Bearer {token}")).header("Idempotency-Key",Uuid::new_v4().to_string())
        .send_json(json!({"instructions":instructions,"messages":chat.messages})).map_err(|error|error.to_string())?;
    if !response.status().is_success() {
        let error: Value=response.body_mut().read_json().map_err(|error|error.to_string())?;
        return Err(error["error"].as_str().unwrap_or("The server rejected the request.").into());
    }
    let mut reader=BufReader::new(response.body_mut().as_reader());
    let mut line=String::new();
    let mut result=None;
    loop {
        if app.state.lock().unwrap().active.as_ref().is_none_or(|active|active.stopped) { return Err("Task stopped. The server will settle any in-flight model usage.".into()); }
        line.clear();
        if reader.read_line(&mut line).map_err(|error|error.to_string())?==0 { break; }
        if line.len()>1000000 { return Err("Server event exceeded its size limit.".into()); }
        if !line.starts_with("data: ") { continue; }
        let event: Value=serde_json::from_str(line[6..].trim()).map_err(|error|error.to_string())?;
        match event["type"].as_str().unwrap_or("") {
            "text"|"reasoning"=>app.emit(&chat.id,json!({"type":event["type"],"id":message_id,"delta":event["delta"]})),
            "status"=>app.emit(&chat.id,event),
            "result"=>{
                app.emit(&chat.id,json!({"type":"balance","account":event["account"]}));
                result=Some(serde_json::from_value(event["message"].clone()).map_err(|error|error.to_string())?);
            },
            "error"=>{
                app.emit(&chat.id,json!({"type":"balance","account":event["account"]}));
                return Err(event["message"].as_str().unwrap_or("AI request failed.").into());
            },
            _=>{}
        }
    }
    result.ok_or_else(|| "Connection ended before a completed response arrived. Check credits before retrying.".into())
}

// Complete a conversation through bounded model/tool steps, with steering and cancellation.
pub fn run(app: Arc<App>,chat_id: String) {
    let outcome=(|| -> Result<(),String> {
        for _ in 0..30 {
            if !steering(&app,&chat_id) { return Err("Task stopped.".into()); }
            let chat=app.state.lock().unwrap().chats.iter().find(|chat|chat.id==chat_id).unwrap().clone();
            let message_id=Uuid::new_v4().to_string();
            app.emit(&chat_id,json!({"type":"message_start","id":message_id}));
            let message=generate(&app,&chat,&message_id)?;
            app.emit(&chat_id,json!({"type":"message_done","id":message_id,"text":message.text}));
            append(&app,&chat_id,message.clone());
            if message.tool_calls.is_empty() {
                if app.state.lock().unwrap().active.as_ref().unwrap().pending.is_empty() { return Ok(()); }
                continue;
            }
            for call in message.tool_calls {
                app.emit(&chat_id,json!({"type":"tool_start","id":call.id,"name":call.name,"arguments":call.arguments}));
                let updated=app.state.lock().unwrap().active.as_ref().is_some_and(|active|active.stopped || !active.pending.is_empty());
                let result=if updated { Err("Action skipped because the user stopped or updated the instructions.".into()) } else { execute(&app,&chat,&call) };
                let output=result.unwrap_or_else(|error|json!({"error":error}));
                let mut text=output.to_string();
                if text.len()>24000 { text=format!("{}\n[Tool output truncated]",text.chars().take(12000).collect::<String>()); }
                app.emit(&chat_id,json!({"type":"tool_done","id":call.id,"output":text}));
                append(&app,&chat_id,Message {role:"tool".into(),text,call_id:Some(call.id),tool_calls:vec![],provider_state:vec![]});
            }
        }
        Err("Reached the 30-step task limit. Review the results and send another instruction to continue.".into())
    })();
    if let Err(error)=outcome { app.emit(&chat_id,json!({"type":"error","message":error})); }
    let continue_work;
    {
        let mut state=app.state.lock().unwrap();
        // Preserve steering received just as a response finishes, so it is not lost on retry.
        let active=state.active.take();
        let pending=active.as_ref().map(|active|active.pending.clone()).unwrap_or_default();
        continue_work=!pending.is_empty() && active.as_ref().is_some_and(|active|!active.stopped);
        let chat=state.chats.iter_mut().find(|chat|chat.id==chat_id).unwrap();
        for text in pending { chat.messages.push(Message::user(text)); }
        if continue_work {let mut active=active.unwrap();active.pending.clear();state.active=Some(active);}
        app.save(&state);
    }
    if continue_work {std::thread::spawn(move ||run(app,chat_id));return;}
    app.emit(&chat_id,json!({"type":"finished"}));
}
