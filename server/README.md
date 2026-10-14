# Slax AI server

A new Rust backend with the same HTTP/SSE contract as the native Slax client.
The release is one static Linux amd64 musl executable, including HTTP/HTTPS,
SMTP, password hashing, provider adapters and SQLite. The deployment server
does not need Rust, Cargo, Node.js, OpenSSL libraries or a SQLite installation.
API access, email delivery and optional Stripe Checkout remain external services.

## Build

Build on Linux with the pinned toolchain, musl-tools and Cargo.lock:

```sh
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER=musl-gcc
cargo test --locked --target x86_64-unknown-linux-musl
cargo build --release --locked --target x86_64-unknown-linux-musl
readelf -d target/x86_64-unknown-linux-musl/release/slax-ai-server
```

The executable must have no NEEDED libraries. Build dependencies stay on the
build machine. Use a fresh database for this initial Rust implementation.

## Run

Copy the executable, `.env.example` and the desired deployment templates to
`/opt/slax-ai`, outside any public web document root. Run under a dedicated
`slax-ai` user with a private, persistent data directory.

```sh
cd /opt/slax-ai
cp .env.example .env
chmod 600 .env
```

After editing `.env`, create the service account once as root and assign this
deployment directory to it:

```sh
useradd --system --home-dir /opt/slax-ai --shell /usr/sbin/nologin slax-ai
chown -R slax-ai:slax-ai /opt/slax-ai
runuser -u slax-ai -- ./slax-ai-server --env .env --check
runuser -u slax-ai -- ./slax-ai-server --env .env
```

Set `PUBLIC_URL`, `AI_API_KEY`, SMTP settings and `MAIL_FROM`. `--check` validates
configuration and configured TLS files without opening the database, contacting
providers, or sending email. It reports whether API/SMTP settings are present;
it does not verify model access or mail delivery. Never include real secrets in Git.

The default listener is `127.0.0.1:8787`. The client requires an HTTPS backend
URL in normal operation. Set `https://gpt.slax.org` in the client's Settings
after that domain's TLS and routing are configured.

`deploy/slax-ai.service` starts the binary with systemd. Change paths if needed:

```sh
cp deploy/slax-ai.service /etc/systemd/system/slax-ai.service
systemctl daemon-reload
systemctl enable --now slax-ai
curl --fail http://127.0.0.1:8787/health
journalctl -u slax-ai -f
```

The health response is `{"ok":true}`. SIGTERM stops accepting requests and waits
for accepted model requests to settle. The unit's stop timeout accommodates the
maximum configured provider timeout. Run only one server per database; an
exclusive process lock prevents a second instance from recovering live requests.
The trusted credits CLI may use the same database while the server is running.

## Apache HTTPS proxy

Use `deploy/apache.conf` for `gpt.slax.org` after obtaining its valid certificate.
It contains a dedicated HTTPS virtualhost and an HTTP-to-HTTPS redirect.
Keep existing websites and their PHP/MPM configuration intact.

```sh
a2enmod proxy proxy_http headers ssl
cp deploy/apache.conf /etc/apache2/sites-available/03-gpt.slax.org.conf
a2ensite 03-gpt.slax.org
apache2ctl configtest
systemctl reload apache2
```

The `03-` filename loads this exact hostname before the existing `04-slax` wildcard
virtualhost. Apache selects the first matching ServerName/ServerAlias and gives
wildcards no lower priority. [Virtualhost matching](https://httpd.apache.org/docs/2.4/vhosts/name-based.html)

Set `TRUST_PROXY=true` only when the backend listener is accessible exclusively
through the trusted proxy. The template removes incoming X-Forwarded-For and lets
Apache add the actual peer address. Keep `HOST=127.0.0.1`. SSE is excluded from
compression and the proxy timeout is 180 seconds. The existing server certificate
for only `slax.org` and `www.slax.org` does not cover `gpt.slax.org`.

## Native HTTPS without Apache

The same executable can terminate HTTPS through Rustls. Bind an available address
and port; Apache and this service cannot share the same address/port pair.

```dotenv
HOST=0.0.0.0
PORT=443
PUBLIC_URL=https://gpt.slax.org
TRUST_PROXY=false
TLS_CERT_FILE=/opt/slax-ai/tls/fullchain.pem
TLS_KEY_FILE=/opt/slax-ai/tls/privkey.pem
```

Supply a valid certificate and private key readable by the service user. Certificate
issuance and renewal are managed externally; no ACME client is included. After
replacement, reload the certificate with `systemctl reload slax-ai`. A failed reload
keeps the previous certificate active. Native HTTPS needs no Apache or OpenSSL
runtime. The provided unit permits binding privileged ports with CAP_NET_BIND_SERVICE.

## Email and accounts

Registration uses email and a passphrase of at least 12 characters. Passwords use
salted Argon2id PHC records. Password processing runs outside the async HTTP workers.
Sessions use random 256-bit bearer tokens; only SHA-256 token hashes are stored.
Sessions expire after 30 days. Verification/reset codes expire after 15 minutes.
Password reset revokes all prior sessions. Credentials never enter model prompts.

Remote SMTP uses either implicit TLS (`SMTP_SECURE=true`, usually port 465) or
required STARTTLS (`SMTP_SECURE=false`, `SMTP_STARTTLS=true`, usually port 587).
For the existing local sendmail relay, use:

```dotenv
SMTP_HOST=127.0.0.1
SMTP_PORT=25
SMTP_SECURE=false
SMTP_STARTTLS=false
SMTP_USER=
SMTP_PASSWORD=
MAIL_FROM="Slax AI <noreply@slax.org>"
```

Plain SMTP is accepted only for loopback relays. Actual mail delivery must be tested
before public launch. Runtime tests use a fake localhost SMTP server and never send
real mail. Auth and verification attempts are rate-limited; public deployment should
also preserve rate limits across restarts at the proxy.

`WELCOME_CREDITS=20` grants up to 20 credits once after email verification.
`WELCOME_DAILY_LIMIT=1000` caps all introductory grants per UTC day. A verified
account cannot obtain another grant later. Set the welcome amount to zero to disable
the offer. Email verification cannot guarantee one account per person.

## Credits and model billing

100 credits = $1 of prepaid service. Balances and ledger entries use integer
microdollars; 1 credit = 10,000 microdollars. The configurable `PRICE_MULTIPLIER`
defaults to 2; it does not guarantee a profit margin.

Before provider dispatch, the server reserves a conservative upper bound based on
serialized UTF-8 bytes, framing, maximum output and applicable context pricing.
After completion it charges confirmed usage, including cached input and reasoning
output, and refunds the unused reservation atomically. Model options and tools
are server-owned. One model request may be active per account. Idempotency binds
a request ID to its account and normalized conversation and cannot spend twice.

Accepted requests finish and settle even when the user stops or disconnects.
Explicit provider rejection refunds the reservation. Interrupted requests without
confirmed usage retain it, including after a process crash. Reconcile those records
against provider billing and credit the account if appropriate; do not automatically
refund uncertain traffic, which would enable free-inference abuse.

The configured defaults use standard GPT-6.1 Sol rates checked on 2026-10-14.
Responses requests explicitly select the standard service tier. Review every price
when changing model or provider. Compatible providers must enforce output caps
and report reliable usage. Input is capped at MAX_INPUT_BYTES; response events and
the whole upstream stream are bounded.

Trusted operator top-up, with a unique payment reference:

```sh
./slax-ai-server --env .env credits user@example.com 5 unique-payment-reference
```

This adds $5, or 500 credits. Reusing the same reference/amount is safe; a different
amount or account is rejected. A trusted backend can instead POST to
`/v1/admin/credits` using `X-Admin-Token` and JSON with `email`, `amount_microusd`
and `reference`. Never send the admin secret to a client.

## Optional Stripe Checkout

Set `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRET` and the HTTPS `PUBLIC_URL`.
Configure `/v1/payments/webhook` for `checkout.session.completed` and
`checkout.session.async_payment_succeeded`. Verified accounts can choose $1, $5,
$10 or $20; the UI currently offers the first three packages.

The server creates the order and amount. Credits are granted only after a fresh
signed paid webhook matches that saved order's session ID, reference, USD currency
and amount. Raw webhook bytes are authenticated before JSON parsing. Replays
cannot grant twice. Browser success redirects are never payment proof. Test Stripe
test credentials before accepting payments. Refunds/disputes are not automated.

## Providers and data

- `openai-responses`: Responses SSE, strict local tools, `store:false`, reasoning
  summaries and opaque encrypted reasoning continuity.
- `chat-completions`: compatible provider SSE, text/tools and final usage. Set
  `AI_BASE_URL`, `AI_API_KEY`, `AI_MODEL` and all prices for the selected provider.

Both adapters expose the same client contract. Incompatible APIs need a new adapter
in `src/providers.rs`; the local agent need not change. Change provider between
tasks. ChatGPT-account authentication is outside this credit-based implementation.

Incoming complete conversations are forwarded, not stored. Final model responses
are retained for RESULT_RETENTION_HOURS (24 by default), then cleared. Accounts,
ledger, costs and payment references remain in SQLite. The local client stores its
own full chats. Conversations have at most 200 messages, with no automatic context
compaction. Read the selected provider's own retention policy separately.

Keep the database on a persistent local filesystem in a private directory. Back up
using SQLite's online backup API, or stop the service before copying the database
and WAL. Do not put credentials, databases or private TLS keys in a public web root.

## Verification

`cargo test` exercises real localhost HTTP and fake SMTP/provider servers: verified
trial grants, international text, password reset, usage, concurrent reservations,
disconnect settlement, provider switching, reasoning/tool history, operator credits,
signed payment replay protection, output limits, retention and crash recovery.
These tests never call a live AI, payment or mail provider.

An additional integration test runs the unchanged native Slax client, including
its local diagnostic tool loop, against the synthetic Rust gateway:

```sh
SLAX_AI_TEST_CLIENT=/path/to/slax-ai cargo test --locked --target x86_64-unknown-linux-musl existing_native_client_uses_rust_gateway -- --ignored
```

Protocol sources: [function calling](https://developers.openai.com/api/docs/guides/function-calling),
[Responses streaming](https://developers.openai.com/api/docs/guides/streaming-responses),
[OpenAI pricing](https://developers.openai.com/api/docs/pricing),
[Stripe Checkout](https://docs.stripe.com/api/checkout/sessions/create).
