# ADR 0186: Sign in with ChatGPT for the openai provider

Status: accepted. Date: 2026-10-10.

## Context

ChatGPT-plan use in tny went through the `codex` profile
([ADR 0065](0065-codex-chatgpt-responses-backend.md),
[ADR 0066](0066-native-chatgpt-login-and-credential-sources.md)): the Codex
CLI's OAuth client, its `auth.json`, and the private
`chatgpt.com/backend-api/codex` Responses backend.

OpenAI now publishes **Sign in with ChatGPT** for open-source agents
(`developers.openai.com/siwc`). An agent host registers its own client
dynamically, the user grants `chatgpt.tokens.use.direct`, and the resulting
access token is an ordinary bearer for the public Responses API at
`https://api.openai.com/v1`. Pi ships this as its `openai` provider.

The request was to move tny's login from `codex` to `openai` via SIWC.

## Decision

The builtin `openai` profile gains `tny --provider openai login` (SIWC).
`codex` stays, as a legacy profile.

Why keep `codex`: speech, image generation, web search, dictation and the
`/status` weekly allowance all live on `chatgpt.com/backend-api`. A SIWC token
is scoped to `api.openai.com/v1` and must not be sent there, and the public API
has no equivalents on the plan. Removing `codex` would remove those features.
Existing Codex logins keep working. Auto-detection prefers a usable openai
sign-in over a Codex login.

### Sign-in

- Browser Authorization Code + PKCE (S256) with dynamic registration:
  `client_id=dynamic_agent_client` and `agent_name_hint=tny` first, the issued
  `oaiapp_…` id plus `login_hint` afterwards. `ext_agent_host_id` is a
  per-host `urn:uuid:v4` in `~/.tny/openai-host-id`. Scope
  `openid profile email offline_access resource.invoke chatgpt.tokens.use.direct`,
  `resource=https://api.openai.com/v1`, fresh `state` and `nonce`.
  `prompt=consent` only after a declined plan grant. No `id_token_hint`:
  SIWC's reauthorization uses `login_hint`.
- Callback on `127.0.0.1:1455/auth/callback`, falling back to an OS port; a
  pasted redirect URL works for remote browsers. No device-code flow exists
  for this client, so `login --device` exits 2 and says so.
- Callbacks: this attempt's `state` exactly once; registration requires the
  issued `client_id`; reauthorization may omit it but a different one is
  `client_id_mismatch` and ends the attempt; a state-matched `error=` ends the
  attempt without an exchange; duplicates and strays get a 400 and the
  listener keeps waiting.
- Exchange requires `access_token`, `refresh_token` and `scope`. ID-token
  claims checked: `iss`, `aud` (string or array), `exp` (300 s skew), `nonce`,
  `sub`. **No JWKS validation**: the token arrives directly from the issuer
  over TLS (OIDC Core 3.1.3.7), and a JWKS client would add a JOSE stack for no
  security gain here. A reauthorization returning another `sub` is refused
  (`logout --forget` switches accounts).
- No `chatgpt.tokens.use.direct` in the granted scope: the sign-in is stored
  with `plan_usage: false` and never used for requests.

### Store, refresh, logout

`~/.tny/openai-auth.json` (`0600`, atomic) follows the SIWC credential record.
Refresh runs before a request within 180 s of expiry, under an flock with a
re-read, because refresh tokens rotate and a reused one is revoked. The form
carries `grant_type`, `client_id`, `refresh_token` and `resource`, no scope.
Terminal grant errors clear the tokens and keep the registration. Logout
revokes the refresh token (RFC 7009) and keeps the registration; `--forget`
deletes the store. The host id survives both.

A sign-in that ends mid-session (signed out elsewhere, refresh refused, plan
use withdrawn) ends the turn with an auth error rather than sending a stale
or absent credential.

### Where the token may go

- An API key (`OPENAI_API_KEY`, `api_key_env`, `--api-key-env`) wins.
- The token is installed only for the default endpoint on the Responses wire
  with `Authorization: Bearer`. A token fingerprint (fnv1a) detects a key
  replaced after resolution; a moved endpoint or chat wire drops the token.
- Test overrides (`TNY_OPENAI_OAUTH_ISSUER`, `TNY_OPENAI_SIGNIN_BASE_URL`,
  `TNY_OPENAI_CALLBACK_PORT`) accept only numeric-loopback URLs, matching
  `TNY_GROK_BASE_URL`. A store from another issuer is ignored.

### Request shape on the plan

`stream: true`, `store: false`, flat function tools, no `max_output_tokens`,
and `developer` instead of `system` input items (the plan route rejects
`role: "system"`). Default model `gpt-6.1-sol`. Following Pi, a non-`sk-`
bearer sent to `https://api.openai.com` gets the same shaping, which covers
subagent children, job children and SDK callers that hold a plan token.

### Errors

Exact `error.code` matches: usage limit (no retry, usage link), unavailable
(retry with backoff), not eligible, route not supported, grant
(`chatpass_v2_*`), invalid user (sign in again), unsupported capability (names
a sanitized `error.param`). In sign-in mode bare 401/403 get sign-in-specific
text. A `{"detail": …}` body is diagnostic text only. Provider message text is
never matched.

### Processes

Checkpoints carry `openai_signin` and the token fingerprint (private section
only; never in the public view); a restored session runner re-reads the store
before its first request. Subagent children receive no key and re-read the
store themselves. **Known limitation**: background jobs carry the access token
current at launch and do not refresh it, so a job running longer than about an
hour fails with an authentication error. The per-request refresh is a blocking
call that Ctrl-C does not interrupt, and a 401 is not followed by an automatic
refresh-and-retry.

### wasm

Turns work through browser fetch subject to CORS. The loopback callback
listener is native-only; on a tty the wasm build accepts the pasted redirect
URL. `tny_poll` governs all waits.

## Consequences

- `tny --provider openai login` is the documented way to use a ChatGPT plan;
  `codex login` remains for the backend-api features and existing users.
- One more credential file under `~/.tny`; doctor, status and models report
  the sign-in without printing tokens.
- SIWC is a preview: error codes and scopes may move. Codes live in one table
  (`oa_plan_codes` in `openai.c`); sources are pinned in
  [sources.md](../sources.md#sign-in-with-chatgpt-openai-provider-adr-0186).

## Verification

- Unit: resolution and precedence (store, API key, base URL, moved endpoint,
  replaced key, chat wire, signed-out store, declined plan, foreign issuer,
  loopback overrides), the Pi heuristic, ID-token claims, callback parsing
  including duplicates, plan error codes and param sanitizing, and developer
  input items. Checkpoint ownership covers the two new fields.
- Integration (`tests/integration/test_openai_signin.py`): a local issuer and
  API mock covering registration, ask, status/models, endpoint guards,
  refresh (including a terminal `invalid_grant` that ends the turn),
  reauthorization (mismatched id, omitted id, different account),
  `access_denied`, consent after a declined grant, plan errors, logout and
  `--forget`. The wasm CI job runs it against `build/wasm/tny`, which seeds
  the store and skips the browser parts; that path was not run locally (no
  emsdk on the authoring host).
- Live, explicitly authorized (2026-10-10, native macOS release build of the
  branch): `tny --provider openai login` against `auth.openai.com` completed
  dynamic registration (an `oaiapp_…` client id), granted
  `chatgpt.tokens.use.direct` and wrote a `0600` store with `plan_usage: true`.
  One `tny --provider openai --model gpt-6-luna ask` in an empty workspace with
  no `OPENAI_API_KEY` returned exactly the requested text: exit 0, about 3 s,
  no stderr. `tny status` reported the plan and the usage page. Refresh,
  revocation and the plan error codes were not exercised live.
- Mutation (`tests/mutation/mutate.py --only FILE --test FILTER`, no
  timeouts):
  - `oauth_callback_parse` (`--test oauth_callback`): all 10 valid mutants
    killed by unit tests (3 uncompilable). The first run's one survivor
    accepted an empty `code` (`!code || !*code` to `&&`); a missing-code and
    an empty-code case now kill it.
  - `openai_auth.c` (`--test openai`): 79 valid mutants, 69 killed by unit
    tests and 2 by the sign-in suite (13 uncompilable). The first run closed
    real gaps: loopback ports that start with 9, port 65535, an empty or
    non-numeric port, a NULL context or base URL in the Pi heuristic, and the
    never-read `has_refresh` field, which was removed.
  - The 8 survivors are accepted:
    - equivalent: ID-token `exp <= 0` to `< 0` (an `exp` of 0 still fails the
      skew check); sync `expires_at > 0` to `>= 0` (0 still re-reads the
      store); sync `!ctx || !signin` to `&&` (no caller passes NULL); port
      digit test `||` to `&&` (a non-digit port parses as 0 and is refused);
    - one-second boundaries: ID-token expiry `<` to `<=` and the refresh
      window `<` to `<=`;
    - a leading-zero port such as `:080` (`< '0'` to `<= '0'`), safe either
      way;
    - a store path that cannot resolve (no home directory).
  - `--test` is needed because the harness gives each mutant's unit run 60 s,
    while the full unit binary takes about 70 s on the authoring host (most of
    it waits, not CPU). Without the filter, 67 of the 93 `openai_auth.c`
    mutants and 7 of the 13 `oauth_callback_parse` mutants timed out. One of
    those timeouts (`state` `!=` to `==`) was reproduced by hand and
    `oauth_callback_rules` fails it.
