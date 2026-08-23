# 0021 — Native grok device-code login (no grok CLI required)

Status: accepted. Amends [ADR 0019](0019-subscription-logins-claude-grok.md)
decision 4 for the grok profile.

## Context

ADR 0019 kept login ceremonies in the host CLIs: `tny --provider grok login`
was `system("grok login --device-auth")`, so signing in to the grok
subscription required installing the grok CLI (a multi-MiB Rust binary tny
otherwise never runs — the profile speaks OpenAI-compatible HTTP to the CLI
chat proxy directly). That was the only reason to have grok-build installed
at all.

The flow itself is small and documented by grok-build's source (pinned to
its released schema):

- `POST {issuer}/oauth2/device/code` with the form
  `client_id`, `scope`, `referrer=grok-build` →
  `{device_code, user_code, verification_uri[,verification_uri_complete],
  expires_in[, interval]}` (RFC 8628; 404 means the deployment has no
  device endpoint).
- Poll `POST {issuer}/oauth2/token` with
  `grant_type=urn:ietf:params:oauth:grant-type:device_code`, `device_code`,
  `client_id` → `{access_token[, refresh_token, expires_in, id_token]}`,
  or `{error}` ∈ `authorization_pending` (keep polling) / `slow_down`
  (+5 s to the interval) / `access_denied` / `expired_token`.
- Defaults: issuer `https://auth.x.ai`, the grok-build public client id,
  and its frozen ten-scope set (`openid profile email offline_access
  grok-cli:access api:access conversations:read conversations:write
  workspaces:read workspaces:write`).
- Credentials live in `~/.grok/auth.json` as a scope-keyed object:
  `"{issuer}::{client_id}"` → `{key, auth_mode:"oidc", create_time,
  user_id, email?, refresh_token?, expires_at?, oidc_issuer,
  oidc_client_id}`.

One catch: access tokens are short-lived and the grok CLI refreshes them in
the background. A minted login that nobody refreshes dies within the hour,
so "login without the CLI" forces "refresh without the CLI" too.

## Decision

1. **`tny --provider grok login` runs the device flow natively**
   (`src/core/grok_login.c`): request the code, print the verification URL
   + user code, poll, and write the `"{issuer}::{client_id}"` entry into
   `~/.grok/auth.json` **in the grok CLI's own store format** (0600 via
   atomic write), merging with whatever else the file holds. The `id_token`
   payload is decoded (unverified — direct HTTPS channel, display fields
   only, same stance as grok-build) for `user_id`/`email`. Both tools can
   read and refresh the same entry; `TNY_GROK_BIN` is gone.
   `GROK_OAUTH2_ISSUER` / `GROK_OAUTH2_CLIENT_ID` (grok-build's own env
   names) override the endpoint for enterprise IdPs and tests.

2. **tny refreshes the token at provider resolve.** `apply_grok` calls
   `tny_grok_refresh_if_stale()` first: the first store entry carrying
   `refresh_token` + `oidc_issuer` + `oidc_client_id` whose `expires_at` is
   within 60 s runs the `grant_type=refresh_token` exchange against its own
   issuer and persists the rotated tokens (create_time/expires_at updated,
   profile fields kept). Best-effort: any failure leaves the store
   untouched and the stale token flows as before. Entries without
   `expires_at` (30-day-TTL store entries) and fresh entries cost no
   network I/O, so `--help`/`--version` stay fast and resolve stays cheap.

3. **`tny --provider grok logout` is native too:** it removes the legacy
   `https://accounts.x.ai/sign-in` entry and any entry keyed by or issued
   from the active issuer, keeps foreign-issuer entries (enterprise IdPs
   the grok CLI configured), and deletes the file when nothing is left.

4. This narrows ADR 0019's "tny never persists credentials" to: **the only
   credential tny ever writes is the grok store entry (login and refresh),
   in the host CLI's own format and location.** Claude, codex, and cursor
   are unchanged — those hosts still own their ceremonies. Tokens are never
   logged; refresh failures print only under `TNY_DEBUG=1`.

## Consequences

- A grok subscription works from a bare machine: `tny --provider grok
  login`, open the URL on any device, done. SSH/containers keep working
  (the URL prints; no localhost callback is needed).
- The mock-issuer tests in `tests/test_core.c` (`grok_native_*`) cover the
  wire shape (endpoints, form encoding, RFC 8628 grant), the pending →
  success poll, denial, store compatibility, refresh-in-place, and logout
  scoping — no live keys in CI.
- If xAI rotates the public client id or scope set, logins fail loudly at
  the device-code request; the env overrides are the escape hatch until a
  tny release catches up.
- Size: no new dependencies (yyjson + the existing HTTP/1.1 client);
  stripped macOS binary measured 477,648 bytes after the change, well
  under budget.
