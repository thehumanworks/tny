# OpenAI-compatible providers

This backend is the **native harness**. tny owns the tool loop, permissions, MCP, skills, sessions, and `tny acp`.

## HTTP surface (v1)

Implement Chat Completions first. It is what Groq, OpenRouter, Together, DeepSeek, ollama, llama.cpp, vLLM, Azure OpenAI (with tweaks), and local gateways actually share.

```text
POST {base_url}/chat/completions
Authorization: Bearer <key>          # default
# or --auth-header-name / --auth-header-prefix for Azure / odd providers
```

`base_url` examples: `https://api.openai.com/v1`, `https://openrouter.ai/api/v1`, `http://127.0.0.1:11434/v1`.

Request (minimum):

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

`--fast` (`TNY_CAP_FAST`) adds `"service_tier":"priority"` — OpenAI's paid
fast tier ("priority processing" pre-rename; the API also accepts `"fast"`,
tny sends `"priority"` because older models and compatible routers accept
it too). Omitted unless requested: strict providers reject unknown members.

Stream: `text/event-stream`. Lines `data: {…}` then `data: [DONE]`. Accumulate `choices[0].delta.content` and `choices[0].delta.tool_calls` (index, id, function.name, function.arguments fragments).

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

Non-stream: read `choices[0].message`.

Structured outputs: when `ctx->output_schema` is set (`tny ask
--output-schema`, docs/cli.md), every POST carries a `response_format`
object — `{"type":"json_schema","json_schema":{"name":…,"strict":…,"schema":…}}`.
`tny_openai_response_format()` normalizes bare schemas, `json_schema`
objects, and full wrappers into that shape. Tool calls are unaffected; the
schema constrains the final assistant text.

Also implement `GET {base_url}/models` for `/models` when the provider has it; otherwise show configured ids only.

## Provider quirks (handle with flags, not forks)

| Quirk | Flag / rewrite |
| --- | --- |
| Azure `api-key` header | `auth_header_name=api-key`, empty prefix |
| Old `max_tokens` vs `max_completion_tokens` | `max_tokens_field` |
| Reasoning effort | `--effort` / `/effort` → `reasoning_effort` in the request; canonical levels map per [ADR 0009](../adr/0009-reasoning-effort.md) (`off`→`none`, `light`→`low`, `max`→`xhigh`); provider-specific tokens (`minimal`, …) pass through verbatim. Omitted when unset — providers without the field would 400 |
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
- `auth_header_name`, `auth_header_prefix`, and `max_tokens_field` work
  exactly as in the `"openai"` object (settings profile only).
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
4. Stop on final text, step limit, cancel, or permission deny.

This is the only backend that uses [features/permissions.md](../features/permissions.md) and [features/mcp-and-skills.md](../features/mcp-and-skills.md) as the execution engine. Host backends have their own loops; tny only maps events.

## Responses API (optional, later)

`POST /v1/responses` with typed SSE (`response.output_text.delta`, `response.function_call_arguments.delta`). Useful for OpenAI-native and Codex-shaped gateways. Do not block v1 on it. Chat Completions remains the compatibility layer.

## SSE client notes

- Honor `Last-Event-ID` only if we reconnect the same request (rare).
- Read timeouts must be long (minutes) while still detecting a dead TCP.
- Treat a 401/403 as startup-class errors (exit 1). Mid-stream 5xx as run errors (exit 2) after retry if `Retry-After` says so.
