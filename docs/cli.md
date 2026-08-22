# CLI

Design the CLI so humans and coding agents can run it without menus. Every input is a flag or stdin. Interactive prompts are a fallback, never the only path. Each subcommand has `--help` with copy-paste examples.

Binary name: `tny`.

## Command tree

```text
tny                         # interactive TUI, fresh session
tny ask [prompt]            # one turn, then exit
tny resume [last|<id>]      # interactive resume
tny acp                     # ACP server (native loop only)
tny sessions
tny session last|<id>
tny providers               # list configured providers and doctor hints
tny models
tny permissions
tny workspace list|add|remove|clear
tny status
tny doctor
tny login                   # provider-specific; see --provider
tny logout
tny setup                   # write provider config from flags/env
```

Global flags are **leading**:

```text
tny --provider cursor|codex|acp|openai|NAME [command]   # --backend is an alias;
                            # NAME = a settings.json provider profile (below)
tny --cwd DIR
tny --model ID
tny --effort LEVEL          # reasoning effort (--reasoning-effort is an alias)
tny --add-dir DIR           # repeatable, process-only
tny --permission-mode ask|auto|yolo   # default: yolo (docs/adr/0001)
tny --fast                  # paid fast tier (TNY_CAP_FAST providers only)
tny --json                  # where listed
tny -r                      # session picker (TUI)
tny -c                      # resume last for this workspace
```

## Provider selection

`--provider` accepts the four builtin names plus any **named OpenAI-compatible
provider** (`"openrouter"`, `"xai"`, a local gateway — any name), defined
either way or both:

- a top-level `~/.tny/settings.json` object with a `base_url`, and/or
- `NAME_BASE_URL` in the environment (name uppercased, non-alphanumerics →
  `_`; the env value beats the settings `base_url`)

Named providers run on the openai backend but keep their own name, config,
key env, and saved model (see
[backends/openai-compatible.md](backends/openai-compatible.md)). Env
detection is a lazy in-memory scan at provider-resolution time — startup
paths (`--help`, `--version`, first TUI paint) never run it.

`--provider` default, in order:

1. the provider (and its saved model) last used, recorded in `~/.tny/settings.json` (`last_provider`, `models.{provider}`) — named providers included
2. `openai` if `OPENAI_BASE_URL` or `OPENAI_API_KEY` is set
3. the env-defined provider if **exactly one** `NAME_BASE_URL` + `NAME_API_KEY` pair is set (a lone `*_BASE_URL` from an unrelated tool never hijacks the default; keyless local gateways need an explicit `--provider NAME` once — `last_provider` remembers it)
4. `codex` if a `codex login` exists (`$CODEX_HOME/auth.json`, default `~/.codex/auth.json`) — subscriptions need no API key
5. `cursor` if `CURSOR_API_KEY` is set in the environment
6. `openai` (its connect error explains how to configure a key)

## Reasoning effort

`--effort` (env `TNY_REASONING_EFFORT`, TUI `/effort`) takes the canonical
levels `off | light | medium | high | xhigh | max` and maps them onto each
provider's wire vocabulary ([ADR 0009](adr/0009-reasoning-effort.md)):
codex `turn/start.effort`, cursor `ModelSelection.params`, openai
`reasoning.effort` (`reasoning_effort` on the chat wire). ACP has no
portable knob at protocolVersion 1; the
backend says so in one status line and the agent's default applies.

Providers advertise their real per-model levels through their catalogs;
`tny models` shows them (`[effort: …]` / `"efforts"` in `--json`) and any
advertised token is accepted verbatim (e.g. `--effort minimal` on openai).
Unset means the provider default; `--effort default` clears an inherited
env or settings value.

A default lives in `~/.tny/settings.json` under `"effort"` — one string for
every provider, or a per-provider object like `"models"`
([ADR 0015](adr/0015-settings-default-effort.md)):

```json
{ "effort": "high" }
{ "effort": { "codex": "xhigh", "openai": "medium" } }
```

Precedence: `--effort` / `/effort` (an explicit `default` included) beats
`TNY_REASONING_EFFORT` beats the settings entry beats the provider default.
tny never *writes* the effort back to settings — a scripted
`tny ask --effort X` does not change what tomorrow's session does.

## `tny ask` (scripts and CI)

```text
tny ask "summarize this repository"
printf 'summarize src/\n' | tny ask --stdin
tny ask --json --no-save "list the public CLI"
tny ask --resume last "now add tests"
tny ask --provider cursor --model composer-2 "find the login bug"
tny --provider codex --effort xhigh ask "prove this queue is lock-free"
tny ask --yolo --cwd /tmp/ws "run the test suite"
```

Stdout: assistant Markdown (or one JSON object with `--json`).
Stderr: progress, tool lines, diagnostics.
Exit 0 finished, 1 startup/config, 2 run failed, 130 interrupted.

JSON object (keep field names stable):

```json
{
  "output": "…",
  "exit_code": 0,
  "provider": "openai",
  "model": "provider/model",
  "session_id": "…",
  "steps": 1,
  "tool_calls": [{"name": "read_file", "status": "success"}]
}
```

`--json` is required on `ask`, `status`, `doctor`, `permissions`, `models`, `session`, `sessions`, `workspace`, `usage`.

## Provider-specific flags

| Provider | Flags / env |
| --- | --- |
| cursor | `--bridge-bin PATH`, `CURSOR_SDK_BRIDGE_BIN`, `CURSOR_API_KEY` (also pass through to RPCs) |
| codex | `--codex-ws URL` to attach (attach-or-fail); without it tny first tries `TNY_CODEX_WS`, then a live registered host from `~/.tny/codex-host.json` (loopback only, written by whichever tny spawned the server — a running TUI, typically), and only then spawns `codex app-server` on an ephemeral port (never a fixed port that could collide). Discovery failures fall back to spawning silently (`docs/adr/0004`). `--codex-bin`, `--ws-token-file`, `CODEX_REMOTE_TOKEN` |
| acp | `--agent CMD` plus extra args after `--`, e.g. `tny --provider acp --agent gemini -- acp` |
| openai | `--base-url`, `--api-key-env NAME`, `--wire-api responses\|chat` (default `responses`; `chat` for legacy-only providers, [ADR 0016](adr/0016-responses-api-default-wire.md)), `OPENAI_BASE_URL`, `OPENAI_API_KEY`, `OPENAI_WIRE_API` |
| named provider | same flags; `NAME_BASE_URL` (beats the settings `base_url`), key from the profile's `api_key_env`, default `NAME_API_KEY` — never `OPENAI_API_KEY`; `NAME_WIRE_API` / profile `wire_api` |

Model precedence for every provider: `--model` > saved `models.{provider}` >
the provider object's `model` (openai-compatible only) > `NAME_DEFAULT_MODEL`
from the environment (`CODEX_DEFAULT_MODEL`, `OPENROUTER_DEFAULT_MODEL`, …).

`tny ask` never blocks on an approval. Unresolved permissions fail the run unless `--auto` reviews (native loop) or `--yolo`. Host providers must be pre-authorized or they fail closed.

`--image PATH` (repeatable) attaches image files to the first user message as
`image_url` data URLs on the native OpenAI-compatible loop. The same encoding
is used when the model calls `read_image` mid-turn. Max 8 MiB; type comes
from magic bytes (png/jpeg/gif/webp), not the extension.

## Structured output (`--output-schema`)

`tny ask --output-schema VALUE` constrains the final answer to a JSON Schema
via Chat Completions `response_format` (openai-compatible provider only —
other providers fail at startup with exit 1). VALUE is a file path, or inline
JSON when it starts with `{`. Three shapes are accepted and normalized:

- a bare JSON Schema — wrapped as `{"type":"json_schema","json_schema":{"name":"output","strict":true,"schema":…}}`
- a `json_schema` object (`{"name":…,"schema":…}`) — wrapped, `name` defaults to `output`
- a full `response_format` (`{"type":"json_schema",…}`) — sent as-is

```text
tny ask --output-schema schema.json "extract the TODOs as JSON"
tny ask --output-schema '{"type":"object","properties":{"count":{"type":"integer"}},"required":["count"],"additionalProperties":false}' "how many files?"
```

Stdout is the model's JSON text (inside `output` with `--json`). The tool
loop still runs; the schema constrains the final assistant message.
## `--fast` (speed tier)

`--fast` opts in to the provider's paid fast tier ([ADR 0010](adr/0010-fast-tier-capability.md); `TNY_CAP_FAST` in
`src/core/backend.h`). OpenAI renamed "priority processing" to "fast mode";
the API accepts both spellings. Each capable provider maps the flag to its
own wire field:

| Provider | Wire mapping |
| --- | --- |
| openai | `"service_tier":"priority"` on the chat-completions request (`fast` alias server-side) |
| codex | `thread/start` `serviceTier:"priority"` (same tier; the value every app-server release accepts) |
| cursor | `ModelSelection` param `{"id":"fast","value":"true"}` — fast is a per-model variant, not a request field |
| acp | not supported — `--fast` exits 1 with the capable provider list |

The interactive TUI exposes the same capability as `/fast [fast|priority|default]`.

Provider caveats: `--provider cursor` runs Cursor's own headless loop — the bridge exposes no per-call approval RPC, so tny's permission mode does not apply (a status line says so); it also rejects `--image`. `--provider codex` ignores `--image` with a status line (no documented image input item).

## Help shape

```text
Usage: tny ask [options] [prompt]

Options:
  --json          Write one JSON object to stdout
  --resume last   Continue the latest workspace session
  --no-save       Do not persist a session
  --provider NAME cursor | codex | acp | openai | settings profile (--backend also accepted)

Examples:
  tny ask "explain src/main.c"
  tny ask --json --provider openai "list exported symbols"
  tny --provider cursor ask --model composer-2 "fix the leak"
```

Missing required values print the error, then a correct example, then exit 1. No timed prompts.
