# OpenAI-compatible providers

This backend is the **native harness**. tny owns the tool loop, permissions, MCP, skills, sessions, and `tny acp`.

## HTTP surface (v1)

Two wires, one loop ([ADR 0016](../adr/0016-responses-api-default-wire.md)):

```text
POST {base_url}/responses            # default: the Responses API
POST {base_url}/chat/completions     # wire_api "chat": legacy providers
Authorization: Bearer <key>          # default
# or --auth-header-name / --auth-header-prefix for Azure / odd providers
```

The **Responses API is the default** — newer OpenAI models 400 function
tools + `reasoning_effort` on the chat wire, and responses-first gateways
exist. **Chat Completions stays implemented** for what older ollama,
llama.cpp, and small routers actually share; select it per provider with
`wire_api: "chat"` in the profile, `OPENAI_WIRE_API` / `NAME_WIRE_API` in
the environment (env beats settings, like `base_url`), or `--wire-api
chat` for one run.

`base_url` examples: `https://api.openai.com/v1`, `https://openrouter.ai/api/v1`, `http://127.0.0.1:11434/v1`.

Sessions persist **chat-shaped messages** on either wire (the portable
format; old sessions resume unchanged). The responses wire translates at
request time (`src/backends/openai/responses.c`).

Responses request (minimum):

```json
{
  "model": "gpt-4.1-mini",
  "instructions": "…system preamble…",
  "input": [
    { "role": "user", "content": "…" },
    { "type": "function_call", "call_id": "call_1", "name": "read_file", "arguments": "{…}" },
    { "type": "function_call_output", "call_id": "call_1", "output": "…" }
  ],
  "tools": [ { "type": "function", "name": "read_file", "parameters": { } } ],
  "tool_choice": "auto",
  "stream": true,
  "store": false
}
```

`store:false` always — tny owns session state, and the full input rides
every POST (no `previous_response_id` chaining), so compaction, resume,
and steer work identically on both wires. Image messages become
`input_text` / `input_image` parts; the compaction summary is a leading
system input item.

Chat request (minimum, `wire_api: "chat"`):

```json
{
  "model": "gpt-4.1-mini",
  "messages": [
    { "role": "system", "content": "…" },
    { "role": "user", "content": "…" }
  ],
  "tools": [ { "type": "function", "function": { "name": "read_file", "parameters": { } } } ],
  "tool_choice": "auto",
  "stream": true
}
```

`--fast` (`TNY_CAP_FAST`) adds `"service_tier":"priority"` on both wires —
OpenAI's paid fast tier ("priority processing" pre-rename; the API also
accepts `"fast"`, tny sends `"priority"` because older models and
compatible routers accept it too). Omitted unless requested: strict
providers reject unknown members.

Stream: `text/event-stream`, chunked, split anywhere.

- **Responses**: typed events; tny dispatches on the payload's `"type"`
  (the `event:` line is redundant). `response.output_text.delta` → text,
  `response.reasoning_summary_text.delta` / `response.reasoning_text.delta`
  → thinking, `response.output_item.added` + `response.function_call_arguments.delta`
  assemble tool calls (`response.output_item.done` carries the complete
  `arguments` string and is authoritative), `response.completed` carries
  `usage.input_tokens/output_tokens` and ends the stream.
  `response.failed` / `error` → run error with the provider message;
  `response.incomplete` keeps the partial text and ends with a non-success
  limit/filter stop (as do chat's `length` / `content_filter` reasons).
- **Chat**: lines `data: {…}` then `data: [DONE]`. Accumulate
  `choices[0].delta.content` and `choices[0].delta.tool_calls` (index, id,
  function.name, function.arguments fragments). Non-stream fallback: read
  `choices[0].message`.

OpenRouter-compatible chat streams may carry textual `reasoning_details`
items; tny normalizes their `text`/`summary` fields to `THINKING`. A top-level
SSE `error` is terminal even when the HTTP status remains 200, and `[DONE]`
is only framing—not evidence that the run succeeded.

Tool-call assembly (`src/backends/openai/toolcalls.c`, unit-tested in
`tests/test_openai.c`) is **id-first**, not index-first: a fragment with an
unseen `id` always opens a new call, a fragment with a known `id` merges into
it, id-less fragments key by `index` (most recent call for that index), and
fragments with neither go to the last open call. Rationale: gateways have
been observed streaming parallel calls with a repeated or missing `index`
but a fresh `id` per call; index-keyed assembly merged two calls and dropped
an id, so the next request was missing a tool output and the provider
rejected the session with 400 "no tool output found for function call …".

Transcript pairing is an invariant: every id in a recorded assistant
`tool_calls` message gets exactly one `role:"tool"` message, ids fall back
to slot-unique `call_<n>` (never a shared constant), and an interrupt
mid-step records `error: interrupted` results for the calls that did not
run instead of leaving them unpaired.

Structured outputs: when `ctx->output_schema` is set (`tny ask
--output-schema`, docs/cli.md), `tny_openai_response_format()` normalizes
bare schemas, `json_schema` objects, and full wrappers into the chat
`response_format` shape — `{"type":"json_schema","json_schema":{"name":…,"strict":…,"schema":…}}` —
which is what ctx stores. The chat wire sends it verbatim; the responses
wire flattens it onto `"text":{"format":{"type":"json_schema","name":…,"schema":…}}`
(`tny_openai_responses_text_format()`). Tool calls are unaffected; the
schema constrains the final assistant text.

Also implement `GET {base_url}/models` for `/models` when the provider has it; otherwise show configured ids only.

## Builtin subscription profiles: claude and grok

Two profiles ship with tny ([ADR 0019](../adr/0019-subscription-logins-claude-grok.md)).
They behave exactly like user-named profiles (openai backend, own name /
saved model / key resolution) but need no settings entry, and they resolve
subscription credentials other CLIs minted. A settings object or
`NAME_BASE_URL` env var named `claude` / `grok` shadows the builtin.

**claude** — Anthropic's OpenAI-compat endpoint
(`https://api.anthropic.com/v1`, chat wire, default model
`claude-sonnet-4-6`). Credential order:

1. `CLAUDE_CODE_OAUTH_TOKEN` (from `claude setup-token`; `tny --provider
   claude login` runs it when nothing resolves),
2. `ANTHROPIC_API_KEY` (Console key),
3. `~/.claude/.credentials.json` → `claudeAiOauth.accessToken`
   (`$CLAUDE_CONFIG_DIR` honored; written by `claude /login` on
   Linux/Windows — macOS keeps it in the Keychain, use the env var there).

OAuth-sourced tokens (or the `sk-ant-oat` prefix) ride
`Authorization: Bearer` **plus** `anthropic-beta: oauth-2025-04-20`; a
Console key must not carry the beta header. These extra headers live in
`ctx->extra_headers` and are appended to every request.

**grok** — two credential modes:

1. session token from `~/.grok/auth.json` — the legacy
   `"https://accounts.x.ai/sign-in".key` entry, or an OIDC
   `"{issuer}::{client_id}"` entry written by tny's **native device-code
   login** (`tny --provider grok login`,
   [ADR 0019](../adr/0019-native-grok-device-login.md)) or by the grok CLI:
   base URL `https://cli-chat-proxy.grok.com/v1`, chat wire (proxy models
   are streaming-only chat), headers `X-XAI-Token-Auth: xai-grok-cli`,
   `x-grok-model-override: <model>` (the proxy routes on the header, not
   the body; default model `grok-4.6`), and
   `x-grok-client-version` (the proxy version-gates and answers HTTP 426
   below its rolling minimum; tny pins a known-accepted grok-build
   version, `TNY_GROK_CLIENT_VERSION` overrides it without a rebuild).
   OIDC entries carry `refresh_token` / `expires_at`; at provider resolve
   tny runs the `refresh_token` grant when the entry is at/near expiry and
   writes the rotated tokens back into the store, so a device-code login
   outlives its first access token without the grok CLI's background
   refresher. The model-override header is pinned at provider-resolve
   time, so a mid-session `/model` on the proxy applies at the next
   resolve.
2. else `XAI_API_KEY` against `https://api.x.ai/v1` on the default
   Responses wire, default model `grok-4.6`.

Credentials are read when the provider resolves; the only write is the
grok token refresh above, back into the same `~/.grok/auth.json` entry the
token came from. tny stores env-var names otherwise.

## Provider quirks (handle with flags, not forks)

| Quirk | Flag / rewrite |
| --- | --- |
| Azure `api-key` header | `auth_header_name=api-key`, empty prefix |
| Chat-only provider | `wire_api: "chat"` / `NAME_WIRE_API` / `--wire-api chat` ([ADR 0016](../adr/0016-responses-api-default-wire.md)) |
| Old `max_tokens` vs `max_completion_tokens` | `max_tokens_field` (chat wire; when set, the responses wire sends `max_output_tokens`) |
| Reasoning effort | `--effort` / `/effort` → `reasoning.effort` (responses) / `reasoning_effort` (chat); canonical levels map per [ADR 0009](../adr/0009-reasoning-effort.md) (`off`→`none`, `light`→`low`, `max`→`xhigh`); provider-specific tokens (`minimal`, …) pass through verbatim. Omitted when unset — providers without the field would 400 |
| Extra `thinking`-style objects | pass-through JSON object in provider profile (later) |
| Missing tools | disable tools, error clearly |
| Parallel tool calls | honor `parallel_tool_calls` when present |

Config entry — the builtin `"openai"` object, plus **any number of named
profiles**: every top-level settings object with a `base_url` is a provider
name that `--provider` / `/provider` accepts:

```json
{
  "openai": {
    "base_url": "https://api.openai.com/v1",
    "model": "gpt-4.1-mini"
  },
  "openrouter": {
    "base_url": "https://openrouter.ai/api/v1",
    "api_key_env": "OPENROUTER_API_KEY",
    "model": "anthropic/claude-sonnet-4.6",
    "auth_header_name": "Authorization",
    "auth_header_prefix": "Bearer "
  },
  "xai": {
    "base_url": "https://api.x.ai/v1"
  }
}
```

Named providers can equally be defined **by environment variables alone** —
no settings entry needed:

```sh
OPENROUTER_BASE_URL=https://openrouter.ai/api/v1 \
OPENROUTER_API_KEY=sk-or-… \
OPENROUTER_DEFAULT_MODEL=anthropic/claude-sonnet-4.6 \
tny ask "hi"                       # auto-detected, provider name "openrouter"
```

Named-provider rules:

- A name is valid when settings has a `base_url` object for it **or**
  `NAME_BASE_URL` is set (name uppercased, non-alphanumerics mapped to `_`).
  When both exist, the env `base_url` wins — mirroring how
  `OPENAI_BASE_URL` beats the `"openai"` object. The settings `base_url` is
  what marks the object as a provider profile (reserved objects like
  `workspaces` and `models` never have one). The four builtin names
  (`openai|cursor|codex|acp`) are never named providers.
- The API key comes from the profile's `api_key_env`, defaulting to
  `NAME_API_KEY` (e.g. `xai` → `XAI_API_KEY`). `OPENAI_API_KEY` is **not** a
  fallback: it belongs to a different provider.
- Model precedence: `--model` > saved `models.{name}` > the profile's
  `model` > `NAME_DEFAULT_MODEL` from the environment.
- `auth_header_name`, `auth_header_prefix`, `max_tokens_field`, and
  `wire_api` work exactly as in the `"openai"` object (settings profile;
  `wire_api` can also come from `NAME_WIRE_API` in the environment).
  `last_provider` remembers the provider across launches — env-defined ones
  included.
- Auto-detection without `--provider`/`last_provider` picks an env-defined
  provider only when **exactly one** `NAME_BASE_URL` + `NAME_API_KEY` pair
  is set, so a stray `*_BASE_URL` exported by an unrelated tool never
  hijacks the default. Keyless local gateways need an explicit
  `--provider NAME` once.
- Env detection is a lazy in-memory walk of the process environment at
  provider-resolution time (microseconds, no I/O) — startup paths never run
  it, so `--help` / `--version` / first TUI paint stay fast.
- `--base-url` / `--api-key-env` flags override the selected provider for
  one run.

## Agent loop

1. Assemble messages: tny preamble + `AGENTS.md` chain + skill catalog (names only) + bounded history.
2. POST with built-in + selected MCP tool schemas.
3. On tool calls: permission check → execute (parallel read-only, serial writes) → append `role: tool` messages. `read_image` then injects a **user** message with `image_url` data-URL parts (providers reject image parts on `role: tool`; [ADR 0008](../adr/0008-native-loop-images.md)) → POST again.
4. Stop on final text, cancel, permission deny, or the optional step limit (unlimited by default; `--max-steps` / `/max-steps` / `.tny.json` `"steps"` set a cap — [ADR 0024](../adr/0024-unlimited-steps-default.md)).

This is the only backend that uses [features/permissions.md](../features/permissions.md) and [features/mcp-and-skills.md](../features/mcp-and-skills.md) as the execution engine. Host backends have their own loops; tny only maps events.

## SSE client notes

- Honor `Last-Event-ID` only if we reconnect the same request (rare).
- Read timeouts must be long (minutes) while still detecting a dead TCP.
- Treat a 401/403 as startup-class errors (exit 1). Mid-stream 5xx as run errors (exit 2) after retry if `Retry-After` says so.
