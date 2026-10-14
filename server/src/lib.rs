pub mod config;
pub mod database;
pub mod error;
pub mod protocol;
pub mod auth;
pub mod payments;
pub mod providers;

use axum::{Json,Router,body::Bytes,extract::{ConnectInfo,DefaultBodyLimit,Request,State},http::{HeaderMap,Method,Uri},middleware::{self,Next},response::{Html,IntoResponse,Response,Sse,sse::{Event,KeepAlive}}};
use config::Config;
use database::{Account,hash,now};
use error::{Error,Result};
use futures_util::StreamExt;
use rusqlite::{Connection,OptionalExtension,params};
use serde_json::{Value,json};
use std::{collections::HashMap,convert::Infallible,net::SocketAddr,sync::{Arc,Mutex},time::Duration};
use subtle::ConstantTimeEq;
use tokio::sync::mpsc;
use tokio_stream::wrappers::UnboundedReceiverStream;
use tokio_util::task::TaskTracker;

struct Bucket {count:u32,until:i64}
pub struct App {
    pub config:Config,pub db:Mutex<Connection>,pub http:reqwest::Client,pub tasks:TaskTracker,
    mailer:auth::Mailer,dummy_password:String,limits:Mutex<HashMap<String,Bucket>>,
}

impl App {
    // Recovery runs once at startup; all HTTP transactions share one serialized SQLite connection.
    pub fn new(config:Config,mut db:Connection) -> Result<Arc<Self>> {
        let _=rustls::crypto::ring::default_provider().install_default();
        database::recover(&mut db)?;
        let mailer=auth::Mailer::new(&config)?;
        let http=reqwest::Client::builder().redirect(reqwest::redirect::Policy::none()).connect_timeout(Duration::from_secs(10))
            .timeout(Duration::from_secs(150)).build().map_err(|_|Error::new(500,"Unable to initialize HTTPS client."))?;
        let app=Arc::new(Self {config,db:Mutex::new(db),http,mailer,dummy_password:auth::password_hash(&database::secret())?,limits:Mutex::new(HashMap::new()),tasks:TaskTracker::new()});
        let weak=Arc::downgrade(&app);
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(Duration::from_secs(60)).await;
                let Some(app)=weak.upgrade() else {break;};
                if app.cleanup().is_err() {eprintln!("Unable to complete periodic cleanup.");}
            }
        });
        Ok(app)
    }

    // Bounded buckets limit email abuse and expensive password/model work before dispatch.
    fn rate(&self,key:String,max:u32,period:i64) -> Result<()> {
        let mut limits=self.limits.lock().unwrap();let now=now();
        if limits.len()>=10000 && !limits.contains_key(&key) {return Err(Error::new(429,"Too many attempts. Try again later."));}
        let bucket=limits.entry(key).or_insert(Bucket {count:0,until:now+period});
        if bucket.until<now {bucket.count=0;bucket.until=now+period;}
        bucket.count=bucket.count.saturating_add(1);
        if bucket.count>max {return Err(Error::new(429,"Too many attempts. Try again later."));}Ok(())
    }

    // Retain monetary records but remove expired model response text and obsolete rate buckets.
    pub fn cleanup(&self) -> Result<()> {
        let now=now();self.limits.lock().unwrap().retain(|_,bucket|bucket.until>=now);
        self.db.lock().unwrap().execute("UPDATE requests SET result=NULL WHERE result IS NOT NULL AND finished<?",[now-self.config.retention_ms])?;Ok(())
    }
}

// Construct the same route surface used by the existing Rust Slax client.
pub fn router(app:Arc<App>) -> Router {
    Router::new().fallback(handle).with_state(app.clone()).layer(DefaultBodyLimit::max(2000000))
        .layer(middleware::from_fn_with_state(app,security))
}

// Trust forwarded addresses only behind the configured proxy, which must replace the header.
fn client_ip(app:&App,peer:SocketAddr,headers:&HeaderMap) -> String {
    if app.config.trust_proxy {
        if let Some(value)=headers.get("x-forwarded-for").and_then(|value|value.to_str().ok()).and_then(|value|value.split(',').next()) {return value.trim().to_string();}
    }
    peer.ip().to_string()
}

// Apply rate/body-time bounds before extraction and add private API headers to every response.
async fn security(State(app):State<Arc<App>>,request:Request,next:Next) -> Response {
    let peer=request.extensions().get::<ConnectInfo<SocketAddr>>().unwrap().0;
    let mut response=match app.rate(format!("all:{}",client_ip(&app,peer,request.headers())),240,60000) {
        Err(error)=>error.into_response(),
        Ok(())=>match tokio::time::timeout(Duration::from_secs(30),next.run(request)).await {
            Ok(response)=>response,Err(_)=>Error::new(408,"Request timed out.").into_response()
        }
    };
    if response.status()==axum::http::StatusCode::PAYLOAD_TOO_LARGE {
        response=Error::new(413,"Request is too large.").into_response();
    }
    for (name,value) in [("cache-control","no-store"),("x-content-type-options","nosniff"),("content-security-policy","default-src 'none'; frame-ancestors 'none'")] {
        response.headers_mut().insert(axum::http::header::HeaderName::from_static(name),axum::http::HeaderValue::from_static(value));
    }
    response
}

// Route account, billing and streaming operations without ever executing tools on this server.
async fn handle(State(app):State<Arc<App>>,ConnectInfo(peer):ConnectInfo<SocketAddr>,method:Method,uri:Uri,headers:HeaderMap,body:Bytes) -> Result<Response> {
    let path=uri.path();let header=|name:&str|headers.get(name).and_then(|v|v.to_str().ok()).unwrap_or("");
    if method==Method::GET && path=="/health" {return Ok(Json(json!({"ok":true})).into_response());}
    if method==Method::GET && ["/payment/success","/payment/cancel"].contains(&path) {
        let heading=if path.ends_with("success") {"Thank you"} else {"Checkout cancelled"};
        return Ok(Html(format!("<!doctype html><html lang=\"en\"><meta charset=\"utf-8\"><title>Slax AI</title><h1>{heading}</h1><p>Return to Slax AI and refresh your credits. Credits are added after payment is confirmed.</p></html>")).into_response());
    }
    if method==Method::POST && path=="/v1/payments/webhook" {
        if body.len()>1000000 {return Err(Error::new(413,"Request is too large."));}
        return Ok(Json(payments::webhook(&app,&body,header("stripe-signature"))?).into_response());
    }
    let data:Value=if method==Method::POST {serde_json::from_slice(&body).map_err(|_|Error::new(400,"Invalid JSON."))?} else {json!({})};
    if method==Method::POST && path=="/v1/admin/credits" {
        let valid=!app.config.admin_token.is_empty() && bool::from(hash(header("x-admin-token")).as_bytes().ct_eq(hash(&app.config.admin_token).as_bytes()));
        if !valid {return Err(Error::new(401,"Unauthorized."));}
        let reference=data["reference"].as_str().filter(|v|(8..=200).contains(&v.len())).ok_or_else(||Error::new(400,"Supply a unique payment reference."))?;
        let amount=data["amount_microusd"].as_i64().ok_or_else(||Error::new(400,"Invalid credit amount."))?;
        let email=data["email"].as_str().unwrap_or("").trim().to_lowercase();
        let mut db=app.db.lock().unwrap();
        let id=db.query_row("SELECT id FROM users WHERE email=?",[email],|r|r.get::<_,i64>(0)).optional()?.ok_or_else(||Error::new(404,"Account not found."))?;
        database::transaction(&mut db,|db|database::credit(db,id,amount,&format!("admin:{reference}")))?;
        return Ok(Json(json!({"account":database::account(&db,id)?})).into_response());
    }
    if method==Method::POST && ["/v1/auth/register","/v1/auth/login","/v1/auth/forgot","/v1/auth/reset"].contains(&path) {
        app.rate(format!("auth:{}",client_ip(&app,peer,&headers)),12,60000)?;
        if !path.ends_with("login") {app.rate(format!("email:{}",data["email"].as_str().unwrap_or("").trim().to_lowercase()),5,3600000)?;}
        return Ok(Json(auth::public(app,path.rsplit('/').next().unwrap(),data).await?).into_response());
    }
    let bearer=header("authorization").strip_prefix("Bearer ").unwrap_or("");
    let user={
        let db=app.db.lock().unwrap();
        let id=db.query_row("SELECT user_id FROM sessions WHERE token=? AND expires>?",params![hash(bearer),now()],|r|r.get::<_,i64>(0)).optional()?.ok_or_else(||Error::new(401,"Sign in to Slax AI."))?;
        database::account(&db,id)?
    };
    if method==Method::GET && path=="/v1/account" {
        return Ok(Json(json!({"account":user,"model":app.config.model,"provider":app.config.provider,"welcome_credits":app.config.welcome as f64/10000.0,
            "checkout_enabled":!app.config.stripe_key.is_empty() && !app.config.stripe_webhook.is_empty()})).into_response());
    }
    if method==Method::POST && path=="/v1/auth/logout" {
        app.db.lock().unwrap().execute("DELETE FROM sessions WHERE token=?",[hash(bearer)])?;
        return Ok(Json(json!({"ok":true})).into_response());
    }
    if method==Method::POST && ["/v1/auth/verify","/v1/auth/resend"].contains(&path) {
        app.rate(format!("verification:{}",user.id),8,60000)?;
        if path.ends_with("resend") {app.rate(format!("resend:{}",user.id),5,3600000)?;}
        return Ok(Json(auth::verified(app,user,path.rsplit('/').next().unwrap(),data).await?).into_response());
    }
    if !user.verified {return Err(Error::new(403,"Verify your email before using Slax AI."));}
    if method==Method::POST && path=="/v1/checkout" {return Ok(Json(payments::checkout(app,user,data).await?).into_response());}
    if method==Method::GET && path=="/v1/usage" {
        let db=app.db.lock().unwrap();
        let entries=db.prepare("SELECT id,reserved,charged,created,finished,error FROM requests WHERE user_id=? ORDER BY created DESC LIMIT 50")?
            .query_map([user.id],|r|Ok(json!({"id":r.get::<_,String>(0)?,"reserved":r.get::<_,i64>(1)?,"charged":r.get::<_,Option<i64>>(2)?,"created":r.get::<_,i64>(3)?,"finished":r.get::<_,Option<i64>>(4)?,"error":r.get::<_,Option<String>>(5)?})))?
            .collect::<std::result::Result<Vec<_>,_>>()?;
        return Ok(Json(json!({"entries":entries})).into_response());
    }
    if method!=Method::POST || path!="/v1/generate" {return Err(Error::new(404,"Not found."));}
    if app.config.api_key.is_empty() {return Err(Error::new(503,"The AI provider is not configured yet."));}
    let id=header("idempotency-key").to_string();
    if !(16..=100).contains(&id.len()) || !id.bytes().all(|c|c.is_ascii_alphanumeric() || c==b'_' || c==b'-') {return Err(Error::new(400,"Supply an Idempotency-Key."));}
    let conversation=protocol::validate(data,app.config.max_input)?;
    let payload=providers::payload(&conversation,&app.config);
    let reservation=providers::reservation(&payload,&app.config)?;
    let existing=database::reserve(&mut app.db.lock().unwrap(),user.id,&id,&hash(&serde_json::to_string(&conversation).unwrap()),reservation)?;
    let (send,receive)=mpsc::unbounded_channel();
    if let Some(existing)=existing {
        let event=if let Some(result)=existing.result {
            let mut result:Value=serde_json::from_str(&result).map_err(|_|Error::new(500,"Stored response could not be read."))?;
            result["type"]=json!("result");result["charged"]=json!(existing.charged);result["account"]=json!(database::account(&app.db.lock().unwrap(),user.id)?);result
        } else {json!({"type":"error","message":existing.error.unwrap_or_else(||"This completed response has expired. Review local history before sending another request.".into()),"charged":existing.charged,"account":database::account(&app.db.lock().unwrap(),user.id)?})};
        let _=send.send(event);
    } else {
        let _=send.send(json!({"type":"status","message":"Thinking","request_id":id}));
        app.tasks.spawn(generation(app.clone(),user,id,payload,send));
    }
    let stream=UnboundedReceiverStream::new(receive).map(|value|Ok::<Event,Infallible>(Event::default().json_data(value).unwrap()));
    let mut response=Sse::new(stream).keep_alive(KeepAlive::new().interval(Duration::from_secs(10)).text("keepalive")).into_response();
    response.headers_mut().insert("x-accel-buffering",axum::http::HeaderValue::from_static("no"));
    Ok(response)
}

// Detached provider tasks survive client disconnects and settle the ledger before sending completion.
async fn generation(app:Arc<App>,user:Account,id:String,payload:Value,events:mpsc::UnboundedSender<Value>) {
    let generated=providers::generate(&app.http,&app.config,&payload,&events).await;
    let settled=(|| -> Result<Value> {
        let mut db=app.db.lock().unwrap();
        match generated {
            Ok(mut result)=>{
                let usage=serde_json::from_value(result["usage"].clone()).map_err(|_|Error::new(502,"Provider returned invalid usage."))?;
                let charged=database::settle(&mut db,&id,providers::charge(usage,&app.config)?,Some(&result),None)?;
                result["type"]=json!("result");result["charged"]=json!(charged);result["account"]=json!(database::account(&db,user.id)?);Ok(result)
            },
            Err(failure)=>{
                let amount=if let Some(usage)=failure.usage {providers::charge(usage,&app.config)?} else if failure.accepted {database::request(&db,&id)?.reserved} else {0};
                let charged=database::settle(&mut db,&id,amount,None,Some(&failure.error.message))?;
                Ok(json!({"type":"error","message":failure.error.message,"charged":charged,"account":database::account(&db,user.id)?}))
            }
        }
    })();
    let event=settled.unwrap_or_else(|_|json!({"type":"error","message":"Unable to confirm billing. Contact support before retrying this request."}));
    let _=events.send(event);
}
