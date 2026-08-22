# Architecture

tny is a **frontend + native loop**, not a fourth coding agent. Host backends already own planning, tools, and sandboxing. The native OpenAI-compatible backend is the only place tny executes tools itself.

```text
                    +-------------------------------------+
                    |  cli / tui  (one event loop)        |
                    +------------------+------------------+
                                       | normalized events
                    +------------------v------------------+
                    |  session + permission + render bus  |
                    +------------------+------------------+
           +---------------+-----------+----------+---------------+
           v               v                      v               v
     cursor-bridge    codex-app-server        acp-client     openai-native
     spawn/attach     ws:// or unix://        spawn stdio    HTTP SSE + tools
     Connect sdk.v1   JSON-RPC (no jsonrpc)   JSON-RPC 2.0   tny owns tools
           |               |                      |               |
           v               v                      v               v
     cursor-sdk-bridge  codex app-server    gemini/claude/...  provider API
```

## Two kinds of backend

| Kind | Backends | Who runs tools? | tny role |
| --- | --- | --- | --- |
| **Host** | Cursor bridge, Codex app-server, ACP client | The host process | Protocol client, approvals UI, session mapping |
| **Native** | OpenAI-compatible | tny | Agent loop, MCP, skills, sandbox, ACP **server**, `read_image` |

Never leak host-specific types into the TUI. Map every backend onto one event set: `text_delta`, `thinking`, `tool_start`, `tool_end`, `permission_request`, `plan`, `usage`, `turn_end`, `error`, `status`, `steer_rejected` (a mid-turn `steer()` the host refused after accepting it; the event carries the rejected text and the frontend re-queues it — [ADR 0011](adr/0011-mid-turn-input-steer-or-queue.md), [ADR 0013](adr/0013-steer-rejection-owns-the-text.md)).

## Process rules

- One tny process, one primary workspace (`cwd` unless `--cwd`).
- Host processes are children or attach targets. Do not embed Node/Bun/Rust runtimes.
- Always have a RAII-style shutdown path: cancel turn → close stream → `Shutdown`/EOF → wait → kill.
- Drain host stderr on a dedicated reader. A full pipe stalls `cursor-sdk-bridge` and most ACP agents.
- Never log bearer tokens, ready-line JSON, or `.env` values.

## Config and state

| Path | Contents |
| --- | --- |
| `~/.tny/settings.json` | Model, permission mode, UI, per-workspace overrides |
| `~/.tny/mcp.json` | Trusted MCP servers only (never repo-local MCP) |
| `~/.tny/sessions/` | Transcripts and recovery checkpoints |
| `~/.tny/skills/` | Managed skill installs |
| `<repo>/.tny.json` | Repo-safe limits only (steps, tool result bytes, sandbox, context on/off) |
| `<repo>/AGENTS.md` | Project instructions (also `CLAUDE.md` as alias if present) |

Credentials stay in the OS store or env vars (`CURSOR_API_KEY`, `OPENAI_API_KEY`, provider-specific keys, Codex's own login). Not in project JSON.

## Shared internals (implement later)

```text
src/
  main.c            # argv → cli or tui
  cli/              # ask, resume, doctor, acp, status
  tui/              # ANSI renderer, input, slash/@/$
  core/             # events, session store, permissions, AGENTS.md loader, images
  backends/
    cursor/         # bridge manager + Connect client
    codex/          # websocket JSON-RPC
    acp/            # client + server
    openai/         # HTTP + SSE + tool loop
  net/              # http1, connect framing, websocket, tls shim
  json/             # yyjson wrappers
  proto/            # nanopb sdk.v1 (generated, not edited)
  mcp/              # used by native loop and ACP server
```

POSIX `poll`/`kqueue` only. No libuv, no threads-per-connection unless a host callback server requires it. One deliberate exception: the TUI's pre-warm runs a single backend `connect()` on a detached pthread at startup ([ADR 0002](adr/0002-tui-provider-prewarm.md)); the connected backend is handed back before any turn starts, so all events still flow through the one event loop.

## ACP server vs ACP client

- `tny --backend acp --agent <cmd>` is an **ACP client** (drive Gemini CLI, `agent acp`, OpenCode, …).
- `tny acp` is an **ACP server** exposing the **native** OpenAI-compatible loop (fx parity).

Do not serve Cursor-bridge or Codex sessions through `tny acp`. Those hosts already have their own ACP or IDE surfaces.
