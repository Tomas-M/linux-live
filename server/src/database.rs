use crate::error::{Error,Result};
use base64::{Engine,engine::general_purpose::URL_SAFE_NO_PAD};
use rand::RngCore;
use rusqlite::{Connection,OptionalExtension,TransactionBehavior,params};
use serde::{Deserialize,Serialize};
use serde_json::Value;
use sha2::{Digest,Sha256};
use std::{fs,os::unix::fs::{DirBuilderExt,PermissionsExt},path::Path,time::{SystemTime,UNIX_EPOCH}};

#[derive(Clone,Debug,Serialize,Deserialize)]
pub struct Account {pub id:i64,pub email:String,pub verified:bool,pub balance:i64,pub credits:f64}
#[derive(Clone,Debug)]
pub struct Request {pub reserved:i64,pub charged:Option<i64>,pub result:Option<String>,pub error:Option<String>,pub finished:Option<i64>}

// Monetary timestamps use UTC milliseconds throughout the ledger and recovery paths.
pub fn now() -> i64 {SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as i64}
// Persist only SHA-256 digests of session and email verification secrets.
pub fn hash(value:&str) -> String {hex::encode(Sha256::digest(value.as_bytes()))}
// Session credentials contain 256 bits of operating-system randomness.
pub fn secret() -> String {let mut bytes=[0;32];rand::rngs::OsRng.fill_bytes(&mut bytes);URL_SAFE_NO_PAD.encode(bytes)}

// SQLite is embedded; its directory and database are private to the service user.
pub fn open(path:&Path) -> Result<Connection> {
    let memory=path==Path::new(":memory:");
    if !memory {
        if let Some(parent)=path.parent().filter(|p|!p.as_os_str().is_empty()) {
            fs::DirBuilder::new().recursive(true).mode(0o700).create(parent).map_err(|_|Error::new(500,"Unable to create database directory."))?;
        }
    }
    let db=Connection::open(path)?;
    db.busy_timeout(std::time::Duration::from_secs(5))?;
    db.execute_batch("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;
        CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY,email TEXT UNIQUE NOT NULL,password TEXT NOT NULL,verified INTEGER NOT NULL DEFAULT 0,balance INTEGER NOT NULL DEFAULT 0 CHECK(balance>=0),created INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS sessions(token TEXT PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id),expires INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS codes(user_id INTEGER NOT NULL REFERENCES users(id),purpose TEXT NOT NULL,code TEXT NOT NULL,expires INTEGER NOT NULL,PRIMARY KEY(user_id,purpose));
        CREATE TABLE IF NOT EXISTS ledger(id INTEGER PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id),amount INTEGER NOT NULL,reference TEXT UNIQUE NOT NULL,created INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS requests(id TEXT PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id),fingerprint TEXT NOT NULL,reserved INTEGER NOT NULL,charged INTEGER,result TEXT,error TEXT,created INTEGER NOT NULL,finished INTEGER);
        CREATE TABLE IF NOT EXISTS orders(id TEXT PRIMARY KEY,user_id INTEGER NOT NULL REFERENCES users(id),amount INTEGER NOT NULL,stripe_id TEXT UNIQUE);")?;
    if !memory {fs::set_permissions(path,fs::Permissions::from_mode(0o600)).map_err(|_|Error::new(500,"Unable to protect database."))?;}
    Ok(db)
}

// Roll back every partial financial operation before reporting an error.
pub fn transaction<T>(db:&mut Connection,operation:impl FnOnce(&Connection)->Result<T>) -> Result<T> {
    let tx=db.transaction_with_behavior(TransactionBehavior::Immediate)?;
    let result=operation(&tx)?;tx.commit()?;Ok(result)
}

// The public account contains no password, token, or provider credentials.
pub fn account(db:&Connection,id:i64) -> Result<Account> {
    Ok(db.query_row("SELECT id,email,verified,balance FROM users WHERE id=?",[id],|row| {
        let balance=row.get::<_,i64>(3)?;
        Ok(Account {id:row.get(0)?,email:row.get(1)?,verified:row.get(2)?,balance,credits:balance as f64/10000.0})
    })?)
}

// Operator and payment credits are idempotent and reject reused references with different data.
pub fn credit(db:&Connection,user:i64,amount:i64,reference:&str) -> Result<()> {
    if amount<=0 || amount>9007199254740991 {return Err(Error::new(400,"Invalid credit amount."));}
    if let Some((id,previous))=db.query_row("SELECT user_id,amount FROM ledger WHERE reference=?",[reference],|r|Ok((r.get::<_,i64>(0)?,r.get::<_,i64>(1)?))).optional()? {
        if id!=user || previous!=amount {return Err(Error::new(409,"Credit reference already used."));}
        return Ok(());
    }
    if db.execute("UPDATE users SET balance=balance+? WHERE id=? AND balance<=9007199254740991-?",params![amount,user,amount])?==0 {return Err(Error::new(400,"Account not found or credit balance exceeds the limit."));}
    db.execute("INSERT INTO ledger(user_id,amount,reference,created) VALUES(?,?,?,?)",params![user,amount,reference,now()])?;Ok(())
}

// Reserve before dispatch, binding one request ID to one account and canonical conversation.
pub fn reserve(db:&mut Connection,user:i64,id:&str,fingerprint:&str,amount:i64) -> Result<Option<Request>> {
    transaction(db,|db| {
        if let Some((owner,content))=db.query_row("SELECT user_id,fingerprint FROM requests WHERE id=?",[id],|r|Ok((r.get::<_,i64>(0)?,r.get::<_,String>(1)?))).optional()? {
            if owner!=user || content!=fingerprint {return Err(Error::new(409,"Request ID already used."));}
            let existing=request(db,id)?;
            if existing.finished.is_none() {return Err(Error::new(409,"This request is still being processed."));}
            return Ok(Some(existing));
        }
        if db.query_row("SELECT count(*) FROM requests WHERE user_id=? AND finished IS NULL",[user],|r|r.get::<_,i64>(0))?>0 {return Err(Error::new(409,"Another request is still running. Try again shortly."));}
        if db.execute("UPDATE users SET balance=balance-? WHERE id=? AND balance>=?",params![amount,user,amount])?==0 {return Err(Error::new(402,"Not enough credits for this request. Add credits or start a shorter chat."));}
        db.execute("INSERT INTO requests(id,user_id,fingerprint,reserved,created) VALUES(?,?,?,?,?)",params![id,user,fingerprint,amount,now()])?;
        db.execute("INSERT INTO ledger(user_id,amount,reference,created) VALUES(?,?,?,?)",params![user,-amount,format!("reserve:{id}"),now()])?;Ok(None)
    })
}

// Read the authoritative request row for retries, settlement, and usage reporting.
pub fn request(db:&Connection,id:&str) -> Result<Request> {
    Ok(db.query_row("SELECT reserved,charged,result,error,finished FROM requests WHERE id=?",[id],|r|Ok(Request {reserved:r.get(0)?,charged:r.get(1)?,result:r.get(2)?,error:r.get(3)?,finished:r.get(4)?}))?)
}

// Confirmed usage refunds the unused reservation exactly once; uncertain usage retains it.
pub fn settle(db:&mut Connection,id:&str,charge:i64,result:Option<&Value>,error:Option<&str>) -> Result<i64> {
    transaction(db,|db| {
        let row=request(db,id)?;
        if row.finished.is_some() {return Ok(row.charged.unwrap());}
        let charged=charge.clamp(0,row.reserved);let refund=row.reserved-charged;
        let user=db.query_row("SELECT user_id FROM requests WHERE id=?",[id],|r|r.get::<_,i64>(0))?;
        db.execute("UPDATE users SET balance=balance+? WHERE id=?",params![refund,user])?;
        db.execute("INSERT INTO ledger(user_id,amount,reference,created) VALUES(?,?,?,?)",params![user,refund,format!("settle:{id}"),now()])?;
        db.execute("UPDATE requests SET charged=?,result=?,error=?,finished=? WHERE id=?",params![charged,result.map(Value::to_string),error,now(),id])?;Ok(charged)
    })
}

// A restart cannot safely refund model traffic whose final usage was never confirmed.
pub fn recover(db:&mut Connection) -> Result<()> {
    let pending=db.prepare("SELECT id,reserved FROM requests WHERE finished IS NULL")?.query_map([],|r|Ok((r.get::<_,String>(0)?,r.get::<_,i64>(1)?)))?.collect::<std::result::Result<Vec<_>,_>>()?;
    for (id,amount) in pending {settle(db,&id,amount,None,Some("Server restarted before provider usage could be confirmed."))?;}
    Ok(())
}
