use crate::{App,database::{self,Account},error::{Error,Result}};
use hmac::{Hmac,Mac};
use rusqlite::{OptionalExtension,params};
use serde_json::{Value,json};
use sha2::Sha256;
use std::sync::Arc;

// Hosted checkout owns the package allowlist; no payment-card fields enter our client or API.
pub async fn checkout(app:Arc<App>,user:Account,body:Value) -> Result<Value> {
    if app.config.stripe_key.is_empty() || app.config.stripe_webhook.is_empty() {return Err(Error::new(503,"Online top-ups are not enabled yet."));}
    let cents=body["cents"].as_i64().filter(|v|[100,500,1000,2000].contains(v)).ok_or_else(||Error::new(400,"Select a supported credit package."))?;
    let id=uuid::Uuid::new_v4().to_string();
    app.db.lock().unwrap().execute("INSERT INTO orders(id,user_id,amount) VALUES(?,?,?)",params![id,user.id,cents])?;
    let form=[("mode","payment".into()),("client_reference_id",id.clone()),("customer_email",user.email),
        ("success_url",format!("{}/payment/success",app.config.public_url)),("cancel_url",format!("{}/payment/cancel",app.config.public_url)),
        ("line_items[0][price_data][currency]","usd".into()),("line_items[0][price_data][unit_amount]",cents.to_string()),
        ("line_items[0][price_data][product_data][name]","Slax AI credits".into()),("line_items[0][quantity]","1".into())];
    let response=app.http.post(format!("{}/checkout/sessions",app.config.stripe_url)).bearer_auth(&app.config.stripe_key).header("Idempotency-Key",&id)
        .timeout(std::time::Duration::from_secs(20)).form(&form).send().await.map_err(|_|Error::new(502,"Checkout could not be opened. Try again shortly."))?;
    if !response.status().is_success() {return Err(Error::new(502,"Checkout could not be opened. Try again shortly."));}
    let session:Value=response.json().await.map_err(|_|Error::new(502,"Invalid checkout response."))?;
    let stripe_id=session["id"].as_str().ok_or_else(||Error::new(502,"Invalid checkout response."))?;
    let url=session["url"].as_str().filter(|url|url.starts_with("https://checkout.stripe.com/")).ok_or_else(||Error::new(502,"Invalid checkout response."))?;
    app.db.lock().unwrap().execute("UPDATE orders SET stripe_id=? WHERE id=?",params![stripe_id,id])?;
    Ok(json!({"url":url}))
}

// Verify a fresh HMAC over the original bytes, then fulfill only a matching paid order once.
pub fn webhook(app:&App,raw:&[u8],signature:&str) -> Result<Value> {
    if app.config.stripe_webhook.is_empty() {return Err(Error::new(503,"Payment webhooks are disabled."));}
    let parts:Vec<_>=signature.split(',').filter_map(|part|part.trim().split_once('=')).collect();
    let timestamp=parts.iter().find(|(key,_)|*key=="t").and_then(|(_,value)|value.parse::<i64>().ok()).filter(|value|*value>0).ok_or_else(||Error::new(401,"Invalid webhook signature."))?;
    if (database::now()/1000).abs_diff(timestamp)>300 {return Err(Error::new(401,"Invalid webhook signature."));}
    let mut mac=Hmac::<Sha256>::new_from_slice(app.config.stripe_webhook.as_bytes()).unwrap();
    mac.update(format!("{timestamp}.").as_bytes());mac.update(raw);
    let valid=parts.iter().filter(|(key,_)|*key=="v1").any(|(_,value)|hex::decode(value).is_ok_and(|bytes|mac.clone().verify_slice(&bytes).is_ok()));
    if !valid {return Err(Error::new(401,"Invalid webhook signature."));}
    let event:Value=serde_json::from_slice(raw).map_err(|_|Error::new(400,"Invalid JSON."))?;
    if !["checkout.session.completed","checkout.session.async_payment_succeeded"].contains(&event["type"].as_str().unwrap_or("")) || event["data"]["object"]["payment_status"]!="paid" {return Ok(json!({"received":true}));}
    let session=&event["data"]["object"];
    database::transaction(&mut app.db.lock().unwrap(),|db| {
        let id=session["id"].as_str().ok_or_else(||Error::new(400,"Payment does not match a saved order."))?;
        let order=db.query_row("SELECT id,user_id,amount FROM orders WHERE stripe_id=?",[id],|r|Ok((r.get::<_,String>(0)?,r.get::<_,i64>(1)?,r.get::<_,i64>(2)?))).optional()?
            .ok_or_else(||Error::new(400,"Payment does not match a saved order."))?;
        let matches=session["client_reference_id"]==order.0 && session["currency"]=="usd" && session["amount_total"].as_i64()==Some(order.2);
        if !matches {return Err(Error::new(400,"Payment does not match a saved order."));}
        database::credit(db,order.1,order.2*10000,&format!("stripe:{id}"))?;
        Ok(json!({"received":true}))
    })
}
