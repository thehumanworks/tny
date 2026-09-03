# 0066 — Native ChatGPT login, a tny-managed credential store, and file-less credentials

Date: 2026-09-03
Status: accepted (amends [ADR 0065](0065-codex-chatgpt-responses-backend.md)
decision 4; supersedes the codex part of [ADR 0019](0019-subscription-logins-claude-grok.md)
decision 4)

## Context

ADR 0065 moved `--provider codex` onto the ChatGPT Responses backend but
kept the sign-in ceremony in the Codex CLI: `tny --provider codex login`
was `system("codex login")`, and the only credential source was the file
that CLI writes. Three things were wrong with that:

- **The CLI became a dependency again**, for the one thing tny no longer
  needed it for. A multi-MiB Rust binary had to be installed to mint a
  token that tny then used on its own.
- **tny could not manage the credential.** Refreshing meant rewriting the
  Codex CLI's file behind its back, logout meant deleting a file another
  tool owns, and a user who wanted tny and the CLI on different accounts
  had no way to say so.
- **A file was mandatory.** Containers, CI runners, the browser wasm
  build, and "run tny once with this token" scripts have no `~/.codex`
  and should not need to fabricate one.

Both reference implementations pin the flow tny needs — the Codex CLI
(`codex-rs/login`: `server.rs`, `device_code_auth.rs`, `auth/manager.rs`)
and pi (`packages/ai/src/utils/oauth/openai-codex.ts`):

- **Browser (PKCE)**: `GET https://auth.openai.com/oauth/authorize` with
  `response_type=code`, the public client id `app_EMoamEEZ73f0CkXaXp7hrann`,
  `redirect_uri=http://localhost:1455/auth/callback` (1457 is the CLI's
  fallback; both are on the provider's allow-list), `scope=openid profile
  email offline_access`, `code_challenge=S256(verifier)`, `state`,
  `id_token_add_organizations=true`, `codex_cli_simplified_flow=true`,
  `originator`. The callback carries `code` + `state`; `POST /oauth/token`
  with `grant_type=authorization_code`, `code`, `redirect_uri`,
  `client_id`, `code_verifier` (form encoded) returns `id_token`,
  `access_token`, `refresh_token`, `expires_in`.
- **Device code**: `POST {issuer}/api/accounts/deviceauth/usercode`
  `{client_id}` → `{device_auth_id, user_code, interval}`; the user opens
  `{issuer}/codex/device`; poll `POST …/deviceauth/token`
  `{device_auth_id, user_code}` — 403/404 (or
  `deviceauth_authorization_pending`) means keep polling, 200 returns
  `{authorization_code, code_verifier}`; exchange as above with
  `redirect_uri={issuer}/deviceauth/callback` and the server-issued
  verifier. 15-minute ceiling.
- **Refresh**: `POST /oauth/token` `{client_id, grant_type:
  "refresh_token", refresh_token}`; the CLI refreshes when the access
  token's `exp` has passed or `last_refresh` is older than eight days, pi
  when `expires` is within a skew window. The account id is the
  `https://api.openai.com/auth`.`chatgpt_account_id` claim of the access
  token.

## Decision

1. **Credential precedence is fixed and explicit** (`src/core/codex_auth.c`):

   1. `--chatgpt-token TOKEN` (+ `--chatgpt-account-id ID`) — CLI flags;
   2. `CHATGPT_ACCESS_TOKEN` (+ `CHATGPT_ACCOUNT_ID`) — environment;
   3. `~/.tny/codex-auth.json` — tny's own store;
   4. `$CODEX_HOME/auth.json` — the Codex CLI's file (`codex login`),
      including its API-key mode.

   1 and 2 need no filesystem: they are the credential for containers,
   CI, the browser wasm build, and one-off runs. The account id is taken
   from the flag/env when given, else derived from the access token's
   JWT claim (then the id token's); an opaque token with no derivable id
   still runs, without the `chatgpt-account-id` header. Any source
   present auto-detects the `codex` provider in the ADR 0019 order.
   Flag and env tokens carry no refresh token and are never refreshed.

2. **tny signs in natively** (`src/core/codex_login.c`), no Codex CLI:
   `tny --provider codex login` runs the browser PKCE flow — tny listens
   on `127.0.0.1:1455` (1457 fallback, `TNY_CODEX_CALLBACK_PORT` for
   tests), prints and opens the authorize URL with `originator=tny`,
   verifies `state`, answers the browser with a plain "signed in" page,
   and also accepts the redirect URL or bare code **pasted on the
   terminal** (the box whose browser is elsewhere, as pi does). `login
   --device` runs the device-code flow. The verifier is 32 CSPRNG bytes
   base64url (43 chars, RFC 7636), the challenge `S256`; `sha256`,
   `random_bytes`, `b64url_encode`, and `url_form_append` join `util`.
   `TNY_CODEX_OAUTH_ISSUER` redirects every endpoint at a mock.

3. **The store is tny's, in the Codex CLI's shape.** Success writes
   `~/.tny/codex-auth.json` (0600, atomic) as
   `{auth_mode, OPENAI_API_KEY:null, tokens{id_token, access_token,
   refresh_token, account_id}, last_refresh, expires_at}` — the same
   parser reads both files, and `expires_at` (from `expires_in`, which
   the CLI discards) gives a refresh signal even for opaque tokens.
   `tny --provider codex logout` deletes this file only and says so when
   the Codex CLI's file or `CHATGPT_ACCESS_TOKEN` still supplies a
   credential.

4. **Refresh happens on the file that wins.** At provider resolve tny
   refreshes tny's store if it exists, else the Codex CLI's file, each
   **in place**: stale means the access token's `exp` (else `expires_at`)
   is within 60 s, or `last_refresh` is older than eight days. Rewriting
   the CLI's file is exactly what the CLI itself does with a rotated
   refresh token, so tny and the CLI keep sharing that session; tny's own
   store never touches it and vice versa. Failures are silent
   (`TNY_DEBUG=1` explains) and the stale token flows; the provider's 401
   names the fix. `CODEX_REFRESH_TOKEN_URL_OVERRIDE` (the CLI's own
   override) still points the grant at a test endpoint.

5. **Reporting names the source.** `tny providers` / `doctor` say which
   source resolved (`--chatgpt-token`, `CHATGPT_ACCESS_TOKEN`,
   `~/.tny/codex-auth.json`, `$CODEX_HOME/auth.json`); the connect error
   lists all three ways in. Tokens never print.

## Consequences

- A ChatGPT subscriber with nothing installed but tny runs
  `tny --provider codex login` (or `login --device` over SSH) and is
  done; `codex login` users change nothing. Scripts run
  `CHATGPT_ACCESS_TOKEN=… tny ask …` on a read-only filesystem.
- Removed: `--codex-bin`, `TNY_CODEX_BIN`, `ctx->codex_bin`, and every
  `system("codex …")` call. The `codex` host line in `doctor` now reports
  the credential state.
- Secrets on argv are visible in process listings; `--chatgpt-token` is
  documented as the last resort behind the env var, matching how the
  other providers treat `--api-key-env` vs stored keys.
- The browser flow needs a listening socket: native only. wasm gets the
  device flow (plain HTTPS) and the flag/env sources; the docs table says
  so and the wasm CI job runs the same integration file minus the
  callback run.
- Tests: `tests/test_util.c` (SHA-256 FIPS vectors, RFC 7636 appendix B,
  base64url, form encoding, CSPRNG), `tests/test_core.c`
  `codex_credential_precedence` (every source and override, store
  round-trip, logout), and `tests/integration/test_codex_chatgpt.py`
  against a mock issuer: flag/env/store/CLI-file turns with header
  assertions, both refresh paths and their in-place rewrites, the device
  flow (pending polls, server verifier), the browser flow (authorize URL
  parameters, bad state → 400 and keep waiting, 404 on other paths,
  PKCE verifier matches the challenge, pasted URL on a pty), logout, and
  the no-credential error.
- Risk: OpenAI can change the login endpoints (they are the Codex CLI's,
  not a public API). The flow is isolated in one file with the endpoint
  constants at the top; the flag/env sources are the fallback that never
  depends on them.
