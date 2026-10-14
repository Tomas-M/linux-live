use slax_ai_server::{App,config::Config,database,error::{Error,Result}};
use std::{fs,net::{IpAddr,SocketAddr},os::unix::{fs::PermissionsExt,io::AsRawFd},path::{Path,PathBuf},time::Duration};

// Keep one server process per database while allowing the trusted credits CLI to share SQLite.
fn lock_database(path:&Path) -> Result<fs::File> {
    if let Some(parent)=path.parent().filter(|p|!p.as_os_str().is_empty()) {fs::create_dir_all(parent).map_err(|_|Error::new(500,"Unable to create database directory."))?;}
    let file=fs::OpenOptions::new().create(true).truncate(false).write(true).open(path.with_extension("server.lock")).map_err(|_|Error::new(500,"Unable to lock database."))?;
    if unsafe {libc::flock(file.as_raw_fd(),libc::LOCK_EX|libc::LOCK_NB)}!=0 {return Err(Error::new(500,"Another server already owns this database."));}
    Ok(file)
}

// Parse decimal dollar top-ups exactly, without binary floating-point rounding.
fn dollars(text:&str) -> Result<i64> {
    let (whole,fraction)=text.split_once('.').unwrap_or((text,""));
    let valid_whole=!whole.is_empty() && whole.bytes().all(|c|c.is_ascii_digit());
    let valid_fraction=fraction.len()<=6 && fraction.bytes().all(|c|c.is_ascii_digit());
    if !valid_whole || !valid_fraction {return Err(Error::new(400,"Supply a positive dollar amount with at most six decimal places."));}
    let whole=whole.parse::<i64>().map_err(|_|Error::new(400,"Invalid dollar amount."))?;
    let fraction=if fraction.is_empty() {0} else {fraction.parse::<i64>().unwrap()*10i64.pow(6-fraction.len() as u32)};
    whole.checked_mul(1000000).and_then(|amount|amount.checked_add(fraction)).filter(|amount|*amount>0 && *amount<=9007199254740991).ok_or_else(||Error::new(400,"Invalid dollar amount."))
}

// SIGTERM and Ctrl+C stop acceptance; already-paid generations finish before process exit.
async fn shutdown() {
    let mut term=tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).unwrap();
    tokio::select! {_=tokio::signal::ctrl_c()=>{},_=term.recv()=>{}}
}

// The deployable executable includes HTTP, optional HTTPS, SMTP, cryptography and SQLite.
#[tokio::main]
async fn main() -> Result<()> {
    unsafe {libc::umask(0o077);}
    let mut args:Vec<String>=std::env::args().skip(1).collect();
    if args.iter().any(|arg|arg=="--help") {
        println!("Slax AI server 0.1.0\nUsage: slax-ai-server [--env FILE] [--check]\n       slax-ai-server [--env FILE] credits EMAIL DOLLARS UNIQUE_REFERENCE\nTLS_CERT_FILE and TLS_KEY_FILE enable native HTTPS. SIGHUP reloads the certificate.");return Ok(());
    }
    let env=if let Some(index)=args.iter().position(|arg|arg=="--env") {
        if index+1>=args.len() {return Err(Error::new(400,"Supply a configuration file after --env."));}
        let file=PathBuf::from(args.remove(index+1));args.remove(index);Some(file)
    } else if Path::new(".env").is_file() {Some(PathBuf::from(".env"))} else {None};
    let config=Config::load(env.as_deref())?;
    if args==["--check"] {
        let _=slax_ai_server::auth::Mailer::new(&config)?;
        if let (Some(cert),Some(key))=(&config.tls_cert,&config.tls_key) {
            let _=rustls::crypto::ring::default_provider().install_default();
            axum_server::tls_rustls::RustlsConfig::from_pem_file(cert,key).await.map_err(|_|Error::new(500,"Unable to load TLS certificate and key."))?;
        }
        println!("Configuration is valid. Listener: {}:{}; provider: {}; model: {}; API key configured: {}; SMTP configured: {}; native HTTPS: {}.",config.host,config.port,config.provider,config.model,!config.api_key.is_empty(),!config.smtp_host.is_empty(),config.tls_cert.is_some());return Ok(());
    }
    if args.first().is_some_and(|arg|arg=="credits") {
        if args.len()!=4 || !(8..=200).contains(&args[3].len()) {return Err(Error::new(400,"Usage: credits EMAIL DOLLARS UNIQUE_REFERENCE"));}
        let amount=dollars(&args[2])?;let mut db=database::open(&config.database)?;
        let id=db.query_row("SELECT id FROM users WHERE email=?",[args[1].trim().to_lowercase()],|r|r.get::<_,i64>(0)).map_err(|_|Error::new(404,"Account not found."))?;
        database::transaction(&mut db,|db|database::credit(db,id,amount,&format!("admin:{}",args[3])))?;
        println!("{}",serde_json::to_string(&database::account(&db,id)?).unwrap());return Ok(());
    }
    if !args.is_empty() {return Err(Error::new(400,"Unknown argument. Use --help."));}
    let address=SocketAddr::new(config.host.parse::<IpAddr>().map_err(|_|Error::new(500,"HOST must be an IP address."))?,config.port);
    let _lock=lock_database(&config.database)?;
    fs::set_permissions(config.database.with_extension("server.lock"),fs::Permissions::from_mode(0o600)).map_err(|_|Error::new(500,"Unable to protect database lock."))?;
    let app=App::new(config.clone(),database::open(&config.database)?)?;
    let router=slax_ai_server::router(app.clone()).into_make_service_with_connect_info::<SocketAddr>();
    let tls=if let (Some(cert),Some(key))=(&config.tls_cert,&config.tls_key) {
        Some(axum_server::tls_rustls::RustlsConfig::from_pem_file(cert,key).await.map_err(|_|Error::new(500,"Unable to load TLS certificate and key."))?)
    } else {None};
    let reload=tls.clone();let paths=config.tls_cert.zip(config.tls_key);
    let mut hup=tokio::signal::unix::signal(tokio::signal::unix::SignalKind::hangup()).unwrap();
    tokio::spawn(async move {
        while hup.recv().await.is_some() {
            if let (Some(tls),Some((cert,key)))=(&reload,&paths) {
                if tls.reload_from_pem_file(cert,key).await.is_err() {eprintln!("TLS certificate reload failed; the previous certificate remains active.");}
            }
        }
    });
    let handle=axum_server::Handle::new();let stop=handle.clone();let grace=Duration::from_millis(config.timeout_ms+30000);
    tokio::spawn(async move {shutdown().await;stop.graceful_shutdown(Some(grace));});
    println!("Slax AI server listening on {address}. Native HTTPS: {}.",tls.is_some());
    if let Some(tls)=tls {
        let mut server=axum_server::bind_rustls(address,tls).handle(handle);
        server.http_builder().http1().timer(hyper_util::rt::TokioTimer::new()).header_read_timeout(Duration::from_secs(15));
        server.serve(router).await.map_err(|_|Error::new(500,"HTTPS server failed."))?;
    } else {
        let mut server=axum_server::bind(address).handle(handle);
        server.http_builder().http1().timer(hyper_util::rt::TokioTimer::new()).header_read_timeout(Duration::from_secs(15));
        server.serve(router).await.map_err(|_|Error::new(500,"HTTP server failed."))?;
    }
    app.tasks.close();app.tasks.wait().await;
    Ok(())
}
