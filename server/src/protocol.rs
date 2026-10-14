use crate::error::{Error,Result};
use serde::{Deserialize,Serialize};
use serde_json::{Value,json};

#[derive(Clone,Debug,Serialize,Deserialize)]
pub struct ToolCall {pub id:String,pub name:String,pub arguments:String}
#[derive(Clone,Debug,Serialize,Deserialize)]
pub struct Message {
    pub role:String,pub text:String,
    #[serde(default,skip_serializing_if="Option::is_none")] pub call_id:Option<String>,
    #[serde(default)] pub tool_calls:Vec<ToolCall>,
    #[serde(default)] pub provider_state:Vec<Value>,
}
#[derive(Clone,Debug,Serialize,Deserialize)]
pub struct Conversation {pub instructions:String,pub messages:Vec<Message>}

// Only the server owns tool definitions; local effects remain in the approved native client.
pub fn tools() -> Vec<Value> {
    vec![
        json!({"name":"diagnostics","description":"Read system information using fixed diagnostic commands. No changes are made.","parameters":{"type":"object","properties":{"section":{"type":"string","enum":["system","disks","network","graphics","services"]}},"required":["section"],"additionalProperties":false}}),
        json!({"name":"list_directory","description":"List a directory within the selected workspace.","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false}}),
        json!({"name":"read_file","description":"Read a UTF-8 text file within the selected workspace.","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false}}),
        json!({"name":"write_file","description":"Propose a UTF-8 file change. The user must approve the displayed diff before it is applied.","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"],"additionalProperties":false}}),
        json!({"name":"run_command","description":"Propose a shell command on the LOCAL computer, within the selected workspace. Every command requires user approval. Use diagnostics for routine system inspection.","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"],"additionalProperties":false}}),
    ]
}

// Normalize client history and discard client-selected generation options before fingerprinting.
pub fn validate(body:Value,max_input:usize) -> Result<Conversation> {
    let mut conversation:Conversation=serde_json::from_value(body).map_err(|_|Error::new(400,"Invalid conversation."))?;
    if conversation.instructions.len()>24000 || conversation.messages.is_empty() || conversation.messages.len()>200 {return Err(Error::new(400,"Invalid conversation."));}
    for message in &mut conversation.messages {
        if !["user","assistant","tool"].contains(&message.role.as_str()) {return Err(Error::new(400,"Invalid message."));}
        if message.role=="tool" && message.call_id.as_ref().is_none_or(|id|id.is_empty()) {return Err(Error::new(400,"Missing tool call ID."));}
        if message.role!="tool" {message.call_id=None;}
        if message.role!="assistant" {message.tool_calls.clear();message.provider_state.clear();}
        if message.tool_calls.len()>8 {return Err(Error::new(400,"Too many tool calls."));}
        message.provider_state=message.provider_state.iter().filter(|item|item["type"]=="reasoning").map(|item|json!({"type":"reasoning","id":item["id"],"summary":item["summary"].as_array().cloned().unwrap_or_default(),"encrypted_content":item["encrypted_content"]})).collect();
    }
    if serde_json::to_vec(&conversation).unwrap().len()>max_input {return Err(Error::new(413,"This conversation is too large. Start a new chat with a short summary."));}
    Ok(conversation)
}
