# 0049 — MCP servers warm in the background; the model sees a cached catalog

Date: 2026-08-31
Status: accepted (amends the "nothing is spawned until the model uses an
mcp_* tool" MCP invariant, the way ADR 0002 amended host spawn)

## Context

MCP was fully lazy: the first `mcp_search_tools` / `mcp_select_tool` paid
spawn + `initialize` + `tools/list` for every configured server, serially,
on the model's clock — seconds of tool-call latency before the first real
result. Worse, the model had no idea what MCP tools existed without spending
a round trip on a search, so sessions with a populated `~/.tny/mcp.json`
started blind.

The alternative of promoting MCP tools into the OpenAI `tools` array was
rejected: full JSON schemas for large catalogs blow up every request, and
the permission identity (`mcp:server/tool`) plus the untrusted-output
boundary live on the `mcp_select_tool` chokepoint.

## Decision

**A native session starts every `~/.tny/mcp.json` server in the background
at session start, TUI-prewarm style, and injects a compact cached catalog
into the system prompt. Calls still route through `mcp_select_tool`.**

Mechanics (`src/mcp/mcp.c`):

- `mcp_warm_start(ctx)` runs once per process at native-session start: the
  TUI calls it from `tui_prewarm_start` (after first paint, and again for
  free when `/provider` switches back to the native loop); `tny ask` calls
  it right before its backend connect (after the `-B` fork, so the servers
  are the background child's children and die with its process group).
  `--help`/`--version` never reach either call site, and `tny acp` server
  mode plus libtny embeds are excluded (`mcp_disabled` / `library_mode`).
- One detached pthread per server runs spawn + `initialize` +
  `tools/list` and commits the connection under a mutex. A tool call that
  names a server mid-warm waits on the condvar — the same cost the lazy
  path paid (the `tui_prewarm_take` contract). Nothing else ever blocks.
- The per-request system prompt (openai backend) appends the cached catalog:
  `server/tool — one-line description`, capped at 120 bytes per description
  and 64 tools per session; overflow becomes "+N more — find them with
  mcp_search_tools". Built per request, so it appears as soon as a server's
  handshake lands, with no `tools/list` on the model's clock. MCP tools are
  **not** promoted into the `tools` array; the only MCP entries in the
  function schema remain `mcp_search_tools`, `mcp_select_tool`,
  `mcp_features`.
- A failed warm-up is silent until a call names that server; the call
  retries the spawn synchronously and reports the existing error. Where
  threads are unavailable (wasm), every slot stays lazy and native spawn
  keeps its clean error.
- `mcp_search_tools` now AND-matches whitespace-separated tokens against
  `name description` (not one contiguous substring); an empty query lists
  the cached catalog without starting or waiting for anything.
- Shutdown abandons servers still mid-handshake instead of waiting out
  their 30 s timeout: the warm thread cleans up on commit, and a server
  that outlives the process exits on stdin EOF.

## Consequences

- First MCP use costs one `tools/call`, not N handshakes; the model knows
  the catalog from turn one.
- Warmed servers freeze the session's launch cwd. The profile is
  user-global, so this matches intent; a workspace switch does not respawn
  them.
- Repo-local MCP files remain unread; permission identity and the
  untrusted-data boundary are unchanged.
- Unit coverage: `tests/test_mcp.c` (fake stdio servers: non-blocking
  start, catalog without search, token search, hung server, silent
  failure, workspace profile never loads).
