use crate::{config::Config,error::{Error,Result},protocol::Conversation};
use futures_util::StreamExt;
use serde::{Deserialize,Serialize};
use serde_json::{Value,json};
use std::collections::BTreeMap;
use tokio::sync::mpsc::UnboundedSender;

#[derive(Clone,Copy,Debug,Serialize,Deserialize)]
pub struct Usage {pub input_tokens:u64,pub cached_tokens:u64,pub output_tokens:u64}
pub struct Failure {pub error:Error,pub accepted:bool,pub usage:Option<Usage>}

// Convert canonical history only at the provider boundary; model choices remain server-owned.
pub fn payload(conversation:&Conversation,config:&Config) -> Value {
    if config.provider=="openai-responses" {
        let mut input=vec![];
        for message in &conversation.messages {
            if message.role=="tool" {input.push(json!({"type":"function_call_output","call_id":message.call_id,"output":message.text}));continue;}
            input.extend(message.provider_state.clone());
            if !message.text.is_empty() {input.push(json!({"role":message.role,"content":message.text}));}
            for call in &message.tool_calls {input.push(json!({"type":"function_call","call_id":call.id,"name":call.name,"arguments":call.arguments}));}
        }
        let tools:Vec<_>=crate::protocol::tools().into_iter().map(|mut tool| {tool["type"]=json!("function");tool["strict"]=json!(true);tool}).collect();
        return json!({"model":config.model,"instructions":conversation.instructions,"input":input,"tools":tools,"parallel_tool_calls":false,
            "store":false,"stream":true,"max_output_tokens":config.max_output,"service_tier":"default",
            "reasoning":{"effort":config.effort,"summary":"auto"},"include":["reasoning.encrypted_content"]});
    }
    let mut messages=vec![json!({"role":"system","content":conversation.instructions})];
    for message in &conversation.messages {
        let mut item=json!({"role":message.role,"content":if message.text.is_empty() {Value::Null} else {json!(message.text)}});
        if message.role=="tool" {item["tool_call_id"]=json!(message.call_id);}
        if !message.tool_calls.is_empty() {item["tool_calls"]=json!(message.tool_calls.iter().map(|call|json!({"id":call.id,"type":"function","function":{"name":call.name,"arguments":call.arguments}})).collect::<Vec<_>>());}
        messages.push(item);
    }
    let tools:Vec<_>=crate::protocol::tools().into_iter().map(|tool|json!({"type":"function","function":tool})).collect();
    json!({"model":config.model,"messages":messages,"tools":tools,"parallel_tool_calls":false,"stream":true,"stream_options":{"include_usage":true},"max_tokens":config.max_output})
}

// At most one token per serialized UTF-8 byte plus framing bounds the pre-dispatch reservation.
pub fn reservation(payload:&Value,config:&Config) -> Result<i64> {
    let input=serde_json::to_vec(payload).unwrap().len() as u64+4096;
    let long=input>config.long_threshold;
    let input_rate=config.input_price*if long {config.long_input} else {1.0};
    let output_rate=config.output_price*if long {config.long_output} else {1.0};
    money((input as f64*input_rate+config.max_output as f64*output_rate)*config.multiplier)
}

// Rates expressed as dollars per million tokens also equal microdollars per token.
pub fn charge(usage:Usage,config:&Config) -> Result<i64> {
    if usage.cached_tokens>usage.input_tokens || usage.input_tokens>9007199254740991 || usage.output_tokens>config.max_output {return Err(Error::new(502,"Provider returned invalid usage."));}
    let long=usage.input_tokens>config.long_threshold;
    let input=(usage.input_tokens-usage.cached_tokens) as f64*config.input_price+usage.cached_tokens as f64*config.cached_price;
    let input_multiplier=if long {config.long_input} else {1.0};
    let output_rate=config.output_price*if long {config.long_output} else {1.0};
    money((input*input_multiplier+usage.output_tokens as f64*output_rate)*config.multiplier)
}

// Reject unrepresentable monetary values instead of overflowing a stored balance.
fn money(amount:f64) -> Result<i64> {
    if !amount.is_finite() || !(0.0..=9007199254740991.0).contains(&amount) {return Err(Error::new(502,"Invalid request cost."));}
    Ok(amount.ceil() as i64)
}

#[derive(Default)]
pub struct Decoder {buffer:Vec<u8>}
impl Decoder {
    // Frame bytes before decoding UTF-8 so fragmented international characters remain intact.
    pub fn push(&mut self,bytes:&[u8]) -> Result<Vec<Value>> {
        self.buffer.extend_from_slice(bytes);
        let mut events=vec![];
        loop {
            let lf=self.buffer.windows(2).position(|w|w==b"\n\n").map(|p|(p,2));
            let crlf=self.buffer.windows(4).position(|w|w==b"\r\n\r\n").map(|p|(p,4));
            let boundary=match (lf,crlf) {(Some(a),Some(b))=>Some(if a.0<b.0 {a} else {b}),(a,b)=>a.or(b)};
            let Some((end,length))=boundary else {break;};
            if end>2000000 {return Err(Error::new(502,"Provider event exceeded the size limit."));}
            let frame=String::from_utf8(self.buffer.drain(..end+length).collect()).map_err(|_|Error::new(502,"Invalid provider text encoding."))?;
            let data=frame.lines().filter_map(|line|line.strip_prefix("data:").map(str::trim_start)).collect::<Vec<_>>().join("\n");
            if !data.is_empty() && data!="[DONE]" {events.push(serde_json::from_str(&data).map_err(|_|Error::new(502,"Invalid provider stream event."))?);}
        }
        if self.buffer.len()>2000000 {return Err(Error::new(502,"Provider event exceeded the size limit."));}
        Ok(events)
    }
    // Trailing comments are harmless; incomplete data frames cannot confirm billable completion.
    pub fn finish(&self) -> Result<()> {
        let remainder=std::str::from_utf8(&self.buffer).map_err(|_|Error::new(502,"Incomplete provider text encoding."))?.trim();
        if !remainder.is_empty() && !remainder.starts_with(':') {return Err(Error::new(502,"Provider stream ended in an incomplete frame."));}
        Ok(())
    }
}

// Read checked integer usage from either provider contract, including cached and reasoning tokens.
fn usage(value:&Value,responses:bool) -> Result<Usage> {
    let integer=|key:&str|value[key].as_u64().ok_or_else(||Error::new(502,"Provider returned invalid usage."));
    let (input,output,details)=if responses {("input_tokens","output_tokens","input_tokens_details")} else {("prompt_tokens","completion_tokens","prompt_tokens_details")};
    let cached=&value[details]["cached_tokens"];
    let cached_tokens=if cached.is_null() {0} else {cached.as_u64().ok_or_else(||Error::new(502,"Provider returned invalid usage."))?};
    Ok(Usage {input_tokens:integer(input)?,cached_tokens,output_tokens:integer(output)?})
}

// Continue an accepted request even if its UI stream closes; upstream errors never expose raw bodies.
pub async fn generate(client:&reqwest::Client,config:&Config,payload:&Value,events:&UnboundedSender<Value>) -> std::result::Result<Value,Failure> {
    let endpoint=if config.provider=="openai-responses" {"responses"} else {"chat/completions"};
    let response=client.post(format!("{}/{endpoint}",config.base_url)).bearer_auth(&config.api_key).header("Accept","text/event-stream")
        .timeout(std::time::Duration::from_millis(config.timeout_ms)).json(payload).send().await
        .map_err(|_|Failure {error:Error::new(502,"AI provider connection failed. Usage could not be confirmed."),accepted:true,usage:None})?;
    if !response.status().is_success() {
        return Err(Failure {error:Error::new(502,if response.status().as_u16()==429 {"AI provider is busy. Try again shortly."} else {"AI provider rejected the request. Please contact support."}),accepted:false,usage:None});
    }
    let result=read_response(response,config,events).await;
    result.map_err(|(error,usage)|Failure {error,accepted:true,usage})
}

// Normalize provider text, tool calls and opaque reasoning into the existing native-client API.
async fn read_response(response:reqwest::Response,config:&Config,events:&UnboundedSender<Value>) -> std::result::Result<Value,(Error,Option<Usage>)> {
    let responses=config.provider=="openai-responses";
    let mut stream=response.bytes_stream();let mut decoder=Decoder::default();
    let mut final_response=None;let mut confirmed=None;let mut text=String::new();let mut finish_reason=None;
    let mut calls=BTreeMap::<u64,Value>::new();let mut received=0usize;
    while let Some(chunk)=stream.next().await {
        let bytes=chunk.map_err(|_|(Error::new(502,"AI provider stream was interrupted."),confirmed))?;
        received+=bytes.len();
        if received>8000000 {return Err((Error::new(502,"Provider stream exceeded the size limit."),confirmed));}
        let frames=decoder.push(&bytes).map_err(|e|(e,confirmed))?;
        for event in frames {
            if responses {
                match event["type"].as_str().unwrap_or("") {
                    "response.output_text.delta"=>{let _=events.send(json!({"type":"text","delta":event["delta"]}));},
                    "response.reasoning_summary_text.delta"=>{let _=events.send(json!({"type":"reasoning","delta":event["delta"]}));},
                    "response.completed"|"response.incomplete"=>{
                        let value=usage(&event["response"]["usage"],true).map_err(|e|(e,None))?;
                        charge(value,config).map_err(|e|(e,None))?;confirmed=Some(value);final_response=Some(event["response"].clone());
                    },
                    "response.failed"|"error"=>return Err((Error::new(502,"AI provider could not finish the request."),confirmed)),
                    _=>{}
                }
            } else {
                if !event["error"].is_null() {return Err((Error::new(502,"AI provider could not finish the request."),confirmed));}
                let choice=&event["choices"][0];
                if let Some(delta)=choice["delta"]["content"].as_str() {text.push_str(delta);let _=events.send(json!({"type":"text","delta":delta}));}
                if let Some(deltas)=choice["delta"]["tool_calls"].as_array() {
                    for delta in deltas {
                        let index=delta["index"].as_u64().ok_or_else(||(Error::new(502,"Invalid provider tool index."),confirmed))?;
                        if index>7 {return Err((Error::new(502,"Provider returned too many tool calls."),confirmed));}
                        let call=calls.entry(index).or_insert_with(||json!({"id":"","name":"","arguments":""}));
                        if let Some(id)=delta["id"].as_str() {call["id"]=json!(id);}
                        for key in ["name","arguments"] {
                            if let Some(value)=delta["function"][key].as_str() {call[key]=json!(format!("{}{value}",call[key].as_str().unwrap()));}
                        }
                    }
                }
                if let Some(reason)=choice["finish_reason"].as_str() {finish_reason=Some(reason.to_string());}
                if !event["usage"].is_null() {
                    let value=usage(&event["usage"],false).map_err(|e|(e,None))?;charge(value,config).map_err(|e|(e,None))?;confirmed=Some(value);
                }
                if text.len()>2000000 || calls.values().map(|v|v.to_string().len()).sum::<usize>()>2000000 {return Err((Error::new(502,"Provider output exceeded the size limit."),confirmed));}
            }
        }
    }
    decoder.finish().map_err(|e|(e,confirmed))?;
    let confirmed=confirmed.ok_or_else(||(Error::new(502,"AI stream ended before usage was confirmed."),None))?;
    let message=if responses {
        let response=final_response.ok_or_else(||(Error::new(502,"AI provider did not confirm completion."),Some(confirmed)))?;
        if response["status"]!="completed" {return Err((Error::new(502,"The model reached its output limit. Please continue with a shorter task."),Some(confirmed)));}
        let output=response["output"].as_array().ok_or_else(||(Error::new(502,"Invalid model output."),Some(confirmed)))?;
        let text=output.iter().filter(|item|item["type"]=="message").flat_map(|item|item["content"].as_array().into_iter().flatten()).filter(|item|item["type"]=="output_text").filter_map(|item|item["text"].as_str()).collect::<String>();
        let calls:Vec<_>=output.iter().filter(|item|item["type"]=="function_call").map(|item|json!({"id":item["call_id"],"name":item["name"],"arguments":item["arguments"]})).collect();
        json!({"role":"assistant","text":text,"tool_calls":calls,"provider_state":output.iter().filter(|item|item["type"]=="reasoning").collect::<Vec<_>>()})
    } else {
        match finish_reason.as_deref() {
            Some("stop"|"tool_calls")=>{},
            Some("length")=>return Err((Error::new(502,"The provider reached its output limit. Start a shorter task."),Some(confirmed))),
            _=>return Err((Error::new(502,"AI provider did not confirm successful completion."),Some(confirmed)))
        }
        json!({"role":"assistant","text":text,"tool_calls":calls.into_values().collect::<Vec<_>>(),"provider_state":[]})
    };
    if serde_json::from_value::<crate::protocol::Message>(message.clone()).is_err() {return Err((Error::new(502,"Invalid provider tool call."),Some(confirmed)));}
    Ok(json!({"message":message,"usage":confirmed}))
}
