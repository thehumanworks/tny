# Architecture

tny is a **frontend + native loop**, not a fourth coding agent. Host backends
already own planning, tools, and sandboxing. The native OpenAI-compatible
backend owns the general tny tool loop; explicitly registered Cursor custom
tools are the narrow callback exception.

```text
                    +-------------------------------------+
                    | cli / tui / acp / libtny adapters   |
                    +------------------+------------------+
                                       | normalized events
                    +------------------v------------------+
                    | private runtime + session + events  |
                    +------------------+------------------+
           +---------------+-----------+----------+
           v               v                      v
     cursor-bridge      acp-client          openai-native
     spawn              spawn stdio         HTTP SSE + tools
     Connect sdk.v1     JSON-RPC 2.0        tny owns tools
     + callbacks                            (openai, codex, claude, grok,
           |               |                 named profiles)
           v               v                      v
     cursor-sdk-bridge  gemini/claude/...   provider API / chatgpt.com
```

## Two kinds of backend

| Kind | Backends | Who runs tools? | tny role |
| --- | --- | --- | --- |
| **Host** | Cursor bridge, ACP client | The host process | Protocol client, approvals UI, session mapping; Cursor custom-tool/store callbacks are explicit exceptions |
| **Native** | OpenAI-compatible, including the builtin codex / claude / grok subscription profiles | tny | Agent loop, MCP, skills, sandbox, ACP **server**, `read_image` |

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

Cursor's reverse callbacks do not change that ownership model. The bridge owns
the agent loop and its built-in tools. A registered libtny custom tool is
executed only through the authenticated `CallCustomTool` boundary; an optional
`CallStore` service owns only local agent/run/event/checkpoint persistence.
Both share one bounded loopback HTTP server. Blocking Create/Resume lends that
server to a bounded pump thread for store traffic only; normal Send/Observe
polls its fds in the main event loop.

## Embedding boundary

[`libtny`](adr/0023-libtny-embedding-abi.md) exposes opaque
runtime/session/event/error handles through a pull-driven C ABI. It does
not expose `tny_ctx`, `tny_backend`, `tny_backend_event`, yyjson, or `pollfd`
layouts.
The public `next_event` operation and the CLI adapters drive the same private
runtime engine. TUI prewarm remains an acceleration adapter over that engine,
not a separate provider lifecycle.

The runtime provider selector supports Cursor conversations through that same
API: create/resume/send/cancel, normalized events, and registered custom
tools. ABI 1 has no image-send entry point. Cursor images and management RPCs
remain CLI surfaces and do not expand the embedding ABI.

## Process rules

- One tny process, one primary workspace (`cwd` unless `--cwd`).
- **Turns run in a detached session runner** ([ADR 0053](adr/0053-forked-turn-isolation.md)): on native builds, `ask` and the TUI fork a `setsid()` runner that owns the backend, engine, MCP servers, and every `session.json` write, streaming normalized events back over `<session>/sock` (NDJSON). The caller is a renderer; its death detaches, never kills, the turn. wasm, `--ephemeral`, and `TNY_ISOLATE=0` run in-process; on macOS a caller that has already initialized SecureTransport also keeps later turns in-process because Apple's trust runtime is unsafe in a fork-only child; `tny acp` (server) and libtny embedders stay in-process by design — their callers own lifecycle.
- Host processes are children or attach targets. Do not embed Node/Bun/Rust runtimes.
- Always have a RAII-style shutdown path: cancel turn → close stream → `Shutdown`/EOF → wait → kill.
- Drain host stderr on a dedicated reader. A full pipe stalls `cursor-sdk-bridge` and most ACP agents.
- Never log bearer tokens, ready-line JSON, or `.env` values.

## Config and state

| Path | Contents |
| --- | --- |
| `~/.tny/settings.json` | Provider/model/effort/fast defaults, trusted Cursor sdk.v1 options, permission mode, named provider/ACP-agent profiles, UI, per-workspace overrides, optional `mcp.import_from` ([schema](../schemas/settings.schema.json)) |
| `~/.tny/mcp.json` | Authoritative stdio and Streamable HTTP MCP servers (never repo-local MCP). Foreign user/project configs load only when global `mcp.import_from` explicitly names their harness ([ADR 0052](adr/0052-mcp-import-from-harnesses.md)) |
| `~/.tny/sessions/` | Transcripts and recovery checkpoints |
| `~/.tny/skills/` | Managed skill installs |
| `~/.tny/extensions/` | Trusted global Python event hooks (`*.py`, `*/index.py`) |
| `~/.tny/tasks/` | User task-preset Markdown definitions (`NAME.md`) |
| `<repo>/.tny.json` | Repo-safe limits only (steps, tool result bytes, sandbox, context on/off) |
| `<repo>/.tny/tasks/` | Project task-preset Markdown definitions; project files cannot add authority or cost |
| `<repo>/AGENTS.md` | Project instructions (also `CLAUDE.md` as alias if present). Over `--ssh`, the remote cwd's file is used instead of this local path ([ADR 0040](adr/0040-ssh-agents-md.md)) |

Credentials stay in the OS store or env vars (`CURSOR_API_KEY`, `OPENAI_API_KEY`, provider-specific keys, MCP `header_env` / `bearer_token_env`, Codex's own login). Not in project JSON.

## Shared internals

```text
src/
  main.c            # argv → cli or tui
  cli/              # ask, resume, doctor, acp, status
  tui/              # ANSI renderer, input, slash/@/$
  core/             # events, session store, permissions, AGENTS.md loader, images
  backends/
    cursor/         # v1.0.30 bridge/client, recovery, management, callbacks
    acp/            # client + server
    openai/         # HTTP + SSE + tool loop
  net/              # http1, connect framing, websocket, tls shim
  json/             # yyjson wrappers
  mcp/              # used by native loop and ACP server
third_party/
  cursor-sdk-bridge/v1.0.30/ # pinned sdk.v1 protos/contract; never hand-edited
```

POSIX `poll`/`kqueue` only, always through the `tny_poll` seam (`src/util/tny_poll.h`): native forwards to `poll(2)`; the wasm build ([ADR 0017](adr/0017-wasm-browser-parity.md)) waits on `net_wasm.c`'s pseudo-fd registry and yields to the JS event loop via Asyncify. `src/net/net.h` is the transport boundary — on wasm, `http_conn` rides `fetch()` and `ws_conn` the browser/node WebSocket, with `tcp.c`/`stream.c`/`http1.c`/`ws.c` excluded from the source list wholesale. Remote MCP reuses that same `http_conn` boundary ([ADR 0051](adr/0051-mcp-streamable-http.md)); it does not add a platform seam, and wasm is remote-only. No libuv, no threads-per-connection unless a host callback server requires it. One deliberate exception: the TUI's pre-warm runs a single backend `connect()` on a detached pthread at startup ([ADR 0002](adr/0002-tui-provider-prewarm.md)); the connected backend is handed back before any turn starts, so all events still flow through the one event loop.

## ACP server vs ACP client

- `tny --backend acp --agent <cmd>` is an **ACP client** (drive Gemini CLI, `agent acp`, OpenCode, …).
- `tny acp` is an **ACP server** exposing the **native** OpenAI-compatible loop (fx parity).

Do not serve Cursor-bridge or Codex sessions through `tny acp`. Those hosts already have their own ACP or IDE surfaces.
