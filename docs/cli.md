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
tny --provider cursor|codex|acp|openai [command]   # --backend is an alias
tny --cwd DIR
tny --model ID
tny --add-dir DIR           # repeatable, process-only
tny --permission-mode ask|auto|yolo   # default: yolo (docs/adr/0001)
tny --json                  # where listed
tny -r                      # session picker (TUI)
tny -c                      # resume last for this workspace
```

## Provider selection

`--provider` default, in order:

1. the provider (and its saved model) last used, recorded in `~/.tny/settings.json` (`last_provider`, `models.{provider}`)
2. `openai` if `OPENAI_BASE_URL` or `OPENAI_API_KEY` is set
3. `codex` if a `codex login` exists (`$CODEX_HOME/auth.json`, default `~/.codex/auth.json`) — subscriptions need no API key
4. `cursor` if `CURSOR_API_KEY` is set in the environment
5. `openai` (its connect error explains how to configure a key)

## `tny ask` (scripts and CI)

```text
tny ask "summarize this repository"
printf 'summarize src/\n' | tny ask --stdin
tny ask --json --no-save "list the public CLI"
tny ask --resume last "now add tests"
tny ask --provider cursor --model composer-2 "find the login bug"
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
| openai | `--base-url`, `--api-key-env NAME`, `OPENAI_BASE_URL`, `OPENAI_API_KEY` |

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

Provider caveats: `--provider cursor` runs Cursor's own headless loop — the bridge exposes no per-call approval RPC, so tny's permission mode does not apply (a status line says so); it also rejects `--image`. `--provider codex` ignores `--image` with a status line (no documented image input item).

## Help shape

```text
Usage: tny ask [options] [prompt]

Options:
  --json          Write one JSON object to stdout
  --resume last   Continue the latest workspace session
  --no-save       Do not persist a session
  --provider NAME cursor | codex | acp | openai (--backend also accepted)

Examples:
  tny ask "explain src/main.c"
  tny ask --json --provider openai "list exported symbols"
  tny --provider cursor ask --model composer-2 "fix the leak"
```

Missing required values print the error, then a correct example, then exit 1. No timed prompts.
