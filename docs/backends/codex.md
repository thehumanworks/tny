# Codex (ChatGPT Responses backend)

The `codex` provider drives the user's **ChatGPT subscription** through the
Responses-compatible backend the Codex CLI itself talks to
(`https://chatgpt.com/backend-api/codex`), on tny's **native loop**
([ADR 0065](../adr/0065-codex-chatgpt-responses-backend.md)). It is a
builtin profile of the [openai backend](openai-compatible.md), alongside `grok` ([ADR 0019](../adr/0019-subscription-logins-claude-grok.md)):
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
| 4 | `$CODEX_HOME/auth.json` (default `~/.codex/auth.json`) | yes, in place | written by the Codex CLI's `codex login`; OAuth tokens only |

Any of them present auto-detects the provider ([cli.md](../cli.md#provider-selection)).
`tny providers` / `tny doctor` name the source in use; tokens never print.

- **bearer** — the access token, sent as `Authorization: Bearer`.
- **account** — the explicit id when given (flag/env/`tokens.account_id`),
  else the `"https://api.openai.com/auth".chatgpt_account_id` claim of the
  access token (then the id token). It rides `chatgpt-account-id`; an
  opaque token with no derivable id still runs without the header.
- **Stored API keys are rejected.** Migrate to an env-key HTTP profile (`OPENAI_API_KEY` or `api_key_env`). Subscription OAuth tokens remain supported.

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

## Subscription usage (`/status`, `tny status`)

For the builtin Codex **ChatGPT subscription** profile, status also fetches:

```http
GET https://chatgpt.com/backend-api/wham/usage
Authorization: Bearer <access_token>
chatgpt-account-id: <account_id>
```

This is the ChatGPT usage endpoint from the Codex CLI's release-pinned
backend client and generated OpenAPI models (see [sources.md](../sources.md)).
It is not `/codex/usage` or the public OpenAI billing API. The request uses
the active profile's credentials and origin; trusted `TNY_CODEX_BASE_URL`
gateways replace the trailing `/codex` with `/wham/usage`.

Status finds the 604800-second window in `rate_limit.primary_window` or
`secondary_window`. It displays `100 - used_percent` as **weekly limit:
N% left**, plus the remaining days/hours and `reset_at` in local time,
including weekday, date, hour, and timezone. Short windows are not mislabeled
as weekly. API-key Codex logins and other providers do not make this request
or display subscription usage.

`tny status --json` adds `codex_usage` with `weekly_remaining_percent` and
`reset_at` (Unix seconds). Unavailable, missing, or invalid weekly data produces
`codex_usage: null` and a text “weekly limit: unavailable” message; other
health fields remain available. Error bodies and tokens never print. Reads
are bounded (5 seconds for response headers, 5 seconds for the body, 64 KiB
body maximum), in addition to the shared transport's connection timeout.
The value is fetched each time status runs, not cached from a prior turn.

Wasm uses the same transport seam and endpoint. Remote CLI transport works;
browser use requires backend CORS or a trusted gateway. A blocked request
shows unavailable rather than a fabricated allowance.

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

## Native web search

Builtin ChatGPT Responses requests expose only `run_code`, like other native
profiles. Code calls the shared `web_search` operation; provider-hosted search
is not an alternate execution path. Unsolicited hosted search is rejected and
historical hosted search items are not replayed to the provider.
Explicit `web_search_command` or `web_search_url` settings retain priority.
**The Codex login powers search independently of the conversation provider.**
`tny web search QUERY` uses that same independent service with Codex selected.
It defaults to a separate `gpt-5.6-sol` search model (`web_search_model` overrides),
without changing the conversation model. Only absence of a Codex/ChatGPT login
selects DuckDuckGo; failed or invalid logged-in requests are errors, not fallback.
Explicit command/URL settings remain authoritative. API-key-only Codex auth is
not a subscription login. See [ADR 0106](../adr/0106-native-web-search-and-duckduckgo.md)
and its routing amendment [ADR 0109](../adr/0109-provider-independent-codex-search.md).

## Model catalog (`tny models`, `/models`)

The ChatGPT backend's catalog is not the public `GET /v1/models`:

```text
GET https://chatgpt.com/backend-api/codex/models?client_version=<discovery compatibility version>
Authorization: Bearer <access_token>
chatgpt-account-id: <account_id>
OpenAI-Beta: responses=v1
→ {"models":[{"slug":"gpt-5.6-sol","display_name":"GPT-5.6-Sol","visibility":"list",
              "default_reasoning_level":"low","context_window":272000,
              "supported_reasoning_levels":[{"effort":"low","description":"…"},…],…},…]}
```

`client_version` is **required** (a bare `/models` is HTTP 400) and gates
the listing: the backend only returns models whose `minimal_client_version`
the claimed client meets. The former `0.154.0` pin omitted `gpt-6-sol` and
`gpt-6-luna`, whose minimum is `0.155.0`. tny now claims `999.999.999` for
catalog discovery, so a new minimum version does not require a tny release.
`TNY_CODEX_CLIENT_VERSION` overrides that default if the backend changes its
version handling or a gateway needs a specific value. This value affects
catalog filtering, not the model used for a turn.

In ChatGPT mode every `tny models` or TUI `/models` invocation sends a fresh
request; tny has no CLI/TUI catalog cache to revalidate. It normalizes the
answer into the shared catalog shape
(`[{"id","name","description","efforts":[…],"default_effort","context_window"}]`),
dropping entries whose `visibility` is not `list` (`hide`, `none`). `--json`
reports `{"kind":"models","provider":"codex","models":[…]}`; the plain
listing shows `[effort: …]` per model and the `efforts` tokens
are what `--effort` accepts verbatim. The backend can still restrict models
by account or visibility, and a catalog entry does not establish that a turn
with that model will succeed. If the catalog request fails, tny shows its
configured-model fallback. See [ADR 0170](../adr/0170-codex-catalog-discovery-version.md).

## Selection, shadowing, overrides

| Knob | Effect |
| --- | --- |
| `--provider codex` / `/provider codex` / `last_provider` | select the builtin profile |
| `$CODEX_HOME/auth.json` present | auto-detected first among subscription logins |
| `TNY_CODEX_BASE_URL` | redirect the ChatGPT-mode base URL (mocks, gateways) while keeping the profile's headers |
| `TNY_CODEX_CLIENT_VERSION` | Override the `999.999.999` catalog discovery compatibility value on `/models?client_version=` |
| `--base-url` | one-run override of any profile's URL  |
| settings `"codex": {"base_url": …}` or `CODEX_BASE_URL` | a **user profile named codex shadows the builtin** entirely (no ChatGPT headers, `CODEX_API_KEY` key) — explicit config wins, like `grok` |
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

Streams from `chatgpt.com` are interrupted more often than from
`api.openai.com` — a clean close without `response.completed`, a reset
mid-body, or a socket that goes silent. Each is an interruption, never a
truncated answer: before any text the request is retried, after text the
answer is continued from the shown partial, and a stream silent for
`TNY_PROVIDER_STALL_SECS` (default 300 s, the Codex CLI's own idle timeout)
is given up on ([ADR 0087](../adr/0087-stream-completion-and-continuation.md),
[openai-compatible.md](openai-compatible.md#stream-errors-retries-and-diagnostics-adr-0069)).

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
  set, stored API-key rejection, `TNY_CODEX_BASE_URL`, shadowing, model default, store
  round-trip (`0600`, `expires_at`), logout.
- `tests/integration/test_codex_chatgpt.py`: the full loop against the
  strict Responses mock with header assertions for each source, both
  refresh paths and their in-place rewrites, the device flow (pending
  polls, server-issued verifier), the browser flow (authorize URL
  parameters, bad state → 400 and keep waiting, 404 elsewhere, PKCE
  verifier matches the challenge, pasted URL on a pty), logout, and the
  no-credential error — against an in-test mock issuer.
- Live: `.claude/skills/tny-live-testing` against a real `codex login`.
