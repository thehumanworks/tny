# Implementation plan

Research is done. Product code is **not** in this repository yet. When implementation starts, follow this order. Each phase has an acceptance gate. Do not skip ahead to TUI chrome before the first backend streams.

## Phase 0 — skeleton

- Makefile, `src/main.c` printing `--version` / `--help`
- `tny doctor --json` reports OS, libc, missing optional host binaries
- Size: stripped binary already under the 1.5 MiB Linux / 1.8 MiB macOS gate (it should be tens of KB)

## Phase 1 — CLI + OpenAI-compatible

- `tny ask` non-stream then SSE
- Tool loop with `read_file`, `list_files`, `terminal` behind `ask` permissions
- `--json`, exit codes 0/1/2/130
- Session save/resume last

**Gate:** `tny ask --json "list files in ."` against a local OpenAI-compatible mock.

## Phase 2 — TUI

- ANSI transcript + composer + `/help` `/quit` `/status`
- Interrupt, multiline, prompt history
- Same native loop as `ask`

**Gate:** hyperfine `tny --version` vs `fx --version`; first prompt < 10 ms.

## Phase 3 — ACP client

- Spawn `--agent`, ACP **v1** initialize, session/prompt (pending until stopReason), updates, cancel
- Always answer `session/request_permission`; Cursor extras if the argv is `agent acp`

**Gate:** scripted fake ACP agent (fixture) plus one real agent if installed.

## Phase 4 — Codex WebSocket

- Attach and spawn `codex app-server --listen ws://127.0.0.1:4500`
- initialize / thread/start / turn/start / deltas / interrupt
- Token file auth

**Gate:** fixture WS server from a recorded JSON-RPC transcript.

## Phase 5 — Cursor SDK Bridge

- Bridge manager (ready line, bearer file, stderr drain)
- Ping, ListModels, CreateAgent local, Send stream, Shutdown
- JSON Connect first, nanopb when streams are correct

**Gate:** curl smoke test in CI (skip if no `CURSOR_API_KEY`); unit-test ready-line parser.

## Phase 6 — ACP server + remaining fx tools

- `tny acp` over the native loop
- MCP, skills, subagents, `/undo`, extra dirs, compact, doctor polish

**Gate:** Zed or a tiny ACP client can run one native turn.

## Phase 7 — bake-off

- Publish size/speed table vs the fx version pinned in [size-and-speed.md](size-and-speed.md)
- Fill [parity-with-fx.md](features/parity-with-fx.md)

## Phase 8 — wasm browser parity (docs/adr/0017) — DONE

- `tny_poll` + per-platform source lists; `src/net/net_wasm.c` (fetch, WebSocket, pseudo-fd registry, Asyncify)
- `make wasm` (node, NODERAWFS, CI) and `make wasm-web` (browser, MEMFS) from one object set
- ACP `--agent ws://` remote transport, native and wasm; codex attach under wasm
- The landing page runs the artifact in xterm.js; the JS agent loop is deleted
- CI: the same openai/acp/codex mock suites against `TNY=build/wasm/tny`, a size guard, and a headless-browser smoke

## Hard rules during implementation

- No C++ sources.
- No new dependency without updating [language-and-runtime.md](language-and-runtime.md) and the size budget.
- No secrets in the repo. Tests use fixtures, not live keys, unless the user opted in.
- Do not implement exploit/PoC code for any system.
- Prefer extending the native tool list over adding UI frameworks.
