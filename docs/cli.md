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
| codex | `--codex-ws URL` to attach; without it tny spawns `codex app-server` on an ephemeral port (never a fixed port that could collide). `--codex-bin`, `--ws-token-file`, `CODEX_REMOTE_TOKEN` |
| acp | `--agent CMD` plus extra args after `--`, e.g. `tny --provider acp --agent gemini -- acp` |
| openai | `--base-url`, `--api-key-env NAME`, `OPENAI_BASE_URL`, `OPENAI_API_KEY` |

`tny ask` never blocks on an approval. Unresolved permissions fail the run unless `--auto` reviews (native loop) or `--yolo`. Host providers must be pre-authorized or they fail closed.

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
