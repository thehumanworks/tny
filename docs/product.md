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
| Model-agnostic interfaces | One event set and one CLI/TUI over native OpenAI-compatible HTTP profiles. |

The final report stays skim-readable. Private reasoning need not be dumped.

One native HTTP backend supports Responses and Chat Completions. Named
profiles cover environment-key gateways (OpenRouter, explicitly configured
AIProxy and others), Codex's ChatGPT Responses subscription, and Grok's public
API or compatible subscription proxy. No external agent binary is required.
See [ADR 0152](adr/0152-native-http-only-providers.md). Optional external ACP
clients connect through the owning-runtime MCP bridge ([ADR 0164](adr/0164-optional-acp-clients.md));
HTTP operation still needs no external agent executable.

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
- `status`, `doctor`, models, usage, workspace extra dirs, project `AGENTS.md` (over `--ssh`: remote cwd, not the launch directory).

## What tny adds

tny owns tools, permissions and context. Agent tool calls use bounded Lua through
`run_code`, with each native invocation in a fresh execution server
([ADR 0174](adr/0174-execution-server-code-mode.md)). HTTP uses the native agent loop;
optional ACP agents own their loop and reach tny tools through MCP.
BYOK keys come from environment variables; OAuth subscription login and
refresh are native. Claude models work through configured compatible gateways.

## Bounded self-improvement

[Automatic workflow learning](instruction-improvement.md) is on by default in
normal CLI/TUI work. Verified recovery outcomes revise bounded, workspace-scoped
advice used in later requests and sessions. It currently learns exact-edit
recovery, with no extra inference or user-selected preset. An explicit opt-out is
available. A separate optional controller can evolve larger task-instruction
bodies. Neither trains model weights or rewrites harness code automatically.
Offline replay results are not claims of general live coding gains.

## Embedding

The native harness is being extracted behind an experimental headless C ABI
(`libtny`, [ADR 0023](adr/0023-libtny-embedding-abi.md)). The CLI, TUI, and C embedders share one runtime; the public ABI does not expose the
private backend or `tny_backend_event` structs. Python/cffi and native
TypeScript/Node-API packages are thin scheduler and type adapters over that
same ABI ([SDK contract](sdks.md)); they do not contain provider-wire logic.

## Non-goals (v1)

- A browser/wasm JavaScript embedding API. The binary itself compiles to wasm
  for the GitHub Pages terminal ([ADR 0017](adr/0017-wasm-browser-parity.md));
  the native Node-API SDK is a separate shared-library artifact and does not
  imply browser support.
- Depending on another vendor's agent executable.
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

No vendor agent binary is a runtime dependency.
Dated bake-off numbers versus fx v0.0.3 live in
[size-and-speed.md](size-and-speed.md) as historical measurements.

Opt-in [collective swarm mode](collective-swarm.md) adds peer proposals, challenges,
shared-channel publication and bounded event-driven mailbox waits over durable teams.
[Purposeful swarm files](purposeful-swarms.md) add strictly validated names, purposes
and bounded nested coordination, compiled into that same durable runtime.
