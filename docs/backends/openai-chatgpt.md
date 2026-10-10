# Sign in with ChatGPT (openai provider)

`tny --provider openai login` signs in with **Sign in with ChatGPT** (SIWC)
and sends the builtin `openai` profile's requests on the user's ChatGPT plan
([ADR 0186](../adr/0186-sign-in-with-chatgpt-openai-provider.md)). The access
token is an ordinary bearer for the public Responses API,
`https://api.openai.com/v1/responses`; tny's loop, tools, MCP, permissions and
sessions are unchanged. No Codex CLI, no `chatgpt.com/backend-api`, no vendor
executable.

```sh
tny --provider openai login      # browser sign-in; prints the link
tny --provider openai ask "run the tests"
tny status                       # auth: ok (ChatGPT plan, signed in as …)
tny --provider openai logout     # revoke; keep the app registration
tny --provider openai logout --forget
```

The legacy [`codex` profile](codex.md) keeps its own login and the ChatGPT
backend. Speech, images, web search, dictation and the `/status` weekly
allowance still use the Codex login: they live on `chatgpt.com/backend-api`,
which a SIWC token must never reach.

Canonical sources: the SIWC developer docs
(`developers.openai.com/siwc/llms.txt`) and Pi's `openai` provider; see
[sources.md](../sources.md#sign-in-with-chatgpt-openai-provider-adr-0186).

## Sign-in

Authorization Code + PKCE (S256) against `https://auth.openai.com`, with
dynamic client registration. `tny --provider openai login` prints

```text
Continue with ChatGPT — open this link to sign in:
```

and the authorize URL, tries to open the system browser, and waits for the
callback on `http://127.0.0.1:1455/auth/callback` (a busy 1455 falls back to an
OS-assigned port; the printed `redirect_uri` names it). When the browser is on
another machine, paste the full redirect URL into the terminal instead. There
is no device-code flow for this client: `login --device` exits 2 with that
explanation. Ctrl-C aborts (130); the attempt times out after 10 minutes.

| Request | Value |
| --- | --- |
| `client_id` | `dynamic_agent_client` with `agent_name_hint=tny` on the first sign-in; the issued `oaiapp_…` id plus `login_hint` afterwards |
| `ext_agent_host_id` | this host's `urn:uuid:<v4>`, created once in `~/.tny/openai-host-id` |
| `scope` | `openid profile email offline_access resource.invoke chatgpt.tokens.use.direct` |
| `resource` | `https://api.openai.com/v1` |
| `state`, `nonce`, `code_challenge` | fresh per attempt |
| `prompt=consent` | only when the saved sign-in declined plan use |

A callback must carry this attempt's `state` exactly once. The first sign-in
also needs the issued `client_id`; a reauthorization may omit it but never
names a different one (`client_id_mismatch` ends the attempt). A
state-matched `error=` (for example `access_denied`) ends the attempt with
nothing exchanged. Duplicate or unrelated requests get a 400 and the listener
keeps waiting.

The code is exchanged at `/api/accounts/oauth/token` (form body). The response
must carry `access_token`, `refresh_token` and `scope`, and an `id_token`
whose `iss`, `aud` (string or array containing the client id), `exp` (300 s
skew), `nonce` and `sub` check out. The token comes straight from the issuer
over TLS, so tny checks claims, not JWKS signatures. A reauthorization that
returns a different `sub` is refused: run `logout --forget` to switch
accounts.

If the granted scope lacks `chatgpt.tokens.use.direct`, the sign-in is kept
but marked `plan_usage: false`: tny sends nothing on the plan and says so; the
next `login` asks again with `prompt=consent`.

## Store, refresh, logout

`~/.tny/openai-auth.json` (`0600`, atomic writes) follows the SIWC credential
record: issuer, issued client id, subject, email, host id, tokens, scopes,
`plan_usage`, `expires_at`. Tokens never print.

- **Refresh** — access tokens last about an hour. Before each provider request
  tny refreshes when the token is within 180 s of expiry
  (`grant_type=refresh_token`, `client_id`, `refresh_token`, `resource`; no
  scope). Refresh tokens rotate and are single-use, so the refresh holds an
  flock on `~/.tny/openai-auth.lock` and re-reads the store under it. Terminal
  grant errors (`invalid_grant` and kin) clear the tokens and keep the
  registration; network and 5xx failures keep the session for the next try.
- **Ended mid-session** — when a request finds the sign-in gone (signed out in
  another terminal, refresh refused, plan use withdrawn), the turn ends with an
  auth error naming `tny --provider openai login`; the token is never sent
  again.
- **Logout** — revokes the refresh token at `/api/accounts/oauth/revoke`
  (`token_type_hint=refresh_token`), then clears the tokens. The issued client
  id, subject and email stay so the next sign-in reuses the registration. If
  revocation cannot be confirmed, tny says to disconnect tny in ChatGPT
  Settings. `--forget` deletes the store; the host id survives both.

## Precedence and guards

- `OPENAI_API_KEY` (or the profile's `api_key_env`, or `--api-key-env`) beats
  the sign-in. `login` warns when a key is set.
- The plan token goes only to the default endpoint
  (`https://api.openai.com/v1`) on the Responses wire with
  `Authorization: Bearer`. `OPENAI_BASE_URL`/settings `base_url`,
  `--base-url`, `--wire-api chat` or a custom auth header turn it off, also
  when applied after resolution: tny fingerprints the installed token and drops
  it when the endpoint moves or the key is replaced.
- Auto-detection prefers a usable openai sign-in over a Codex login when no
  provider is selected ([cli.md](../cli.md#provider-selection)).
- Test overrides must be numeric loopback URLs:
  `TNY_OPENAI_OAUTH_ISSUER`, `TNY_OPENAI_SIGNIN_BASE_URL`, plus
  `TNY_OPENAI_CALLBACK_PORT`. Any other value fails closed. With an issuer
  override set, tny does not open a browser.

## Requests on the plan

The plan route accepts a subset of Responses. In sign-in mode tny sends
`stream: true`, `store: false`, flat function tools, and no
`max_output_tokens`; system instructions and the compaction summary ride as
`developer` input items because `role: "system"` items are rejected. The
default model is `gpt-6.1-sol`; `tny --provider openai models` lists the
plan's catalog (`{"models":[…]}` is normalized like the codex catalog).

The same shaping applies when a non-`sk-` bearer goes to
`https://api.openai.com` (Pi's rule): a background job child, an SDK caller or
an `OPENAI_API_KEY` holding a plan token.

## Errors

Plan errors are matched on the exact `error.code`, never on message text:

| `error.code` | tny says / does |
| --- | --- |
| `subscription_sharing_usage_limit_exceeded` | plan usage limit reached; no retry; links `chatgpt.com/settings/usage` |
| `subscription_sharing_usage_unavailable`, `subscription_sharing_user_unavailable` | temporarily unavailable; retried with backoff |
| `subscription_sharing_user_not_eligible` | not available for this account, workspace or policy; signing in again will not help, use an API key |
| `subscription_sharing_route_not_supported` | the plan does not serve this endpoint |
| `chatpass_v2_scope_not_authorized`, `chatpass_v2_invalid_authorization_context` | the sign-in does not authorize this request; sign in again and allow plan use |
| `subscription_sharing_invalid_user` | sign in again |
| `subscription_sharing_unsupported_capability` | names `error.param` (sanitized to `[A-Za-z0-9_.[]-]`) |

In sign-in mode a bare 401 says to sign in again and a bare 403 says the plan
refused the request. A `{"detail":"…"}` body is shown as diagnostic text only.

## Status, doctor, models

- `tny status` — `auth: ok (ChatGPT plan, signed in as EMAIL)` and
  `usage: manage at https://chatgpt.com/settings/usage`; `--json` adds
  `"chatgpt_plan": {"account": EMAIL|null, "manage_usage_url": URL}`. The
  weekly allowance remains a Codex-login feature.
- `tny doctor` — `openai: Using ChatGPT plan (signed in as EMAIL), manage
  usage: URL`. When a stored sign-in is not in use (another endpoint, no key),
  it says the sign-in applies to `https://api.openai.com/v1` when no API key
  is set.

## Subagents and jobs

Native subagent children re-read the store themselves (the token is not
passed on argv or env) and refresh under the same lock. Session runners
restore the sign-in from the checkpoint (`openai_signin` and the token
fingerprint) and re-read the store before their first request. Background
jobs carry the access token current at launch in their private job record and
do not refresh it: a job that outlives the token (about an hour) fails with an
authentication error and must be rerun.

## wasm

Turns work through browser fetch when the endpoint's CORS allows it. The login
callback listener is native-only; on a tty the wasm build prints the link and
accepts the pasted redirect URL. Sign-in state lives in the virtual home like
any other store.
