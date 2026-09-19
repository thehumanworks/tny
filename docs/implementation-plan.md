# Implementation plan

Phases 0–8 describe the completed path from the original research skeleton to
the current product. New architecture work follows the accepted ADRs and must
preserve the gates below; [ADR 0023](adr/0023-libtny-embedding-abi.md) defines
the ordered extraction and verification gates for `libtny`. ABI 0.3 begins the
SDK foundation with the canonical public event registry and sized event view
([ADR 0030](adr/0030-public-event-schema.md), issues #62/#63). ABI 0.5 removes
the public-runtime singleton and adds scheduler-safe cancellation through the
single `tny_poll` seam ([ADR 0033](adr/0033-libtny-multi-runtime-cancel.md)).

## Phase 0 — skeleton

- Makefile, `src/main.c` printing `--version` / `--help`
- `tny doctor --json` reports OS, libc, missing optional host binaries
- Footprint: stripped artifact size and runtime dependencies measured and reported; no byte ceiling.

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

**Gate:** measure `tny --version` with hyperfine against the pre-change baseline; first prompt < 10 ms.

## Phase 3 — ACP client

- Spawn `--agent`, ACP **v1** initialize, session/prompt (pending until stopReason), updates, cancel
- Always answer `session/request_permission`; Cursor extras if the argv is `agent acp`

**Gate:** scripted fake ACP agent (fixture) plus one real agent if installed.

## Phase 4 — Codex

- Originally a `codex app-server` WebSocket client; replaced by the builtin
  `codex` profile on the native loop against the ChatGPT Responses backend
  ([ADR 0065](adr/0065-codex-chatgpt-responses-backend.md))
- `auth.json` credentials, account-id header, tny-run token refresh

**Gate:** `tests/integration/test_codex_chatgpt.py` against the strict Responses mock.

## Phase 5 — Cursor SDK Bridge

- Bridge manager (ready line, bearer file, stderr drain)
- Ping, ListModels, CreateAgent local, Send stream, Shutdown
- JSON Connect first, nanopb when streams are correct

**Gate:** curl smoke test in CI (skip if no `CURSOR_API_KEY`); unit-test ready-line parser.

## Phase 6 — ACP server + remaining harness tools

- `tny acp` over the native loop
- MCP, skills, subagents, `/undo`, extra dirs, compact, doctor polish

**Gate:** Zed or a tiny ACP client can run one native turn.

## Phase 7 — measurements and feature inventory

- Publish reproducible footprint and startup measurements in [size-and-speed.md](size-and-speed.md), without a competitor target ([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)).
- Fill [parity-with-fx.md](features/parity-with-fx.md)

## Phase 8 — wasm browser parity (docs/adr/0017) — DONE

- `tny_poll` + per-platform source lists; `src/net/net_wasm.c` (fetch, WebSocket, pseudo-fd registry, Asyncify)
- `make wasm` (node, NODERAWFS, CI) and `make wasm-web` (browser, MEMFS) from one object set
- ACP `--agent ws://` remote transport, native and wasm; codex over HTTPS under wasm
- The landing page runs the artifact in xterm.js; the JS agent loop is deleted
- CI: the same openai/acp/codex-profile mock suites against `TNY=build/wasm/tny`, measured wasm artifact size, and a headless-browser smoke

## Hard rules during implementation

- Private C++20 ownership/decoding sources only in the areas authorized by [ADR 0114](adr/0114-private-cpp20-ownership-boundaries.md), [ADR 0126](adr/0126-checkpoint-context-ownership.md), and [ADR 0133](adr/0133-owned-subagent-launch-snapshots.md); all other existing C stays C11.
- No new dependency without updating [language-and-runtime.md](language-and-runtime.md) and measuring the artifact plus runtime dependencies.
- No secrets in the repo. Tests use fixtures, not live keys, unless the user opted in.
- Do not implement exploit/PoC code for any system.
- Prefer extending the native tool list over adding UI frameworks.
