# Architecture

tny is a **frontend + native loop**, not a fourth coding agent. Host backends already own planning, tools, and sandboxing. The native OpenAI-compatible backend is the only place tny executes tools itself.

```text
                    +-------------------------------------+
                    | cli / tui / acp / libtny adapters   |
                    +------------------+------------------+
                                       | normalized events
                    +------------------v------------------+
                    | private runtime + session + events  |
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

Python extensions consume a versioned superset of that renderer vocabulary.
[ADR 0028](adr/0028-extension-parity-contract.md) freezes its lifecycle/control
names and immutable provider capability matrix. The matrix distinguishes
native-owned control from host-owned observation; an adapter may not silently
approximate an unsupported action or expose raw provider payloads.

The native tool loop calls the shared runtime only at quiescent control
boundaries: pre-tool before validation, unresolved permission before execution,
post-tool before result persistence, batch before the next POST, and allowlisted
provider request/response edges. The callback never runs from a backend event
callback or re-enters the backend. Extension-free calls return without
allocating event JSON or starting Python.

## Embedding boundary

[`libtny`](adr/0023-libtny-embedding-abi.md) exposes opaque
runtime/session/event/error handles through a pull-driven C ABI. It does
not expose `tny_ctx`, `tny_backend`, `tny_backend_event`, yyjson, or `pollfd`
layouts.
The public `next_event` operation and the CLI adapters drive the same private
runtime engine. TUI prewarm remains an acceleration adapter over that engine,
not a separate provider lifecycle.

## Process rules

- One tny process, one primary workspace (`cwd` unless `--cwd`).
- Host processes are children or attach targets. Do not embed Node/Bun/Rust runtimes.
- Always have a RAII-style shutdown path: cancel turn → close stream → `Shutdown`/EOF → wait → kill.
- Drain host stderr on a dedicated reader. A full pipe stalls `cursor-sdk-bridge` and most ACP agents.
- Never log bearer tokens, ready-line JSON, or `.env` values.

## Config and state

| Path | Contents |
| --- | --- |
| `~/.tny/settings.json` | Provider/model/effort/fast defaults, permission mode, named provider/ACP-agent profiles, UI, per-workspace overrides ([schema](../schemas/settings.schema.json)) |
| `~/.tny/mcp.json` | Trusted MCP servers only (never repo-local MCP) |
| `~/.tny/sessions/` | Transcripts and recovery checkpoints |
| `~/.tny/skills/` | Managed skill installs |
| `~/.tny/extensions/` | Trusted global Python event hooks (`*.py`, `*/index.py`) |
| `<repo>/.tny.json` | Repo-safe limits only (steps, tool result bytes, sandbox, context on/off) |
| `<repo>/AGENTS.md` | Project instructions (also `CLAUDE.md` as alias if present). Over `--ssh`, the remote cwd's file is used instead of this local path ([ADR 0040](adr/0040-ssh-agents-md.md)) |

Credentials stay in the OS store or env vars (`CURSOR_API_KEY`, `OPENAI_API_KEY`, provider-specific keys, Codex's own login). Not in project JSON.

## Shared internals

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

POSIX `poll`/`kqueue` only, always through the `tny_poll` seam (`src/util/tny_poll.h`): native forwards to `poll(2)`; the wasm build ([ADR 0017](adr/0017-wasm-browser-parity.md)) waits on `net_wasm.c`'s pseudo-fd registry and yields to the JS event loop via Asyncify. `src/net/net.h` is the transport boundary — on wasm, `http_conn` rides `fetch()` and `ws_conn` the browser/node WebSocket, with `tcp.c`/`stream.c`/`http1.c`/`ws.c` excluded from the source list wholesale. No libuv, no threads-per-connection unless a host callback server requires it. One deliberate exception: the TUI's pre-warm runs a single backend `connect()` on a detached pthread at startup ([ADR 0002](adr/0002-tui-provider-prewarm.md)); the connected backend is handed back before any turn starts, so all events still flow through the one event loop.

## ACP server vs ACP client

- `tny --backend acp --agent <cmd>` is an **ACP client** (drive Gemini CLI, `agent acp`, OpenCode, …).
- `tny acp` is an **ACP server** exposing the **native** OpenAI-compatible loop (fx parity).

Do not serve Cursor-bridge or Codex sessions through `tny acp`. Those hosts already have their own ACP or IDE surfaces.
