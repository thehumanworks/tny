# Codex (ChatGPT Responses backend)

The `codex` provider drives the user's **ChatGPT subscription** through the
Responses-compatible backend the Codex CLI itself talks to
(`https://chatgpt.com/backend-api/codex`), on tny's **native loop**
([ADR 0065](../adr/0065-codex-chatgpt-responses-backend.md)). It is a
builtin profile of the [openai backend](openai-compatible.md), like `claude`
and `grok` ([ADR 0019](../adr/0019-subscription-logins-claude-grok.md)):
tny owns tools, permissions, MCP, skills, sessions, steer, `--ssh`, and
extensions there — no `codex app-server` process, no WebSocket, no host
registry. The Codex CLI is only the **login helper**.

Canonical sources: the OpenAI Responses API reference
(`developers.openai.com/api/reference/resources/responses`) and the Codex
CLI's own client (`codex-rs/login`, `codex-rs/core/src/client.rs` in
[openai/codex](https://github.com/openai/codex)) — see
[sources.md](../sources.md).

## Credentials

`codex login` (or `tny --provider codex login`) writes
`$CODEX_HOME/auth.json` (default `~/.codex/auth.json`):

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
  "last_refresh": "2026-09-01T12:00:00Z"
}
```

tny reads it at provider-resolve time (`src/core/codex_auth.c`):

- **bearer** — `tokens.access_token`, sent as `Authorization: Bearer`.
- **account** — `tokens.account_id`, else the
  `"https://api.openai.com/auth".chatgpt_account_id` claim of the access
  token (then the id token). It rides the `chatgpt-account-id` header.
- **API-key mode** — an auth.json whose `OPENAI_API_KEY` is set
  (`codex login --with-api-key`) selects the public API instead:
  `https://api.openai.com/v1`, plain bearer, no ChatGPT headers.

The presence of the file is what auto-detects the provider
([cli.md](../cli.md#provider-selection)). tny never prints tokens.

### Refresh

The Codex CLI refreshes its tokens in the background; tny runs the same
grant itself before reading, when the access token's `exp` claim is within
60 s of now or `last_refresh` is more than eight days old:

```http
POST https://auth.openai.com/oauth/token
Content-Type: application/json

{ "client_id": "app_EMoamEEZ73f0CkXaXp7hrann",
  "grant_type": "refresh_token", "refresh_token": "…" }
```

The reply's `access_token` / `refresh_token` / `id_token` are written back
into the same `auth.json` (`0600`, atomic) with a fresh `last_refresh`, so
tny and the Codex CLI keep sharing one session. Failure leaves the file
untouched and the stale token flows; the provider's 401 then names the
fix. `CODEX_REFRESH_TOKEN_URL_OVERRIDE` (the CLI's own override) points the
grant at a test endpoint.

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
| `--codex-bin PATH` / `TNY_CODEX_BIN` | the CLI `tny --provider codex login` runs |

## Login and logout

`tny --provider codex login [--device]` runs `codex login` (`codex login
--device-auth` with `--device`): the Codex CLI owns the OAuth ceremony —
the localhost PKCE callback, or the device-code flow for headless boxes —
and writes `auth.json`; tny then reports whether the file appeared. Without
the CLI on PATH the command prints how to install it. `tny --provider
codex logout` deletes `auth.json` (what `codex logout` does) without
needing the CLI.

`tny providers` and `tny doctor` show the credential state (`ChatGPT
login`, `API key from auth.json`, or `no login`) and whether the Codex CLI
is on PATH.

## Behavior on the native loop

Everything the openai backend documents applies verbatim:
[permissions](../features/permissions.md) are real (default `yolo`,
`ask`/`auto` opt-in), [MCP and skills](../features/mcp-and-skills.md) run
in-process, sessions are tny transcripts (resume, compact, recover), Enter
during a turn steers the native loop, `--ssh` moves tools to a remote host,
and extensions get the full native capability matrix.

## wasm ([ADR 0017](../adr/0017-wasm-browser-parity.md))

**Works**: the profile is plain HTTPS over `fetch()`. `auth.json` comes from
the wasm build's filesystem view of `$HOME`; the browser build has no
`~/.codex` and needs `TNY_CODEX_BASE_URL`-style gateways or the API-key
mode. The wasm CI job runs `tests/integration/test_codex_chatgpt.py` against
the same mock as the native binary.

## Tests

- `tests/test_core.c` `builtin_codex_profile`: auth.json parsing, claim vs
  field account id, header set, API-key mode, `TNY_CODEX_BASE_URL`,
  shadowing, model default.
- `tests/integration/test_codex_chatgpt.py`: the full loop against the
  strict Responses mock with header assertions, auto-detection, the
  refresh grant + `auth.json` rewrite, API-key mode, the no-login error,
  and native logout.
- Live: `.claude/skills/tny-live-testing` against a real `codex login`.
