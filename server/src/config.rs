use crate::error::{Error,Result};
use std::{collections::HashMap, path::{Path,PathBuf}};

#[derive(Clone)]
pub struct Config {
    pub host:String,pub port:u16,pub public_url:String,pub database:PathBuf,pub trust_proxy:bool,
    pub welcome:i64,pub welcome_daily:i64,pub retention_ms:i64,pub multiplier:f64,
    pub max_output:u64,pub max_input:usize,pub timeout_ms:u64,
    pub provider:String,pub base_url:String,pub api_key:String,pub model:String,pub effort:String,
    pub input_price:f64,pub cached_price:f64,pub output_price:f64,pub long_threshold:u64,pub long_input:f64,pub long_output:f64,
    pub smtp_host:String,pub smtp_port:u16,pub smtp_secure:bool,pub smtp_starttls:bool,pub smtp_user:String,pub smtp_password:String,pub mail_from:String,
    pub admin_token:String,pub stripe_key:String,pub stripe_webhook:String,pub stripe_url:String,
    pub tls_cert:Option<PathBuf>,pub tls_key:Option<PathBuf>,
}

impl Config {
    // An explicit configuration file is required; process environment takes precedence.
    pub fn load(file:Option<&Path>) -> Result<Self> {
        let mut values=HashMap::new();
        if let Some(file)=file {
            let entries=dotenvy::from_path_iter(file).map_err(|_|Error::new(500,"Unable to read configuration file."))?;
            for entry in entries {let (key,value)=entry.map_err(|_|Error::new(500,"Invalid configuration file."))?;values.insert(key,value);}
        }
        values.extend(std::env::vars());
        Self::from_values(&values)
    }

    // Typed configuration validates prices and limits before any request can spend credits.
    pub fn from_values(values:&HashMap<String,String>) -> Result<Self> {
        let text=|key:&str,default:&str|values.get(key).cloned().unwrap_or_else(||default.into());
        let number=|key:&str,default:f64| -> Result<f64> {
            let value=text(key,&default.to_string()).parse::<f64>().map_err(|_|Error::new(500,format!("Invalid setting: {key}")))?;
            if !value.is_finite() || !(0.0..=1e12).contains(&value) {return Err(Error::new(500,format!("Invalid setting: {key}")));}
            Ok(value)
        };
        let integer=|key:&str,default:u64| -> Result<u64> {
            let value=number(key,default as f64)?;
            if value.fract()!=0.0 {return Err(Error::new(500,format!("Invalid setting: {key}")));}
            Ok(value as u64)
        };
        let port=|key:&str,default:u16|u16::try_from(integer(key,default.into())?).map_err(|_|Error::new(500,format!("Invalid setting: {key}")));
        let flag=|key:&str,default:bool| -> Result<bool> {
            match text(key,if default {"true"} else {"false"}).as_str() {"true"=>Ok(true),"false"=>Ok(false),_=>Err(Error::new(500,format!("Invalid setting: {key}")))}
        };
        let config=Self {
            host:text("HOST","127.0.0.1"),port:port("PORT",8787)?,public_url:text("PUBLIC_URL","http://127.0.0.1:8787").trim_end_matches('/').into(),
            database:text("DATABASE","data/slax-ai.sqlite").into(),trust_proxy:flag("TRUST_PROXY",false)?,
            welcome:(number("WELCOME_CREDITS",20.0)?*10000.0).round() as i64,welcome_daily:(number("WELCOME_DAILY_LIMIT",1000.0)?*10000.0).round() as i64,
            retention_ms:(number("RESULT_RETENTION_HOURS",24.0)?*3600000.0).round() as i64,multiplier:number("PRICE_MULTIPLIER",2.0)?,
            max_output:integer("MAX_OUTPUT_TOKENS",4096)?,max_input:integer("MAX_INPUT_BYTES",120000)? as usize,timeout_ms:integer("PROVIDER_TIMEOUT_MS",120000)?,
            provider:text("AI_PROVIDER","openai-responses"),base_url:text("AI_BASE_URL","https://api.openai.com/v1").trim_end_matches('/').into(),
            api_key:text("AI_API_KEY",""),model:text("AI_MODEL","gpt-6.1-sol"),effort:text("AI_REASONING_EFFORT","medium"),
            input_price:number("INPUT_PRICE",2.0)?,cached_price:number("CACHED_INPUT_PRICE",0.1)?,output_price:number("OUTPUT_PRICE",10.0)?,
            long_threshold:integer("LONG_CONTEXT_THRESHOLD",272000)?,long_input:number("LONG_INPUT_MULTIPLIER",2.0)?,long_output:number("LONG_OUTPUT_MULTIPLIER",1.5)?,
            smtp_host:text("SMTP_HOST",""),smtp_port:port("SMTP_PORT",587)?,smtp_secure:flag("SMTP_SECURE",false)?,smtp_starttls:flag("SMTP_STARTTLS",true)?,
            smtp_user:text("SMTP_USER",""),smtp_password:text("SMTP_PASSWORD",""),mail_from:text("MAIL_FROM","Slax AI <support@example.com>"),
            admin_token:text("ADMIN_TOKEN",""),stripe_key:text("STRIPE_SECRET_KEY",""),stripe_webhook:text("STRIPE_WEBHOOK_SECRET",""),stripe_url:"https://api.stripe.com/v1".into(),
            tls_cert:values.get("TLS_CERT_FILE").filter(|v|!v.is_empty()).map(PathBuf::from),tls_key:values.get("TLS_KEY_FILE").filter(|v|!v.is_empty()).map(PathBuf::from),
        };
        let valid_context=(1024..=1000000).contains(&config.max_input);
        let valid_output=(256..=1000000).contains(&config.max_output);
        let valid_timeout=(1000..=600000).contains(&config.timeout_ms);
        if config.multiplier<1.0 || !valid_context {return Err(Error::new(500,"Invalid model limits."));}
        if !valid_output || !valid_timeout {return Err(Error::new(500,"Invalid model limits."));}
        if !["openai-responses","chat-completions"].contains(&config.provider.as_str()) {return Err(Error::new(500,"Unknown AI_PROVIDER."));}
        if config.cached_price>config.input_price || config.long_input<1.0 || config.long_output<1.0 {return Err(Error::new(500,"Invalid pricing rates."));}
        if config.tls_cert.is_some()!=config.tls_key.is_some() {return Err(Error::new(500,"Set both TLS_CERT_FILE and TLS_KEY_FILE."));}
        let plain_smtp=!config.smtp_secure && !config.smtp_starttls;
        let remote_smtp=!config.smtp_host.is_empty() && !["localhost","127.0.0.1","::1"].contains(&config.smtp_host.as_str());
        if plain_smtp && remote_smtp {return Err(Error::new(500,"Unencrypted SMTP is allowed only on localhost."));}
        for url in [&config.base_url,&config.public_url] {
            let url=reqwest::Url::parse(url).map_err(|_|Error::new(500,"Invalid service URL."))?;
            let local=["localhost","127.0.0.1","[::1]"].contains(&url.host_str().unwrap_or(""));
            let user_info=!url.username().is_empty() || url.password().is_some();
            let extra=url.query().is_some() || url.fragment().is_some();
            let secure=url.scheme()=="https" || (url.scheme()=="http" && local);
            if user_info || extra || !secure {return Err(Error::new(500,"Use HTTPS service URLs; localhost HTTP is for development only."));}
        }
        Ok(config)
    }
}
