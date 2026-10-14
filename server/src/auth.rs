use crate::{App,database::{self,Account},error::{Error,Result}};
use argon2::{Argon2,PasswordHash,PasswordHasher,PasswordVerifier,password_hash::SaltString};
use lettre::{AsyncSmtpTransport,AsyncTransport,Message,Tokio1Executor,message::Mailbox,transport::smtp::authentication::Credentials};
use rand::Rng;
use rusqlite::{OptionalExtension,params};
use serde::Deserialize;
use serde_json::{Value,json};
use std::sync::Arc;
use subtle::ConstantTimeEq;

// Bound Argon2 memory/CPU even when many different accounts authenticate concurrently.
static HASH_JOBS:tokio::sync::Semaphore=tokio::sync::Semaphore::const_new(4);

pub struct Mailer {transport:Option<AsyncSmtpTransport<Tokio1Executor>>,from:Mailbox}
impl Mailer {
    // Remote SMTP requires TLS; an explicitly configured loopback relay may use plain SMTP.
    pub fn new(config:&crate::config::Config) -> Result<Self> {
        let from=config.mail_from.parse().map_err(|_|Error::new(500,"Invalid MAIL_FROM."))?;
        if config.smtp_host.is_empty() {return Ok(Self {transport:None,from});}
        let mut builder=if config.smtp_secure {AsyncSmtpTransport::<Tokio1Executor>::relay(&config.smtp_host)}
            else if config.smtp_starttls {AsyncSmtpTransport::<Tokio1Executor>::starttls_relay(&config.smtp_host)}
            else {Ok(AsyncSmtpTransport::<Tokio1Executor>::builder_dangerous(&config.smtp_host))}
            .map_err(|_|Error::new(500,"Invalid SMTP configuration."))?.port(config.smtp_port).timeout(Some(std::time::Duration::from_secs(10)));
        if !config.smtp_user.is_empty() {builder=builder.credentials(Credentials::new(config.smtp_user.clone(),config.smtp_password.clone()));}
        Ok(Self {transport:Some(builder.build()),from})
    }

    // Send verification mail only through the configured server-side transport.
    pub async fn code(&self,email:&str,purpose:&str,code:&str) -> Result<()> {
        let transport=self.transport.as_ref().ok_or_else(||Error::new(503,"Email is not configured."))?;
        let verify=purpose=="verify";
        let subject=if verify {"Verify your Slax AI email"} else {"Reset your Slax AI password"};
        let label=if verify {"verification"} else {"password reset"};
        let message=Message::builder().from(self.from.clone()).to(email.parse().map_err(|_|Error::new(400,"Enter a valid email address."))?).subject(subject)
            .body(format!("Your Slax AI {label} code is {code}. It expires in 15 minutes. If you did not request this, ignore this message."))
            .map_err(|_|Error::new(503,"Email could not be prepared."))?;
        match tokio::time::timeout(std::time::Duration::from_secs(20),transport.send(message)).await {
            Ok(Ok(_))=>Ok(()),_=>Err(Error::new(503,"Email could not be delivered. Try again shortly or contact support."))
        }
    }
}

#[derive(Deserialize)]
struct Login {email:String,password:String}

// Unicode email and passphrases are validated independently of SMTP/provider credentials.
fn credentials(body:&Value) -> Result<Login> {
    let mut login:Login=serde_json::from_value(body.clone()).map_err(|_|Error::new(400,"Enter an email and password."))?;
    login.email=login.email.trim().to_lowercase();
    let pieces:Vec<_>=login.email.split('@').collect();
    if pieces.len()!=2 || pieces[0].is_empty() {return Err(Error::new(400,"Enter a valid email address."));}
    let valid_domain=pieces[1].contains('.') && !pieces[1].starts_with('.') && !pieces[1].ends_with('.');
    let valid_text=login.email.len()<=254 && !login.email.chars().any(char::is_whitespace);
    if !valid_domain || !valid_text {return Err(Error::new(400,"Enter a valid email address."));}
    if login.password.chars().count()<12 || login.password.len()>4096 {return Err(Error::new(400,"Use a password with at least 12 characters."));}
    Ok(login)
}

// Each password gets a fresh salt and an Argon2id PHC record, never a reversible encoding.
pub fn password_hash(password:&str) -> Result<String> {
    let salt=SaltString::generate(&mut rand::rngs::OsRng);
    Argon2::default().hash_password(password.as_bytes(),&salt).map(|value|value.to_string()).map_err(|_|Error::new(500,"Unable to secure password."))
}

// Password hashing runs outside Tokio's request workers and outside SQLite transactions.
async fn encode(password:String) -> Result<String> {
    let slot=HASH_JOBS.acquire().await.unwrap();
    tokio::task::spawn_blocking(move || {let _slot=slot;password_hash(&password)}).await.map_err(|_|Error::new(500,"Password processing failed."))?
}

// Missing accounts verify against a real dummy hash to avoid a cheap enumeration timing path.
async fn matches(password:String,record:String) -> Result<bool> {
    let slot=HASH_JOBS.acquire().await.unwrap();
    tokio::task::spawn_blocking(move || {
        let _slot=slot;
        let encoded=PasswordHash::new(&record).map_err(|_|Error::new(500,"Invalid password record. Reset this account's password."))?;
        Ok(Argon2::default().verify_password(password.as_bytes(),&encoded).is_ok())
    }).await.map_err(|_|Error::new(500,"Password processing failed."))?
}

// New login sessions store only a digest of a random 256-bit bearer token.
fn session(db:&rusqlite::Connection,user:i64) -> Result<Value> {
    let token=database::secret();let now=database::now();
    db.execute("INSERT INTO sessions(token,user_id,expires) VALUES(?,?,?)",params![database::hash(&token),user,now+30*86400000])?;
    db.execute("DELETE FROM sessions WHERE expires<?",[now])?;
    Ok(json!({"token":token,"account":database::account(db,user)?}))
}

// Replacing a code invalidates older verification messages without persisting the raw code.
async fn send_code(app:&App,id:i64,email:&str,purpose:&str) -> Result<()> {
    let code=rand::thread_rng().gen_range(100000..1000000).to_string();
    app.db.lock().unwrap().execute("INSERT INTO codes(user_id,purpose,code,expires) VALUES(?,?,?,?) ON CONFLICT(user_id,purpose) DO UPDATE SET code=excluded.code,expires=excluded.expires",
        params![id,purpose,database::hash(&format!("{id}:{purpose}:{code}")),database::now()+15*60000])?;
    app.mailer.code(email,purpose,&code).await
}

// Verification and reset compare fixed-length digests and enforce their expiry inside transactions.
fn check_code(db:&rusqlite::Connection,id:i64,purpose:&str,body:&Value) -> Result<()> {
    let code=body["code"].as_str().ok_or_else(||Error::new(400,"Invalid or expired email code."))?;
    let stored=db.query_row("SELECT code,expires FROM codes WHERE user_id=? AND purpose=?",params![id,purpose],|r|Ok((r.get::<_,String>(0)?,r.get::<_,i64>(1)?))).optional()?;
    let valid=stored.is_some_and(|(digest,expires)|expires>=database::now() && bool::from(digest.as_bytes().ct_eq(database::hash(&format!("{id}:{purpose}:{code}")).as_bytes())));
    if !valid {return Err(Error::new(400,"Invalid or expired email code."));}Ok(())
}

// Authenticate, register and reset while keeping mail and expensive hashing outside SQLite locks.
pub async fn public(app:Arc<App>,operation:&str,body:Value) -> Result<Value> {
    if operation=="forgot" {
        let email=body["email"].as_str().unwrap_or("").trim().to_lowercase();
        let id=app.db.lock().unwrap().query_row("SELECT id FROM users WHERE email=?",[&email],|r|r.get::<_,i64>(0)).optional()?;
        if let Some(id)=id {send_code(&app,id,&email,"reset").await?;}
        return Ok(json!({"sent":true}));
    }
    let login=credentials(&body)?;
    if operation=="register" {
        if app.db.lock().unwrap().query_row("SELECT count(*) FROM users WHERE email=?",[&login.email],|r|r.get::<_,i64>(0))?>0 {return Err(Error::new(409,"This email is already registered. Sign in or reset your password."));}
        let encoded=encode(login.password).await?;
        let id={
            let db=app.db.lock().unwrap();
            match db.execute("INSERT INTO users(email,password,created) VALUES(?,?,?)",params![login.email,encoded,database::now()]) {
                Ok(_)=>db.last_insert_rowid(),
                Err(error) if error.sqlite_error_code()==Some(rusqlite::ErrorCode::ConstraintViolation)=>return Err(Error::new(409,"This email is already registered.")),
                Err(error)=>return Err(error.into())
            }
        };
        send_code(&app,id,&login.email,"verify").await?;
        return session(&app.db.lock().unwrap(),id);
    }
    let user=app.db.lock().unwrap().query_row("SELECT id,password FROM users WHERE email=?",[&login.email],|r|Ok((r.get::<_,i64>(0)?,r.get::<_,String>(1)?))).optional()?;
    if operation=="login" {
        let record=user.as_ref().map(|v|v.1.clone()).unwrap_or_else(||app.dummy_password.clone());
        if !matches(login.password,record).await? || user.is_none() {return Err(Error::new(401,"Email or password is incorrect."));}
        return session(&app.db.lock().unwrap(),user.unwrap().0);
    }
    let id=user.ok_or_else(||Error::new(400,"Invalid or expired email code."))?.0;
    check_code(&app.db.lock().unwrap(),id,"reset",&body)?;
    let encoded=encode(login.password).await?;
    database::transaction(&mut app.db.lock().unwrap(),|db| {
        check_code(db,id,"reset",&body)?;
        db.execute("UPDATE users SET password=? WHERE id=?",params![encoded,id])?;
        db.execute("DELETE FROM sessions WHERE user_id=?",[id])?;
        db.execute("DELETE FROM codes WHERE user_id=? AND purpose='reset'",[id])?;
        session(db,id)
    })
}

// A verified account can receive one introductory grant from the shared UTC daily pool.
pub async fn verified(app:Arc<App>,user:Account,operation:&str,body:Value) -> Result<Value> {
    if operation=="resend" {if !user.verified {send_code(&app,user.id,&user.email,"verify").await?;}return Ok(json!({"sent":true}));}
    database::transaction(&mut app.db.lock().unwrap(),|db| {
        if !database::account(db,user.id)?.verified {
            check_code(db,user.id,"verify",&body)?;
            let now=database::now();let day=now-now%86400000;
            let granted=db.query_row("SELECT COALESCE(sum(amount),0) FROM ledger WHERE reference LIKE 'welcome:%' AND created>=?",[day],|r|r.get::<_,i64>(0))?;
            let amount=app.config.welcome.min((app.config.welcome_daily-granted).max(0));
            if amount>0 {database::credit(db,user.id,amount,&format!("welcome:{}",user.id))?;}
            db.execute("UPDATE users SET verified=1 WHERE id=?",[user.id])?;
            db.execute("DELETE FROM codes WHERE user_id=? AND purpose='verify'",[user.id])?;
        }
        Ok(json!({"account":database::account(db,user.id)?}))
    })
}
