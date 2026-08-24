# Product

## Goal

Ship a **fast, tiny** coding-agent harness with a Unix-like TUI and a scriptable CLI. Beat [vercel-labs/fx](https://github.com/vercel-labs/fx) on **binary size** and **startup/runtime speed** while keeping fx's functionality.

Required backends (all first-class):

1. **Cursor Agent** via the [Cursor SDK Bridge](https://cursor.com/docs/sdk/bridge) (`sdk.v1`, Connect over HTTP/1.1).
2. **Codex** via `codex app-server` using **WebSockets** (JSON-RPC text frames).
3. **Other agents** via [ACP](https://agentclientprotocol.com/) (JSON-RPC over stdio).
4. **OpenAI-compatible** HTTP providers (native tool loop owned by tny).

fx is Zig 0.16, Apache-2.0, experimental: **6.44 MiB** macOS arm64 and **11.12 MiB** static Linux in v0.0.3 ([fx.sh](https://fx.sh), [README](https://github.com/vercel-labs/fx) still says 7.8 MiB). Inference is Vercel AI Gateway (AI SDK LM spec v4), not public OpenAI-compat. tny is **C11** so the CLI can undercut those binaries and talk to BYOK providers plus host agents.

## What "keep the functionality" means

Keep the *user-visible harness*, not Vercel branding:

- Interactive shell: streaming transcript, `/` commands, `@` file picker, `$` skill picker, interrupt, resume.
- One-shot `ask` for scripts/CI with Markdown on stdout and JSON mode.
- Sessions: list, inspect, resume `last` or id, compact, recover.
- Permissions: `ask` / `auto` / `yolo`, persistent rules, session grants, command sandbox.
- Built-in tools (files, grep/glob, shell, web fetch/search, vision fallback, memory).
- Skills (`SKILL.md`), MCP client, session-backed subagents.
- ACP **server** so editors can drive tny's native loop (`tny acp`), matching `fx acp`.
- `status`, `doctor`, models, usage, workspace extra dirs, project `AGENTS.md`.

## What tny adds

fx talks to Vercel AI Gateway and can *be* an ACP server. It does not ship Cursor SDK Bridge or Codex app-server WebSocket clients. tny's extra job is a **thin multiplexed frontend** over those host harnesses, plus a native OpenAI-compatible loop for BYOK providers (OpenRouter, Groq, local llama.cpp, Azure, etc.).

## Embedding

The native harness is being extracted behind an experimental headless C ABI
(`libtny`, [ADR 0023](adr/0023-libtny-embedding-abi.md)). The CLI, TUI, ACP
server, and C embedders share one runtime; the public ABI does not expose the
private backend or `tny_backend_event` structs.

## Non-goals (v1)

- A JS `createFxAgent()`-style embedding API (fx has this; defer). The binary itself *does* compile to wasm and runs the GitHub Pages landing terminal ([ADR 0017](adr/0017-wasm-browser-parity.md)); what stays out of scope is a JS API surface around it, not the native C ABI.
- Reimplementing Cursor or Codex agent loops inside tny.
- Bundling `cursor-sdk-bridge` or `codex` into the tny binary (spawn or attach).
- Vercel OAuth, AI Gateway team picker, or `fx login` lock-in.
- Completion sounds, terminal recordings, or `fx pr` / `fx issue` (optional later).
- A heavy full-screen IDE TUI (ratatui/ncurses panels, mouse-first layouts).

## Success metrics

Measured on the same machine as a current `fx` release binary:

| Metric | Target |
| --- | --- |
| Stripped `tny` | **< 1.8 MiB macOS** / **< 1.5 MiB Linux musl**; must stay under fx’s 6.44 / 11.12 MiB |
| Cold start to interactive prompt (no backend spawn) | **< 10 ms** (do not claim fx’s 10 µs bench hook) |
| `tny --version` / `tny ask --help` | **< 5 ms** median, stretch **< 2 ms** |
| First token display after backend stream starts | UI overhead **< 2 ms** |
| Feature gate | Parity table in [features/parity-with-fx.md](features/parity-with-fx.md) is green for v1 rows |

Host binaries (bridge, Codex, ACP agents) are **not** counted in the tny size budget.
