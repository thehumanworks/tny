# Codex (ChatGPT Responses backend)

The `codex` provider drives the user's **ChatGPT subscription** through the
Responses-compatible backend the Codex CLI itself talks to
(`https://chatgpt.com/backend-api/codex`), on tny's **native loop**
([ADR 0065](../adr/0065-codex-chatgpt-responses-backend.md)). It is a
builtin profile of the [openai backend](openai-compatible.md), like `claude`
and `grok` ([ADR 0019](../adr/0019-subscription-logins-claude-grok.md)):
tny owns tools, permissions, MCP, skills, sessions, steer, `--ssh`, and
extensions there — no `codex app-server` process, no WebSocket, no host
registry, and no Codex CLI at all: tny signs in itself.

Canonical sources: the OpenAI Responses API reference
(`developers.openai.com/api/reference/resources/responses`), the Codex
CLI's own client and login (`codex-rs/core/src/client.rs`,
`codex-rs/login` in [openai/codex](https://github.com/openai/codex)), and
pi's `openai-codex.ts` — see [sources.md](../sources.md).

## Credentials

Four sources, first hit wins ([ADR 0066](../adr/0066-native-chatgpt-login-and-credential-sources.md),
`src/core/codex_auth.c`):

| # | Source | Refreshed? | Notes |
| --- | --- | --- | --- |
| 1 | `--chatgpt-token TOKEN` (+ `--chatgpt-account-id ID`) | no | file-less; argv is visible to other local users, prefer 2 |
| 2 | `CHATGPT_ACCESS_TOKEN` (+ `CHATGPT_ACCOUNT_ID`) | no | file-less: containers, CI, the browser wasm build, one-off runs |
| 3 | `~/.tny/codex-auth.json` | yes, in place | written by `tny --provider codex login` (below); tny's own |
| 4 | `$CODEX_HOME/auth.json` (default `~/.codex/auth.json`) | yes, in place | written by the Codex CLI's `codex login`; API-key mode honored |

Any of them present auto-detects the provider ([cli.md](../cli.md#provider-selection)).
`tny providers` / `tny doctor` name the source in use; tokens never print.

- **bearer** — the access token, sent as `Authorization: Bearer`.
- **account** — the explicit id when given (flag/env/`tokens.account_id`),
  else the `"https://api.openai.com/auth".chatgpt_account_id` claim of the
  access token (then the id token). It rides `chatgpt-account-id`; an
  opaque token with no derivable id still runs without the header.
- **API-key mode** — a Codex CLI file whose `OPENAI_API_KEY` is set
  (`codex login --with-api-key`) selects the public API instead:
  `https://api.openai.com/v1`, plain bearer, no ChatGPT headers.

Both files share the Codex CLI's shape (tny adds `expires_at`):

```json
{
  "auth_mode": "chatgpt",
  "OPENAI_API_KEY": null,
  "tokens": {
    "id_token": "<jwt>",
    "access_token": "<jwt>",
    "refresh_token": "…",
    "account_id": "…"
  },
  "last_refresh": "2026-09-01T12:00:00Z",
  "expires_at": "2026-09-01T13:00:00Z"
}
```

### Refresh

At provider resolve tny refreshes the file that wins (its own store first,
else the Codex CLI's) when the access token's `exp` claim — or the store's
`expires_at` — is within 60 s of now, or `last_refresh` is more than eight
days old:

```http
POST https://auth.openai.com/oauth/token
Content-Type: application/json

{ "client_id": "app_EMoamEEZ73f0CkXaXp7hrann",
  "grant_type": "refresh_token", "refresh_token": "…" }
```

The reply's `access_token` / `refresh_token` / `id_token` are written back
**into the same file** (`0600`, atomic) with a fresh `last_refresh` /
`expires_at`, so a Codex CLI session stays shared with the CLI and tny's
store stays self-sufficient; the other file is never touched. Failure
leaves the file untouched and the stale token flows; the provider's 401
then names the fix (`TNY_DEBUG=1` shows the refresh error).
`CODEX_REFRESH_TOKEN_URL_OVERRIDE` (the CLI's own override) or
`TNY_CODEX_OAUTH_ISSUER` point the grant at a test endpoint. Flag and env
tokens carry no refresh token and are used as given.

## Request

The default Responses wire of the openai backend, unchanged, plus two
headers ([openai-compatible.md](openai-compatible.md) documents the body):

```http
POST https://chatgpt.com/backend-api/codex/responses
Authorization: Bearer <access_token>
chatgpt-account-id: <account_id>
OpenAI-Beta: responses=v1
Content-Type: application/json
Accept: text/event-stream
```

```json
{
  "model": "gpt-5.6-sol",
  "instructions": "…tny preamble (+ --system-prompt, ADR 0045)…",
  "input": [ { "role": "user", "content": "…" }, … ],
  "tools": [ { "type": "function", "name": "terminal", "parameters": { } } ],
  "tool_choice": "auto",
  "stream": true,
  "store": false
}
```

The backend requires `stream: true` and `store: false`, and wants
`instructions` — exactly what tny already sends on every Responses request
([ADR 0016](../adr/0016-responses-api-default-wire.md)). `input` is the
Responses **item array** (the API accepts a bare string or an array; tny
always sends the array, since function-call and function-call-output items
have no string form). `--effort` rides `reasoning.effort` with the openai
mapping (`max` clamps to `xhigh`); `--fast` adds
`"service_tier":"priority"`; `--output-schema` flattens onto
`text.format`. Streaming events are the ordinary typed Responses SSE.

Default model: `gpt-5.6-sol` (`--model`, `/model`, or a saved
`models.codex` entry override it; `CODEX_DEFAULT_MODEL` applies only to a
shadowing user profile, see below).

## Selection, shadowing, overrides

| Knob | Effect |
| --- | --- |
| `--provider codex` / `/provider codex` / `last_provider` | select the builtin profile |
| `$CODEX_HOME/auth.json` present | auto-detected first among subscription logins |
| `TNY_CODEX_BASE_URL` | redirect the ChatGPT-mode base URL (mocks, gateways) while keeping the profile's headers |
| `--base-url` | one-run override of any profile's URL (also API-key mode) |
| settings `"codex": {"base_url": …}` or `CODEX_BASE_URL` | a **user profile named codex shadows the builtin** entirely (no ChatGPT headers, `CODEX_API_KEY` key) — explicit config wins, like `claude`/`grok` |
| `--chatgpt-token` / `CHATGPT_ACCESS_TOKEN`, `--chatgpt-account-id` / `CHATGPT_ACCOUNT_ID` | file-less credential (precedence above) |
| `TNY_CODEX_OAUTH_ISSUER`, `TNY_CODEX_CALLBACK_PORT`, `CODEX_REFRESH_TOKEN_URL_OVERRIDE` | login/refresh endpoints for mocks and tests |

## Login and logout

`tny --provider codex login` signs in **natively** — no Codex CLI
(`src/core/codex_login.c`, pinned to the Codex CLI's `codex-rs/login` and
pi's `openai-codex.ts`):

- **Browser (default).** tny generates a PKCE verifier (32 CSPRNG bytes,
  base64url) and `state`, listens on `127.0.0.1:1455` (1457 fallback, the
  CLI's allow-listed ports), prints and opens
  `https://auth.openai.com/oauth/authorize?response_type=code&client_id=app_EMoamEEZ73f0CkXaXp7hrann&redirect_uri=http://localhost:1455/auth/callback&scope=openid profile email offline_access&code_challenge=…&code_challenge_method=S256&state=…&id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=tny`,
  and waits for `/auth/callback?code&state`. A wrong `state` is answered
  400 and ignored; any other path is 404. If the browser is on another
  machine, paste the redirect URL (or the bare code) into the terminal —
  the state is checked the same way. The code is exchanged at
  `POST /oauth/token` (form: `grant_type=authorization_code`, `code`,
  `redirect_uri`, `client_id`, `code_verifier`).
- **Device code (`--device`).** `POST /api/accounts/deviceauth/usercode`
  `{client_id}` → tny prints `https://auth.openai.com/codex/device` and the
  one-time code, then polls `POST /api/accounts/deviceauth/token`
  `{device_auth_id, user_code}` (403/404 or
  `deviceauth_authorization_pending` = keep waiting, `slow_down` widens
  the interval, 15-minute ceiling) until it returns
  `{authorization_code, code_verifier}`, exchanged with
  `redirect_uri=https://auth.openai.com/deviceauth/callback`.

Success writes `~/.tny/codex-auth.json` (`0600`), the source the profile
prefers over the Codex CLI's file; the account id is stored from the JWT
claim. Ctrl-C aborts (exit 130); the browser flow times out after ten
minutes. `TNY_CODEX_OAUTH_ISSUER` and `TNY_CODEX_CALLBACK_PORT` redirect
the whole flow at a mock (tests never open a browser).

`tny --provider codex logout` deletes `~/.tny/codex-auth.json` and says so
when the Codex CLI's file (`codex logout` owns that one) or
`CHATGPT_ACCESS_TOKEN` would still supply a credential.

## Behavior on the native loop

Everything the openai backend documents applies verbatim:
[permissions](../features/permissions.md) are real (default `yolo`,
`ask`/`auto` opt-in), [MCP and skills](../features/mcp-and-skills.md) run
in-process, sessions are tny transcripts (resume, compact, recover), Enter
during a turn steers the native loop, `--ssh` moves tools to a remote host,
and extensions get the full native capability matrix.

## wasm ([ADR 0017](../adr/0017-wasm-browser-parity.md))

**Works**: turns are plain HTTPS over `fetch()`. Credentials: the flag/env
sources need no filesystem (the browser build's path); the two files come
from the wasm build's view of `$HOME` under node. `login --device` is plain
HTTPS too; the browser login needs a listening socket and is native-only.
The wasm CI job runs `tests/integration/test_codex_chatgpt.py` against the
same mocks as the native binary, minus the callback run.

## Tests

- `tests/test_util.c`: SHA-256 FIPS vectors and the RFC 7636 appendix B
  PKCE challenge, base64url, form encoding, CSPRNG.
- `tests/test_core.c` `builtin_codex_profile` / `codex_credential_precedence`:
  every source and its precedence, claim vs explicit account id, header
  set, API-key mode, `TNY_CODEX_BASE_URL`, shadowing, model default, store
  round-trip (`0600`, `expires_at`), logout.
- `tests/integration/test_codex_chatgpt.py`: the full loop against the
  strict Responses mock with header assertions for each source, both
  refresh paths and their in-place rewrites, the device flow (pending
  polls, server-issued verifier), the browser flow (authorize URL
  parameters, bad state → 400 and keep waiting, 404 elsewhere, PKCE
  verifier matches the challenge, pasted URL on a pty), logout, and the
  no-credential error — against an in-test mock issuer.
- Live: `.claude/skills/tny-live-testing` against a real `codex login`.
