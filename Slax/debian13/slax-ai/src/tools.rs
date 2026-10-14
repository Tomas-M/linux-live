use serde_json::{json, Value};
use std::{fs, io::{Read, Write}, path::{Path, PathBuf}, process::{Command, Stdio}, thread, time::{Duration, Instant}};
use std::os::unix::{fs::{PermissionsExt,MetadataExt}, io::AsRawFd, process::CommandExt};
use uuid::Uuid;

// Resolve symlinks and keep file access inside the workspace and outside private agent state.
pub fn resolve(workspace: &Path, private: &Path, name: &str) -> Result<PathBuf, String> {
    let joined=workspace.join(name);
    let resolved=if joined.exists() { fs::canonicalize(&joined) } else {
        fs::canonicalize(joined.parent().ok_or("Invalid path")?).map(|parent| parent.join(joined.file_name().unwrap_or_default()))
    }.map_err(|error| error.to_string())?;
    if !resolved.starts_with(workspace) || resolved.starts_with(private) { return Err("This path is outside the selected workspace or contains private Slax AI state.".into()); }
    Ok(resolved)
}

// Read text with a strict byte limit; binary files are not sent to the model.
pub fn read_text(path: &Path) -> Result<String, String> {
    let file=fs::File::open(path).map_err(|error| error.to_string())?;
    if !file.metadata().map_err(|error| error.to_string())?.is_file() { return Err("Not a regular file.".into()); }
    let mut data=Vec::new();
    file.take(16385).read_to_end(&mut data).map_err(|error| error.to_string())?;
    if data.len()>16384 { return Err("File exceeds the 16 KB text limit. Use an approved command to inspect a smaller section.".into()); }
    String::from_utf8(data).map_err(|_| "File is not UTF-8 text.".into())
}

// Apply an approved change atomically, preserving permissions and a private original copy.
pub fn write_text(path: &Path, original: Option<&str>, content: &str, private: &Path) -> Result<Value, String> {
    let current=if path.exists() { Some(read_text(path)?) } else { None };
    if current.as_deref()!=original { return Err("The file changed after the preview. Ask for a fresh preview.".into()); }
    if content.len()>16384 { return Err("File exceeds the 16 KB edit limit.".into()); }
    let id=Uuid::new_v4().to_string();
    let backup=private.join("backups").join(&id);
    fs::create_dir_all(&backup).map_err(|error| error.to_string())?;
    fs::write(backup.join("record.json"),json!({"path":path,"original":current}).to_string()).map_err(|error| error.to_string())?;
    let temporary=path.with_file_name(format!(".slax-ai-{id}"));
    let mut file=fs::OpenOptions::new().write(true).create_new(true).open(&temporary).map_err(|error| error.to_string())?;
    let mode=fs::metadata(path).map(|metadata| metadata.permissions().mode()).unwrap_or(0o644);
    file.set_permissions(fs::Permissions::from_mode(mode)).map_err(|error| error.to_string())?;
    if let Ok(metadata)=fs::metadata(path) {
        if unsafe {libc::geteuid()}==0 && unsafe {libc::fchown(file.as_raw_fd(),metadata.uid(),metadata.gid())}!=0 {let _=fs::remove_file(&temporary);return Err("Could not preserve the file owner.".into());}
    }
    file.write_all(content.as_bytes()).and_then(|_| file.sync_all()).map_err(|error| error.to_string())?;
    if let Err(error)=fs::rename(&temporary,path) { let _=fs::remove_file(temporary);return Err(error.to_string()); }
    Ok(json!({"written":path,"backup_id":id,"note":"A private backup is available. Shell and disk operations are not reversible through file backups."}))
}

// Run only this command's process group, with bounded output, cancellation and a deadline.
pub fn command(program: &str, args: &[&str], workspace: &Path, stopped: impl Fn() -> bool) -> Result<Value, String> {
    let mut child=Command::new(program).args(args).current_dir(workspace).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped()).process_group(0).spawn().map_err(|error| error.to_string())?;
    let stdout=child.stdout.take().unwrap();
    let stderr=child.stderr.take().unwrap();
    let out=thread::spawn(move || { let mut data=Vec::new();let _=stdout.take(16384).read_to_end(&mut data);data });
    let err=thread::spawn(move || { let mut data=Vec::new();let _=stderr.take(8192).read_to_end(&mut data);data });
    let start=Instant::now();
    let mut interrupted=false;
    let status=loop {
        if stopped() || start.elapsed()>Duration::from_secs(60) {
            interrupted=true;
            // The child owns this freshly created group; no unrelated processes are signalled.
            unsafe { libc::kill(-(child.id() as i32),libc::SIGKILL); }
        }
        if let Some(status)=child.try_wait().map_err(|error| error.to_string())? { break status; }
        thread::sleep(Duration::from_millis(100));
    };
    unsafe { libc::kill(-(child.id() as i32),libc::SIGKILL); }
    let output=String::from_utf8_lossy(&out.join().unwrap_or_default()).into_owned();
    let errors=String::from_utf8_lossy(&err.join().unwrap_or_default()).into_owned();
    Ok(json!({"exit_code":status.code(),"stdout":output,"stderr":errors,"interrupted":interrupted,"output_limit":24576}))
}

// Fixed diagnostics cannot execute model-supplied shell syntax.
pub fn diagnose(section: &str, stopped: impl Fn() -> bool) -> Result<Value, String> {
    let commands: Vec<(&str,Vec<&str>)>=match section {
        "system"=>vec![("uname",vec!["-a"]),("cat",vec!["/etc/os-release"]),("free",vec!["-h"]),("findmnt",vec!["/","-o","TARGET,SOURCE,FSTYPE,OPTIONS"])],
        "disks"=>vec![("lsblk",vec!["-o","NAME,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINTS"]),("df",vec!["-hT"]),("findmnt",vec!["-o","TARGET,SOURCE,FSTYPE"])],
        "network"=>vec![("ip",vec!["-brief","address"]),("ip",vec!["route"]),("connmanctl",vec!["state"])],
        "graphics"=>vec![("lspci",vec!["-nn"]),("systemctl",vec!["status","xorg.service","--no-pager","--lines=15"])],
        "services"=>vec![("systemctl",vec!["--failed","--no-pager"]),("journalctl",vec!["-b","-p","err","--no-pager","-n","30"])],
        _=>return Err("Unknown diagnostic section.".into())
    };
    let results: Vec<Value>=commands.iter().map(|(program,args)| json!({"command":program,"result":command(program,args,Path::new("/"),&stopped).unwrap_or_else(|error|json!({"error":error}))})).collect();
    Ok(json!(results))
}

#[cfg(test)]
mod tests {
    use super::*;
    // Paths cannot escape through traversal or a symlink into private state.
    #[test]
    fn restrict_paths() {
        let root=std::env::temp_dir().join(format!("slax-ai-test-{}",Uuid::new_v4()));
        fs::create_dir_all(root.join("private")).unwrap();
        fs::write(root.join("private/token"),"secret").unwrap();
        std::os::unix::fs::symlink(root.join("private/token"),root.join("link")).unwrap();
        assert!(resolve(&root,&root.join("private"),"../escape").is_err());
        assert!(resolve(&root,&root.join("private"),"link").is_err());
        assert!(resolve(&root,&root.join("private"),"new.txt").is_ok());
        fs::remove_dir_all(root).unwrap();
    }
    // Approval previews cannot overwrite a file that changed meanwhile.
    #[test]
    fn detect_changed_file() {
        let root=std::env::temp_dir().join(format!("slax-ai-test-{}",Uuid::new_v4()));
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("test.txt"),"changed").unwrap();
        assert!(write_text(&root.join("test.txt"),Some("old"),"new",&root.join("private")).is_err());
        assert_eq!(fs::read_to_string(root.join("test.txt")).unwrap(),"changed");
        fs::remove_dir_all(root).unwrap();
    }
    // Stop kills an executing command's group instead of waiting for its full timeout.
    #[test]
    fn cancel_running_command() {
        let start=Instant::now();
        let result=command("/bin/sh",&["-c","sleep 10"],Path::new("/"),||start.elapsed()>Duration::from_millis(200)).unwrap();
        assert_eq!(result["interrupted"],true);
        assert!(start.elapsed()<Duration::from_secs(3));
    }
}
