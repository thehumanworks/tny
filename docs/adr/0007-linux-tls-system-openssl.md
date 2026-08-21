# 0007 — Linux TLS uses the system OpenSSL, dlopen'd at first use

Date: 2026-08-21
Status: accepted (supersedes the "trimmed mbedTLS client" follow-up in
language-and-runtime.md / size-and-speed.md)

## Context

v1 shipped Linux with no TLS: `nstream_connect(tls=true)` returned
"https not built on this platform yet", so the openai provider (and any
`https://` / `wss://` URL) was unusable on Linux — the default
`https://api.openai.com/v1` base URL failed on first prompt.

The docs penciled in a **trimmed mbedTLS client** as the follow-up. Costs,
measured against what we actually need: a TLS 1.2/1.3 client with X.509 path
validation vendors ~80+ source files (bignum, ECC, AES-GCM, ChaCha, SHA-2,
X.509, TLS state machines), adds roughly 300–500 KiB to the stripped binary,
and makes tny responsible for shipping crypto — every mbedTLS CVE becomes a
tny release. Meanwhile the repo's own rule is "prefer system TLS", which is
exactly how macOS works: SecureTransport, `dlopen`'d at first TLS use so
launch stays framework-free (docs/adr level: size-and-speed.md item 4).

On Linux the system TLS in practice is the distro's OpenSSL shared library:
`libssl.so.3` (OpenSSL 3.x, every current Debian/Ubuntu/Fedora/Alpine/Arch)
or `libssl.so.1.1` (older LTS). It is present wherever a package manager
exists, patched by the distro, and its compiled-in CA paths match the distro.

## Decision

**Linux TLS `dlopen`s the system libssl at first TLS use** — try
`libssl.so.3`, then `libssl.so.1.1`, then `libssl.so` — resolving the ~17
symbols the client needs via `dlsym` (`src/net/stream.c`). Nothing is linked:
`ldd tny` shows no libssl, launch pays zero TLS cost, and the binary grew
~4 KiB total.

This does not violate "Never OpenSSL":

- That rule (language-and-runtime.md bill of materials, size-budget rule)
  forbids **vendoring or static-linking** OpenSSL — pulling its source or
  archives into the binary. Loading the distro's shared library at runtime is
  the Linux twin of dlopen'ing Security.framework on macOS.
- The OpenSSL 1.1.0+ client ABI used here is stable across 1.1/3.x:
  `TLS_client_method`, `SSL_CTX_new/ctrl/set_verify/
  set_default_verify_paths/load_verify_locations`, `SSL_new/set_fd/ctrl/
  set1_host/connect/read/write/get_error/get_verify_result/shutdown/free`.
  Macros (`SSL_set_tlsext_host_name`, `SSL_CTX_set_min_proto_version`) are
  expanded to their `SSL_ctrl`/`SSL_CTX_ctrl` calls with pinned constants.

Policy baked into the shim:

- TLS 1.2 minimum, `SSL_VERIFY_PEER`, SNI, and hostname verification via
  `SSL_set1_host` — there is no insecure toggle.
- CA trust: `SSL_CTX_set_default_verify_paths` (the distro's compiled-in
  paths, plus the standard `SSL_CERT_FILE`/`SSL_CERT_DIR` env overrides).
  When neither env var is set and the default dir yielded nothing usable, a
  fallback probes the common bundle locations
  (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`,
  `/etc/ssl/ca-bundle.pem`, `/etc/ssl/cert.pem`).
- Verification failures report OpenSSL's own reason string
  ("certificate has expired", "hostname mismatch", …).

## Consequences

- `https://` and `wss://` work on every Linux with an OpenSSL shared library
  — i.e. effectively every dynamic-libc install. Measured: stripped glibc
  x86_64 grew 437,432 → 441,544 bytes, still under the 1.0 MiB stretch goal.
- The Linux Makefile adds `-ldl` (a no-op stub on glibc ≥ 2.34 and musl).
- **musl static publish builds still have no HTTPS**: static musl cannot
  `dlopen`. Those builds now fail with an actionable "install libssl" error
  instead of "not built". If static-binary HTTPS ever becomes a requirement,
  vendored mbedTLS (or BearSSL) for the `STATIC=1` build only is the
  recorded fallback plan; it stays off the dynamic builds.
- macOS is unchanged (SecureTransport). Windows/MSYS2 keeps the clean
  "https not built" error until it grows a schannel or libssl path.
- Tests: `tls_to_plain_http_server_fails_cleanly` (tests/test_net.c) and a
  Linux-only end-to-end https tool turn against a TLS-wrapped mock with a
  runtime self-signed cert (tests/integration/test_https.py), including
  proof that an untrusted certificate is rejected, close-delimited EOF with
  and without close_notify, and a blocked multi-megabyte request write.
  `tests/mutation/mutate.py --only stream` kills 8/8 non-annotated mutants.
- Enabling TLS exposed a latent `http1.c` bug: TLS 1.3 session tickets wake
  the fd while `SSL_read` still reports would-block, and `fill()` gave up
  after a single poll+retry. It now loops until its deadline (a plain-TCP
  transport can also legitimately wake with a partial line).
