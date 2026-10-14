# Slax AI client

A native Rust agent with an embedded browser UI. The release is a static Linux
amd64 musl executable. Slax does not need Rust, Cargo, Python, Node.js, OpenSSL,
or downloaded JavaScript libraries to run it. Chromium displays the UI.

```sh
slax-ai --open
```

Set the HTTPS backend address in Settings, sign up, verify your email, and choose
a workspace. Model access and billing are handled by `../../../server`.
The backend must be deployed and configured before real AI requests can work.

## Build

The pinned Rust toolchain and `Cargo.lock` are build dependencies only.

```sh
rustup target add x86_64-unknown-linux-musl
CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER=musl-gcc cargo build --release --locked --target x86_64-unknown-linux-musl
cargo test --locked --target x86_64-unknown-linux-musl
readelf -d target/x86_64-unknown-linux-musl/release/slax-ai
```

Install musl-tools on the build host. `readelf` must show no NEEDED libraries.
`../modules/06-ai/compile` builds the executable in the disposable application
branch and exports a small Debian package; compilers never enter the runtime module.
The `copy` phase installs the launcher/icon and refreshes the final xlunch caches.

## Runtime behavior

The service binds only to 127.0.0.1 on a free port. A random per-launch capability,
strict Host/Origin checks and no CORS protect local actions. Remote login tokens
stay in native state, not browser JavaScript. Root launches Chromium through
`fbliveapp` as guest. Closing the browser leaves the local service running;
the launcher reopens that existing instance.

Fixed diagnostics are read-only. File tools resolve symlinks and stay inside the
selected workspace, excluding private agent state. Every file write and every
shell command requires an explicit local approval. File previews are checked again
before writing; previous text is backed up privately. Shell/disk changes have no
general undo. Commands run with the service user's permissions, a 60-second limit
and bounded output; their background descendants are terminated at completion.

The composer remains available during a task. Additional instructions are applied
at the next model/tool boundary. A pending approval is invalidated by steering.
This does not inject input into a model's in-flight HTTP inference. Stop prevents
further actions and kills an active command's process group; the gateway still
settles already accepted model usage.

History, backend configuration and the session token live in
`$XDG_STATE_HOME/slax-ai` or `~/.local/state/slax-ai`, with owner-only permissions.
Backups are under its `backups` subdirectory. Slax must have persistent changes
enabled to retain these files across reboot. No machine-specific settings or
credentials belong in the distribution ISO.

For local development only, `--allow-http` permits a localhost HTTP backend.
`--state-dir` isolates test state, and `--port` fixes a port for an SSH tunnel.
The UI and knowledge are embedded, so changing them requires rebuilding the binary.

Current limits: text-only chat, five local tools, 30 model steps per task, and
16 KB individual file edits. Additional providers are backend adapters; native
GUI/computer automation and automatic context compaction are not implemented.
