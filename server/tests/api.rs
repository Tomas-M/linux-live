use axum::{Router,body::{Body,Bytes},extract::State,http::StatusCode,response::{IntoResponse,Response},routing::post};
use hmac::{Hmac,Mac};
use rusqlite::params;
use serde_json::{Value,json};
use sha2::Sha256;
use slax_ai_server::{App,config::Config,database,providers};
use std::{collections::HashMap,sync::{Arc,Mutex,atomic::{AtomicUsize,Ordering}},time::Duration};
use tokio::{io::{AsyncBufReadExt,AsyncWriteExt,BufReader},net::TcpListener,sync::Notify,task::JoinHandle};

#[derive(Clone,Copy)]
enum Mode {Complete,Rejected,Truncated,Incomplete,Chat,ChatLength,Tool,Agent}
struct Provider {mode:Mode,calls:AtomicUsize,payloads:Mutex<Vec<Value>>,gate:Option<Arc<Notify>>}
struct Fixture {app:Arc<App>,base:String,http:reqwest::Client,provider:Arc<Provider>,mail:Arc<Mutex<Vec<String>>>,servers:Vec<JoinHandle<()>>}

// HTTP and SMTP fixtures are loopback-only; no model, email or payment account is contacted.
impl Fixture {
    async fn new(mode:Mode,settings:&[(&str,&str)],gated:bool) -> Self {
        let mail=Arc::new(Mutex::new(vec![]));
        let smtp=TcpListener::bind("127.0.0.1:0").await.unwrap();let smtp_port=smtp.local_addr().unwrap().port();let mailbox=mail.clone();
        let smtp_task=tokio::spawn(async move {
            loop {
                let (socket,_)=smtp.accept().await.unwrap();let mailbox=mailbox.clone();
                tokio::spawn(async move {
                    let mut socket=BufReader::new(socket);socket.get_mut().write_all(b"220 localhost fixture\r\n").await.unwrap();
                    let mut data=None::<String>;let mut line=String::new();
                    loop {
                        line.clear();if socket.read_line(&mut line).await.unwrap_or(0)==0 {break;}
                        if let Some(message)=data.as_mut() {
                            if line==".\r\n" {mailbox.lock().unwrap().push(data.take().unwrap());socket.get_mut().write_all(b"250 accepted\r\n").await.unwrap();}
                            else {message.push_str(&line);}
                            continue;
                        }
                        let reply=if line.starts_with("EHLO") {"250-localhost\r\n250-SMTPUTF8\r\n250-8BITMIME\r\n250 PIPELINING\r\n"}
                            else if line.starts_with("DATA") {data=Some(String::new());"354 send data\r\n"}
                            else if line.starts_with("QUIT") {socket.get_mut().write_all(b"221 bye\r\n").await.unwrap();break;}
                            else {"250 ok\r\n"};
                        socket.get_mut().write_all(reply.as_bytes()).await.unwrap();
                    }
                });
            }
        });
        let provider=Arc::new(Provider {mode,calls:AtomicUsize::new(0),payloads:Mutex::new(vec![]),gate:gated.then(||Arc::new(Notify::new()))});
        let upstream=TcpListener::bind("127.0.0.1:0").await.unwrap();let upstream_port=upstream.local_addr().unwrap().port();
        let mock=Router::new().route("/{*path}",post(upstream_response)).with_state(provider.clone());
        let upstream_task=tokio::spawn(async move {axum::serve(upstream,mock).await.unwrap();});
        let mut values:HashMap<String,String>=settings.iter().map(|(key,value)|(key.to_string(),value.to_string())).collect();
        values.insert("AI_API_KEY".into(),"synthetic-fixture-key".into());values.insert("AI_BASE_URL".into(),format!("http://127.0.0.1:{upstream_port}/v1"));
        values.insert("SMTP_HOST".into(),"127.0.0.1".into());values.insert("SMTP_PORT".into(),smtp_port.to_string());values.insert("SMTP_STARTTLS".into(),"false".into());
        values.insert("MAIL_FROM".into(),"Slax AI <fixture@example.invalid>".into());
        let mut config=Config::from_values(&values).unwrap();config.stripe_url=format!("http://127.0.0.1:{upstream_port}/v1");
        let app=App::new(config,database::open(std::path::Path::new(":memory:")).unwrap()).unwrap();
        let listener=TcpListener::bind("127.0.0.1:0").await.unwrap();let base=format!("http://{}",listener.local_addr().unwrap());
        let service=slax_ai_server::router(app.clone()).into_make_service_with_connect_info::<std::net::SocketAddr>();
        let api_task=tokio::spawn(async move {axum::serve(listener,service).await.unwrap();});
        Self {app,base,http:reqwest::Client::builder().timeout(Duration::from_secs(10)).build().unwrap(),provider,mail,servers:vec![smtp_task,upstream_task,api_task]}
    }

    // Exercise the complete client-facing route rather than calling authentication internals.
    async fn request(&self,path:&str,body:Option<Value>,token:&str,headers:&[(&str,&str)]) -> (u16,Value) {
        request(&self.http,&self.base,path,body,token,headers).await
    }

    // Extract the synthetic email code from the SMTP message's actual body.
    fn code(&self) -> String {self.mail.lock().unwrap().last().unwrap().split("code is ").nth(1).unwrap().chars().take(6).collect()}

    // Registration only becomes usable after the code received by the fake SMTP server is verified.
    async fn user(&self,email:&str,verify:bool) -> String {
        let (status,value)=self.request("/v1/auth/register",Some(json!({"email":email,"password":"synthetic password 123"})),"",&[]).await;
        assert_eq!(status,200,"{value}");let token=value["token"].as_str().unwrap().to_string();
        if verify {assert_eq!(self.request("/v1/auth/verify",Some(json!({"code":self.code()})),&token,&[]).await.0,200);}
        token
    }

    // Gated model responses make races and disconnect billing deterministic.
    async fn wait_for_dispatch(&self) {
        for _ in 0..200 {if self.provider.calls.load(Ordering::SeqCst)>0 {return;}tokio::time::sleep(Duration::from_millis(5)).await;}
        panic!("Provider was not contacted");
    }
}

impl Drop for Fixture {
    // Fixture listeners and pending mock connections are never left running after a test.
    fn drop(&mut self) {for server in &self.servers {server.abort();}}
}

// Parse real SSE responses with the same decoder used for upstream streams.
async fn request(http:&reqwest::Client,base:&str,path:&str,body:Option<Value>,token:&str,headers:&[(&str,&str)]) -> (u16,Value) {
    let mut request=if let Some(body)=body {http.post(format!("{base}{path}")).json(&body)} else {http.get(format!("{base}{path}"))};
    request=request.bearer_auth(token);for (name,value) in headers {request=request.header(*name,*value);}
    let response=request.send().await.unwrap();let status=response.status().as_u16();
    let streaming=response.headers().get("content-type").unwrap().to_str().unwrap().starts_with("text/event-stream");
    let bytes=response.bytes().await.unwrap();
    let value=if streaming {let mut decoder=providers::Decoder::default();let events=decoder.push(&bytes).unwrap();decoder.finish().unwrap();json!(events)} else {serde_json::from_slice(&bytes).unwrap()};
    (status,value)
}

// A completed provider fixture includes cached input and output usage, including hidden reasoning.
fn completed(status:&str,output:Value) -> Value {
    json!({"type":if status=="completed" {"response.completed"} else {"response.incomplete"},"response":{"status":status,"output":output,"usage":{"input_tokens":100,"input_tokens_details":{"cached_tokens":20},"output_tokens":10}}})
}

// Simulate both provider protocols and terminal failures over an actual HTTP connection.
async fn upstream_response(State(provider):State<Arc<Provider>>,bytes:Bytes) -> Response {
    if !bytes.starts_with(b"{") {
        let query=reqwest::Url::parse(&format!("http://localhost/?{}",String::from_utf8_lossy(&bytes))).unwrap();
        let form:HashMap<String,String>=query.query_pairs().map(|(k,v)|(k.into_owned(),v.into_owned())).collect();
        provider.payloads.lock().unwrap().push(json!(form));
        return axum::Json(json!({"id":"cs_fixture_checkout","url":"https://checkout.stripe.com/c/pay/cs_fixture_checkout"})).into_response();
    }
    let body:Value=serde_json::from_slice(&bytes).unwrap();
    let tool_result=body["input"].as_array().is_some_and(|items|items.iter().any(|item|item["type"]=="function_call_output"));
    provider.payloads.lock().unwrap().push(body);
    provider.calls.fetch_add(1,Ordering::SeqCst);if let Some(gate)=&provider.gate {gate.notified().await;}
    let events=match provider.mode {
        Mode::Rejected=>return (StatusCode::TOO_MANY_REQUESTS,"{}").into_response(),
        Mode::Truncated=>vec![json!({"type":"response.output_text.delta","delta":"Partial"})],
        Mode::Incomplete=>vec![completed("incomplete",json!([]))],
        Mode::Chat|Mode::ChatLength=>vec![
            json!({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call-fixture","function":{"name":"read_file","arguments":"{\"path\":"}}]}}]}),
            json!({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"hello.txt\"}"}}]},"finish_reason":if matches!(provider.mode,Mode::Chat) {"tool_calls"} else {"length"}}]}),
            json!({"choices":[],"usage":{"prompt_tokens":100,"prompt_tokens_details":{"cached_tokens":20},"completion_tokens":10}})
        ],
        Mode::Tool=>vec![completed("completed",json!([
            {"type":"reasoning","id":"reason-fixture","summary":[{"type":"summary_text","text":"Checking the local system."}],"encrypted_content":"synthetic-encrypted-state"},
            {"type":"function_call","call_id":"tool-fixture","name":"diagnostics","arguments":"{\"section\":\"system\"}"}
        ]))],
        Mode::Agent if !tool_result=>vec![completed("completed",json!([{"type":"function_call","call_id":"tool-native-fixture","name":"diagnostics","arguments":"{\"section\":\"system\"}"}]))],
        Mode::Complete|Mode::Agent=>vec![json!({"type":"response.output_text.delta","delta":"Hello 世界"}),completed("completed",json!([{"type":"message","content":[{"type":"output_text","text":"Hello 世界"}]}]))],
    };
    let text=events.iter().map(|event|format!("data: {event}\r\n\r\n")).collect::<String>();
    ([("content-type","text/event-stream")],Body::from(text)).into_response()
}

// A common payload deliberately contains no model options or client-owned tool definitions.
fn conversation() -> Value {json!({"instructions":"You are Slax AI.","messages":[{"role":"user","text":"Inspect this computer."}]})}
// Select the normalized terminal event emitted after settlement.
fn last(events:&Value) -> &Value {events.as_array().unwrap().last().unwrap()}
// Read the authoritative account balance for charge/refund assertions.
fn balance(f:&Fixture,id:i64) -> i64 {database::account(&f.app.db.lock().unwrap(),id).unwrap().balance}

// Email verification grants once and blocks upstream work before verification.
#[tokio::test]
async fn verified_welcome_and_unicode_email() {
    let f=Fixture::new(Mode::Complete,&[],false).await;let token=f.user("新用户@example.com",false).await;
    assert_eq!(balance(&f,1),0);
    assert_eq!(f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","verification-request")]).await.0,403);
    assert_eq!(f.provider.calls.load(Ordering::SeqCst),0);
    assert_eq!(f.request("/v1/auth/verify",Some(json!({"code":"000000"})),&token,&[]).await.0,400);
    for _ in 0..2 {assert_eq!(f.request("/v1/auth/verify",Some(json!({"code":f.code()})),&token,&[]).await.0,200);}
    assert_eq!(balance(&f,1),200000);
    let db=f.app.db.lock().unwrap();let encoded:String=db.query_row("SELECT password FROM users",[],|r|r.get(0)).unwrap();
    assert!(encoded.starts_with("$argon2id$"));
    assert_eq!(db.query_row("SELECT count(*) FROM ledger WHERE reference LIKE 'welcome:%'",[],|r|r.get::<_,i64>(0)).unwrap(),1);
}

// Usage settles once, refunds unused reservation, and ignores client-selected pricing/model options.
#[tokio::test]
async fn authoritative_generation_and_idempotency() {
    let f=Fixture::new(Mode::Complete,&[],false).await;let token=f.user("test@example.com",true).await;let before=balance(&f,1);
    let mut body=conversation();body["model"]=json!("untrusted-model");body["max_output_tokens"]=json!(999999);
    let (status,events)=f.request("/v1/generate",Some(body),&token,&[("Idempotency-Key","completed-request-1")]).await;
    assert_eq!(status,200);assert_eq!(last(&events)["type"],"result");assert_eq!(last(&events)["message"]["text"],"Hello 世界");
    assert_eq!(last(&events)["charged"],524);assert_eq!(balance(&f,1),before-524);
    let (_,replay)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","completed-request-1")]).await;
    assert_eq!(last(&replay)["message"]["text"],"Hello 世界");assert_eq!(f.provider.calls.load(Ordering::SeqCst),1);
    let payloads=f.provider.payloads.lock().unwrap();assert_eq!(payloads[0]["model"],f.app.config.model);assert_eq!(payloads[0]["max_output_tokens"],4096);assert_eq!(payloads[0]["store"],false);assert_eq!(payloads[0]["service_tier"],"default");
    assert_eq!(payloads[0]["tools"].as_array().unwrap().len(),5);
}

// Empty accounts cannot reach the model provider.
#[tokio::test]
async fn insufficient_credit_prevents_dispatch() {
    let f=Fixture::new(Mode::Complete,&[("WELCOME_CREDITS","0")],false).await;let token=f.user("test@example.com",true).await;
    assert_eq!(f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","empty-balance-request")]).await.0,402);
    assert_eq!(f.provider.calls.load(Ordering::SeqCst),0);
}

// Explicit provider rejection returns the entire reservation.
#[tokio::test]
async fn rejected_provider_refunds() {
    let f=Fixture::new(Mode::Rejected,&[],false).await;let token=f.user("test@example.com",true).await;let before=balance(&f,1);
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","rejected-request-1")]).await;
    assert_eq!(last(&events)["type"],"error");assert_eq!(last(&events)["charged"],0);assert_eq!(balance(&f,1),before);
}

// Missing usage retains the reservation and replay cannot obtain another free inference.
#[tokio::test]
async fn truncated_stream_retains_reservation() {
    let f=Fixture::new(Mode::Truncated,&[],false).await;let token=f.user("test@example.com",true).await;let before=balance(&f,1);
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","truncated-request-1")]).await;
    let charge=last(&events)["charged"].as_i64().unwrap();assert!(charge>0);assert_eq!(balance(&f,1),before-charge);
    let (_,again)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","truncated-request-1")]).await;
    assert_eq!(last(&again)["charged"],charge);assert_eq!(f.provider.calls.load(Ordering::SeqCst),1);
}

// Confirmed usage is billed even when the provider reaches its output limit.
#[tokio::test]
async fn incomplete_responses_bill_actual_usage() {
    let f=Fixture::new(Mode::Incomplete,&[],false).await;let token=f.user("test@example.com",true).await;
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","incomplete-request-1")]).await;
    assert_eq!(last(&events)["type"],"error");assert_eq!(last(&events)["charged"],524);
}

// Concurrent requests share an atomic account reservation instead of racing past the balance.
#[tokio::test]
async fn simultaneous_requests_cannot_overspend() {
    let f=Fixture::new(Mode::Complete,&[],true).await;let token=f.user("test@example.com",true).await;
    let http=f.http.clone();let base=f.base.clone();let login=token.clone();
    let first=tokio::spawn(async move {request(&http,&base,"/v1/generate",Some(conversation()),&login,&[("Idempotency-Key","concurrent-request-1")]).await});
    f.wait_for_dispatch().await;
    assert_eq!(f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","concurrent-request-2")]).await.0,409);
    f.provider.gate.as_ref().unwrap().notify_one();assert_eq!(first.await.unwrap().0,200);assert!(balance(&f,1)>=0);
}

// Detached generation confirms and settles usage after the client abandons its response body.
#[tokio::test]
async fn disconnected_client_still_settles() {
    let f=Fixture::new(Mode::Complete,&[],true).await;let token=f.user("test@example.com",true).await;
    let response=f.http.post(format!("{}/v1/generate",f.base)).bearer_auth(&token).header("Idempotency-Key","disconnect-request-1").json(&conversation()).send().await.unwrap();
    assert_eq!(response.status(),StatusCode::OK);f.wait_for_dispatch().await;drop(response);f.provider.gate.as_ref().unwrap().notify_one();
    f.app.tasks.close();tokio::time::timeout(Duration::from_secs(3),f.app.tasks.wait()).await.unwrap();
    let db=f.app.db.lock().unwrap();let row=database::request(&db,"disconnect-request-1").unwrap();assert!(row.finished.is_some());assert_eq!(row.charged,Some(524));
}

// Password recovery revokes old sessions and logout invalidates the replacement.
#[tokio::test]
async fn password_reset_and_logout_revoke_tokens() {
    let f=Fixture::new(Mode::Complete,&[],false).await;let token=f.user("test@example.com",true).await;
    assert_eq!(f.request("/v1/auth/forgot",Some(json!({"email":"test@example.com"})),"",&[]).await.0,200);
    let (status,reset)=f.request("/v1/auth/reset",Some(json!({"email":"test@example.com","password":"new synthetic password 123","code":f.code()})),"",&[]).await;assert_eq!(status,200);
    assert_eq!(f.request("/v1/account",None,&token,&[]).await.0,401);
    assert_eq!(f.request("/v1/auth/login",Some(json!({"email":"test@example.com","password":"wrong synthetic password"})),"",&[]).await.0,401);
    let (status,login)=f.request("/v1/auth/login",Some(json!({"email":"test@example.com","password":"new synthetic password 123"})),"",&[]).await;assert_eq!(status,200);
    assert_ne!(login["token"],reset["token"]);let login=login["token"].as_str().unwrap();
    assert_eq!(f.request("/v1/auth/logout",Some(json!({})),login,&[]).await.0,200);assert_eq!(f.request("/v1/account",None,login,&[]).await.0,401);
}

// Compatible providers return exactly the same tool history contract as Responses.
#[tokio::test]
async fn alternate_provider_normalizes_tools() {
    let f=Fixture::new(Mode::Chat,&[("AI_PROVIDER","chat-completions")],false).await;let token=f.user("test@example.com",true).await;
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","chat-provider-request")]).await;
    assert_eq!(last(&events)["message"]["tool_calls"],json!([{"id":"call-fixture","name":"read_file","arguments":"{\"path\":\"hello.txt\"}"}]));
    assert_eq!(last(&events)["charged"],524);let payloads=f.provider.payloads.lock().unwrap();assert_eq!(payloads[0]["stream_options"]["include_usage"],true);
}

// Chat output limits consume the following usage frame instead of forfeiting its confirmed charge.
#[tokio::test]
async fn chat_output_limit_bills_confirmed_usage() {
    let f=Fixture::new(Mode::ChatLength,&[("AI_PROVIDER","chat-completions")],false).await;let token=f.user("test@example.com",true).await;
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","chat-length-request")]).await;
    assert_eq!(last(&events)["type"],"error");assert_eq!(last(&events)["charged"],524);
}

// Encrypted reasoning and local tool results remain valid across another model request.
#[tokio::test]
async fn reasoning_and_tool_results_continue() {
    let f=Fixture::new(Mode::Tool,&[],false).await;let token=f.user("test@example.com",true).await;
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","tool-history-request")]).await;
    let mut body=conversation();body["messages"].as_array_mut().unwrap().push(last(&events)["message"].clone());
    body["messages"].as_array_mut().unwrap().push(json!({"role":"tool","call_id":"tool-fixture","text":"Local diagnostic result."}));
    assert_eq!(f.request("/v1/generate",Some(body),&token,&[("Idempotency-Key","tool-continue-request")]).await.0,200);
    let payloads=f.provider.payloads.lock().unwrap();assert_eq!(payloads[1]["input"][1]["encrypted_content"],"synthetic-encrypted-state");assert_eq!(payloads[1]["input"][3]["type"],"function_call_output");
}

// A UTC daily grant pool is shared across accounts without adding persisted grant-status flags.
#[tokio::test]
async fn daily_trial_pool_is_global() {
    let f=Fixture::new(Mode::Complete,&[("WELCOME_DAILY_LIMIT","30")],false).await;
    for email in ["one@example.com","two@example.com","three@example.com"] {f.user(email,true).await;}
    assert_eq!([balance(&f,1),balance(&f,2),balance(&f,3)],[200000,100000,0]);
}

// Operators need a separate secret and references cannot be reused with different amounts.
#[tokio::test]
async fn trusted_topups_are_idempotent() {
    let f=Fixture::new(Mode::Complete,&[("ADMIN_TOKEN","synthetic-admin-token")],false).await;f.user("test@example.com",true).await;
    let body=json!({"email":"test@example.com","amount_microusd":1000000,"reference":"manual-test-001"});
    assert_eq!(f.request("/v1/admin/credits",Some(body.clone()),"",&[]).await.0,401);
    for _ in 0..2 {assert_eq!(f.request("/v1/admin/credits",Some(body.clone()),"",&[("X-Admin-Token","synthetic-admin-token")]).await.0,200);}
    assert_eq!(balance(&f,1),1200000);let mut changed=body;changed["amount_microusd"]=json!(2000000);
    assert_eq!(f.request("/v1/admin/credits",Some(changed),"",&[("X-Admin-Token","synthetic-admin-token")]).await.0,409);
}

// Only fresh signed payment bytes matching a saved order can grant prepaid credits.
#[tokio::test]
async fn signed_webhook_grants_once_and_rejects_tampering() {
    let f=Fixture::new(Mode::Complete,&[("STRIPE_WEBHOOK_SECRET","synthetic-webhook-secret")],false).await;f.user("test@example.com",true).await;
    f.app.db.lock().unwrap().execute("INSERT INTO orders(id,user_id,amount,stripe_id) VALUES(?,?,?,?)",params!["order-fixture",1,500,"cs_fixture"]).unwrap();
    let body=json!({"type":"checkout.session.completed","data":{"object":{"id":"cs_fixture","client_reference_id":"order-fixture","currency":"usd","amount_total":500,"payment_status":"paid"}}});
    let raw=serde_json::to_vec(&body).unwrap();let timestamp=database::now()/1000;
    let mut mac=Hmac::<Sha256>::new_from_slice(b"synthetic-webhook-secret").unwrap();mac.update(format!("{timestamp}.").as_bytes());mac.update(&raw);
    let signature=format!("t={timestamp},v1={}",hex::encode(mac.finalize().into_bytes()));
    assert_eq!(f.http.post(format!("{}/v1/payments/webhook",f.base)).header("stripe-signature","t=1,v1=bad").body(raw.clone()).send().await.unwrap().status(),StatusCode::UNAUTHORIZED);
    for _ in 0..2 {assert_eq!(f.http.post(format!("{}/v1/payments/webhook",f.base)).header("stripe-signature",&signature).body(raw.clone()).send().await.unwrap().status(),StatusCode::OK);}
    assert_eq!(balance(&f,1),5200000);
    let mut changed=body;changed["data"]["object"]["amount_total"]=json!(1000);
    assert_eq!(f.http.post(format!("{}/v1/payments/webhook",f.base)).header("stripe-signature",signature).json(&changed).send().await.unwrap().status(),StatusCode::UNAUTHORIZED);
}

// Idempotency is bound to both the conversation and account, never just a client-selected string.
#[tokio::test]
async fn replay_cannot_change_content_or_owner() {
    let f=Fixture::new(Mode::Complete,&[],false).await;let first=f.user("one@example.com",true).await;let second=f.user("two@example.com",true).await;
    f.request("/v1/generate",Some(conversation()),&first,&[("Idempotency-Key","bound-replay-request")]).await;
    let mut changed=conversation();changed["messages"][0]["text"]=json!("Something else.");
    assert_eq!(f.request("/v1/generate",Some(changed),&first,&[("Idempotency-Key","bound-replay-request")]).await.0,409);
    assert_eq!(f.request("/v1/generate",Some(conversation()),&second,&[("Idempotency-Key","bound-replay-request")]).await.0,409);
    assert_eq!(f.provider.calls.load(Ordering::SeqCst),1);
}

// Expired response text disappears while the settled cost remains replay-safe.
#[tokio::test]
async fn retained_response_expires_without_spending_again() {
    let f=Fixture::new(Mode::Complete,&[("RESULT_RETENTION_HOURS","0")],false).await;let token=f.user("test@example.com",true).await;
    f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","retention-request-1")]).await;
    tokio::time::sleep(Duration::from_millis(2)).await;f.app.cleanup().unwrap();
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","retention-request-1")]).await;
    assert_eq!(last(&events)["type"],"error");assert_eq!(last(&events)["charged"],524);assert_eq!(f.provider.calls.load(Ordering::SeqCst),1);
}

// Byte-level SSE parsing preserves multilingual text, comments and multi-line JSON frames.
#[test]
fn split_utf8_and_multiline_sse() {
    let mut decoder=providers::Decoder::default();let mut events=vec![];
    for byte in b": keepalive\r\n\r\ndata: {\"text\":\r\ndata: \"\xe4\xb8\x96\xe7\x95\x8c\"}\r\n\r\n" {events.extend(decoder.push(&[*byte]).unwrap());}
    decoder.finish().unwrap();assert_eq!(events,vec![json!({"text":"世界"})]);
    decoder.push(b"data: {").unwrap();assert!(decoder.finish().is_err());
}

// Reservations bound maximum output and configured long-context pricing for international input.
#[test]
fn reservation_bounds_and_invalid_usage() {
    let config=Config::from_values(&HashMap::new()).unwrap();let conversation=slax_ai_server::protocol::validate(conversation(),120000).unwrap();
    let payload=providers::payload(&conversation,&config);
    assert!(providers::reservation(&payload,&config).unwrap()>providers::charge(providers::Usage {input_tokens:100,cached_tokens:0,output_tokens:config.max_output},&config).unwrap());
    assert!(providers::charge(providers::Usage {input_tokens:10,cached_tokens:11,output_tokens:1},&config).is_err());
    let mut long=config;long.long_threshold=50;
    assert_eq!(providers::charge(providers::Usage {input_tokens:100,cached_tokens:20,output_tokens:10},&long).unwrap(),948);
}

// Crash recovery retains unknown usage and cannot refund the same reservation twice.
#[test]
fn restart_recovery_preserves_uncertain_cost() {
    let mut db=database::open(std::path::Path::new(":memory:")).unwrap();
    db.execute("INSERT INTO users(email,password,balance,created) VALUES('test@example.com','synthetic',10000,0)",[]).unwrap();
    database::reserve(&mut db,1,"crash-request","fingerprint",2000).unwrap();
    database::recover(&mut db).unwrap();database::recover(&mut db).unwrap();
    assert_eq!(database::account(&db,1).unwrap().balance,8000);assert_eq!(database::request(&db,"crash-request").unwrap().charged,Some(2000));
}

// Checkout creates a server-owned package and only the signed paid webhook grants it.
#[tokio::test]
async fn checkout_uses_server_amount_and_saved_order() {
    let f=Fixture::new(Mode::Complete,&[("STRIPE_SECRET_KEY","synthetic-stripe-key"),("STRIPE_WEBHOOK_SECRET","synthetic-webhook-secret")],false).await;
    let token=f.user("checkout@example.com",true).await;
    assert_eq!(f.request("/v1/checkout",Some(json!({"cents":99})),&token,&[]).await.0,400);
    let (status,checkout)=f.request("/v1/checkout",Some(json!({"cents":500,"user_id":999,"credits":999999})),&token,&[]).await;
    assert_eq!(status,200);assert!(checkout["url"].as_str().unwrap().starts_with("https://checkout.stripe.com/"));assert_eq!(balance(&f,1),200000);
    let payloads=f.provider.payloads.lock().unwrap();assert_eq!(payloads[0]["line_items[0][price_data][unit_amount]"],"500");
    let db=f.app.db.lock().unwrap();let (owner,amount,reference)=db.query_row("SELECT user_id,amount,stripe_id FROM orders",[],|r|Ok((r.get::<_,i64>(0)?,r.get::<_,i64>(1)?,r.get::<_,String>(2)?))).unwrap();
    assert_eq!((owner,amount,reference),(1,500,"cs_fixture_checkout".into()));
}

// An unreachable upstream cannot repeatedly burn model traffic under new refunded reservations.
#[tokio::test]
async fn ambiguous_connection_error_retains_reservation() {
    let f=Fixture::new(Mode::Complete,&[],false).await;let token=f.user("network@example.com",true).await;
    f.servers[1].abort();tokio::time::sleep(Duration::from_millis(20)).await;
    let (_,events)=f.request("/v1/generate",Some(conversation()),&token,&[("Idempotency-Key","network-failure-request")]).await;
    assert_eq!(last(&events)["type"],"error");assert!(last(&events)["charged"].as_i64().unwrap()>0);
}

// Input bounds and rate limits reject requests before expensive hashing or provider dispatch.
#[tokio::test]
async fn input_and_login_attempt_limits() {
    let f=Fixture::new(Mode::Complete,&[("MAX_INPUT_BYTES","1024")],false).await;let token=f.user("limits@example.com",true).await;
    let mut body=conversation();body["messages"][0]["text"]=json!("世界".repeat(400));
    assert_eq!(f.request("/v1/generate",Some(body),&token,&[("Idempotency-Key","oversized-request-1")]).await.0,413);assert_eq!(f.provider.calls.load(Ordering::SeqCst),0);
    for _ in 0..11 {assert_eq!(f.request("/v1/auth/login",Some(json!({"email":"limits@example.com","password":"incorrect synthetic password"})),"",&[]).await.0,401);}
    assert_eq!(f.request("/v1/auth/login",Some(json!({"email":"limits@example.com","password":"incorrect synthetic password"})),"",&[]).await.0,429);
}

// Invalid monetary/TLS settings fail before the application accepts traffic.
#[test]
fn configuration_rejects_unsafe_settings() {
    for (key,value) in [("INPUT_PRICE","NaN"),("MAX_OUTPUT_TOKENS","1.5"),("PRICE_MULTIPLIER","0.5"),("TLS_KEY_FILE","key.pem"),("PUBLIC_URL","http://example.com")]
    {assert!(Config::from_values(&HashMap::from([(key.into(),value.into())])).is_err());}
    assert!(Config::from_values(&HashMap::from([("SMTP_HOST".into(),"smtp.example.com".into()),("SMTP_STARTTLS".into(),"false".into())])).is_err());
}

// Exercise the existing unchanged native Slax client against this gateway and a local diagnostic tool.
#[tokio::test]
#[ignore="requires SLAX_AI_TEST_CLIENT pointing to a built native Linux client"]
async fn existing_native_client_uses_rust_gateway() {
    let executable=std::env::var("SLAX_AI_TEST_CLIENT").unwrap();
    let f=Fixture::new(Mode::Agent,&[],false).await;f.user("native@example.com",true).await;
    let files=tempfile::tempdir().unwrap();let state=files.path().join("state");let workspace=files.path().join("workspace");std::fs::create_dir(&workspace).unwrap();
    let mut child=tokio::process::Command::new(executable).args(["--allow-http","--backend",&f.base,"--state-dir",state.to_str().unwrap()])
        .stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null()).kill_on_drop(true).spawn().unwrap();
    let runtime=state.join("runtime.json");
    for _ in 0..100 {if runtime.exists() {break;}tokio::time::sleep(Duration::from_millis(20)).await;}
    let runtime:Value=serde_json::from_slice(&std::fs::read(runtime).unwrap()).unwrap();
    let (url,token)=runtime["url"].as_str().unwrap().split_once("/#token=").unwrap();
    let login:Value=f.http.post(format!("{url}/api/auth/login")).bearer_auth(token).json(&json!({"email":"native@example.com","password":"synthetic password 123"})).send().await.unwrap().json().await.unwrap();
    assert_eq!(login["account"]["verified"],true);
    let chat:Value=f.http.post(format!("{url}/api/chat")).bearer_auth(token).json(&json!({"text":"Inspect this computer.","workspace":workspace})).send().await.unwrap().json().await.unwrap();
    assert!(chat["chat_id"].is_string());
    let mut result=Value::Null;
    for _ in 0..200 {
        result=f.http.get(format!("{url}/api/state")).bearer_auth(token).send().await.unwrap().json().await.unwrap();
        if result["active"].is_null() {break;}tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert!(result["active"].is_null());
    let messages=result["chats"][0]["messages"].as_array().unwrap();assert_eq!(messages.iter().map(|message|message["role"].as_str().unwrap()).collect::<Vec<_>>(),["user","assistant","tool","assistant"]);
    assert_eq!(messages.last().unwrap()["text"],"Hello 世界");assert_eq!(f.provider.calls.load(Ordering::SeqCst),2);assert_eq!(balance(&f,1),198952);
    child.kill().await.unwrap();
}
