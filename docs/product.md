# Product

## Goal

tny is a **harness for agents, built by agents, focused on the agent**. User
constraints and tasks are the goal. Ship a **fast, portable and small**
coding-agent harness with a Unix-like TUI and a scriptable CLI. Keep it
maintainable and reliable with explicit C++ ownership. There is no binary-size
ceiling and no competitor-size target ([ADR 0150](adr/0150-agent-first-harness-and-measured-footprint.md)).

An agent-first harness is measured by:

| Property | Meaning |
| --- | --- |
| Effective context | The agent receives the instructions, tools, files and results the task needs. Token count is not a minimization contest. Runtime payload MiB caps remain safety bounds. |
| Reliable discovery and editing | Documented tools find and change the workspace. Exact edits fail closed with actionable errors. |
| Explicit authority | Permission mode, sandbox, worktrees and isolation are named and observable. Defaults stay yolo; ask/auto are opt-ins. |
| Reversible operations | Undo, worktree keep/remove/merge, session recover, and cancel are first-class. |
| Observed completion | A turn is done when required checks ran or blockers are named. CLI exit codes, `--json`, and the docs/test/quality contract are the evidence. |
| Task constraints preserved | User flags, project `AGENTS.md`, provider/model/effort, workspace and extra dirs survive the turn. |
| Model-agnostic interfaces | One event set and one CLI/TUI over Cursor, Codex, ACP and OpenAI-compatible providers. |

The final report stays skim-readable. Private reasoning need not be dumped.

Required backends (all first-class):

1. **Cursor Agent** via the [Cursor SDK Bridge](https://cursor.com/docs/sdk/bridge) (`sdk.v1`, Connect over HTTP/1.1).
2. **Codex** subscriptions via the Responses-compatible **ChatGPT backend** (`chatgpt.com/backend-api/codex`), a builtin profile of the native loop ([ADR 0065](adr/0065-codex-chatgpt-responses-backend.md)).
3. **Other agents** via [ACP](https://agentclientprotocol.com/) (JSON-RPC over stdio).
4. **OpenAI-compatible** HTTP providers (native tool loop owned by tny).

tny uses **C11 with scoped private C++20 ownership modules** (ADR 0114, ADR 0126
and ADR 0133). Prioritize fast startup, extensibility and reliability. Measure
artifact size and runtime dependencies; do not treat a byte count as a gate.
Same-target size comparisons with other harnesses are historical evidence when
dated, not a product goal.

## What "keep the functionality" means

Keep the *user-visible harness*, not another vendor's branding:

- Interactive shell: streaming transcript, `/` commands, `@` file picker, `$` skill picker, interrupt, resume.
- Dictation: microphone speech to an editable prompt, using an STT provider independently of the agent provider.
- Prompt optimisation: `/optimise` or Ctrl-O rewrites a draft using relevant project files and an independently configured model, with review before submission.
- One-shot `ask` for scripts/CI with Markdown on stdout and JSON mode.
- Sessions: list, inspect, resume `last` or id, compact, recover.
- Git worktrees: optional isolated checkouts, named reuse, and explicit merge/remove/keep on TUI exit.
- Permissions: `ask` / `auto` / `yolo`, persistent rules, session grants, command sandbox.
- Built-in tools (files, grep/glob, shell, web fetch/search, vision fallback, memory, speech, image generation/editing).
- Skills (`SKILL.md`), MCP client, session-backed subagents.
- ACP **server** so editors can drive tny's native loop (`tny acp`).
- `status`, `doctor`, models, usage, workspace extra dirs, project `AGENTS.md` (over `--ssh`: remote cwd, not the launch directory).

## What tny adds

tny is a **thin multiplexed frontend** over host harnesses (Cursor, ACP
agents), plus a native OpenAI-compatible loop for BYOK providers (OpenRouter,
Groq, local llama.cpp, Azure, etc.) and subscription logins (Codex, Claude,
Grok). Host binaries stay external.

## Embedding

The native harness is being extracted behind an experimental headless C ABI
(`libtny`, [ADR 0023](adr/0023-libtny-embedding-abi.md)). The CLI, TUI, ACP
server, and C embedders share one runtime; the public ABI does not expose the
private backend or `tny_backend_event` structs. Python/cffi and native
TypeScript/Node-API packages are thin scheduler and type adapters over that
same ABI ([SDK contract](sdks.md)); they do not contain provider-wire logic.

## Non-goals (v1)

- A browser/wasm JavaScript embedding API. The binary itself compiles to wasm
  for the GitHub Pages terminal ([ADR 0017](adr/0017-wasm-browser-parity.md));
  the native Node-API SDK is a separate shared-library artifact and does not
  imply browser support.
- Reimplementing Cursor or Codex agent loops inside tny.
- Bundling `cursor-sdk-bridge` or ACP agents into the tny binary (spawn or attach).
- Vercel OAuth, AI Gateway team picker, or vendor login lock-in.
- Completion sounds, terminal recordings, or issue/PR wrappers (optional later).
- A heavy full-screen IDE TUI (ratatui/ncurses panels, mouse-first layouts).
- A fixed binary-size ceiling, or a product goal to undercut another harness
  on artifact size.

## Success metrics

| Metric | Target |
| --- | --- |
| Agent-first properties | The table above holds for interactive and one-shot turns |
| Stripped `tny` | Measured and reported per platform, with runtime dependencies listed separately; no byte ceiling |
| Cold start to interactive prompt (no backend spawn) | **< 10 ms** |
| `tny --version` / `tny ask --help` | **< 5 ms** median, stretch **< 2 ms** |
| First token display after backend stream starts | UI overhead **< 2 ms** |
| Feature gate | Parity table in [features/parity-with-fx.md](features/parity-with-fx.md) is green for v1 rows |

Host binaries (bridge, ACP agents) are **not** part of the tny artifact.
Dated bake-off numbers versus fx v0.0.3 live in
[size-and-speed.md](size-and-speed.md) as historical measurements.
