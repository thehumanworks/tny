# tny

> This repository is a monorepo
> ([ADR 0045](docs/adr/0045-monorepo-and-tnytty.md)): the root is the tny
> harness; [`tnytty/`](tnytty/) is **tnytty, the tiny terminal** — a C11
> terminal emulator core with a pty host, kitty graphics + bundled `icat`,
> and a REST HTTP API for scripting and session sharing. See
> [`tnytty/docs/`](tnytty/docs/README.md).

A tiny C11 coding-agent harness: an interactive TUI shell and a
noninteractive-first CLI that drive four kinds of agent backends through one
normalized event loop.

```text
tny                          # interactive shell (lazy backend, ~4 ms first paint)
tny --ephemeral              # multi-turn shell with no local conversation store
tny ask --json "fix the failing test"
tny ask --ephemeral "review this without saving the session"
tny ask --backend cursor "explain this repo"
tny --backend acp --agent gemini -- acp -- ask "hi"
tny --task review ask "inspect the current diff"
tny acp                      # serve tny's native loop to any ACP client
```

`--task NAME` selects a runtime preset (`review`, `optimizer`, `document`, or
`retro`, plus `.tny/tasks/NAME.md` custom definitions). It is distinct from
ACP's `--agent CMD`, which selects the executable/WebSocket agent.

## Ephemeral sessions

Use `--ephemeral` before the command to keep a CLI, TUI, or ACP conversation
process-local. `tny ask` also accepts it after the subcommand, and `--no-save`
remains an alias.

The session stays multi-turn in memory while the process runs, but tny does not
write session JSON, recovery checkpoints, large tool-result blobs, or TUI
prompt history. Saved-session resume/import paths are disabled. Codex also
receives its native `thread/start` `ephemeral:true` flag; the OpenAI Responses
wire uses `store:false`. Other host agents can still apply their own remote
retention policy. See [ADR 0020](docs/adr/0020-ephemeral-sessions.md).

## Backends

| Backend | Transport | Host process |
| --- | --- | --- |
| `openai` (default) | OpenAI-compatible `/v1/chat/completions`, SSE streaming, native tool loop | none — tny owns tools/MCP/skills/permissions |
| `cursor` | [Cursor SDK Bridge](https://cursor.com/docs/sdk/bridge): Connect HTTP/1.1 (`sdk.v1`, JSON codec) | `cursor-sdk-bridge` (spawned, ready-line handshake) |
| `codex` | WebSocket JSON-RPC (`codex app-server`) | `codex` (attach via `--codex-ws`, auto-attach to a registered live host, or spawned on an ephemeral port) |
| `acp` | [ACP](https://agentclientprotocol.com/) over stdio JSONL | any ACP agent via `--agent CMD` |

All four normalize onto one event set (text/thinking/tool/permission/plan/
usage/turn-end) rendered by the same TUI and CLI. See
[docs/architecture.md](docs/architecture.md).

## Benchmark vs fx

tny exists to beat [vercel-labs/fx](https://github.com/vercel-labs/fx) on size
and startup while keeping its Unix-shell feature set. Measured against the real
fx v0.0.3 binary, macOS arm64, hyperfine, same machine (fx 2026-08-19,
tny 2026-08-20):

| Metric | fx 0.0.3 | tny 0.1.0 | Result |
| --- | --- | --- | --- |
| Stripped binary | 6,748,416 B (6.4 MiB) | **426,792 B (0.41 MiB)** | 15.8× smaller |
| `tny --version` | 2.2 ms ± 0.3 | **1.7 ms ± 0.2** | 1.3× faster |
| Max RSS (`--version`) | 3.0 MiB | **2.1 MiB** | 1.4× less memory |
| TUI first prompt | — | 3.3–4.3 ms | budget < 10 ms |

Feature parity notes and deliberate deferrals:
[docs/features/parity-with-fx.md](docs/features/parity-with-fx.md).

## Time to first token

Everything between Enter and the provider seeing the turn is pre-paid or
overlapped (`docs/adr/0002`, `docs/adr/0004`): the TUI warms the host **and**
creates/resumes the provider session in the background at startup, `tny ask`
connects while it reads a piped prompt, and codex one-shots attach to an
already-running app-server instead of spawning one. Measured with
`tests/bench/bench_ttft.py` (medians, scripted codex host, 400 ms injected
RPC delay):

| Path | before | after |
| --- | --- | --- |
| TUI: Enter → first output | 411 ms | **6 ms** |
| `ask` with piped stdin | 875 ms | **449 ms** |
| codex one-shot (attach vs spawn) | 235 ms | **11 ms** |

What remains is the model's own time to first token — client-side, tny is
not the bottleneck.

## Install with Nix

```sh
nix run github:thehumanworks/tny                    # no install
nix profile install github:thehumanworks/tny        # put tny on PATH
```

The flake builds from source — same tree CI builds — and exposes `tny`,
`tny-unwrapped`, `libtny`, an overlay, a dev shell, and `nix flake check`
running the full `make test` suite. Non-flake users get `nix-build -A tny` and
`nix-shell`. Details, including the TLS and version specifics Nix needs:
[docs/nix.md](docs/nix.md), [ADR 0035](docs/adr/0035-nix-flake-packaging.md).

## Scriptable workflows

Source the installed Bash/Zsh helpers to run dependency chains with bounded
parallel agents. Root tasks fan out; a dependent task receives successful
outputs in declared order and starts only after all of them finish:

```sh
. "$HOME/.local/share/tny/tny-workflows.sh"
tny_workflow_begin
trap 'tny_workflow_cleanup' EXIT

tny_task review-api --task review --provider codex -- "Review the public API"
tny_task review-tests --task review --provider cursor -- "Find missing tests"
tny_task implement --after review-api --after review-tests -- \
  "Implement the change from both reports and run the tests"
tny_task optimize --task optimizer --after implement -- "Optimize performance and complexity"
tny_task docs --task document --after optimize -- "Document the final behavior"
tny_task retro --task retro --after docs -- "Capture durable lessons from the work"

tny_workflow_run --jobs 2
tny_result implement
```

The Python and TypeScript SDKs expose the same validated DAG, branch-isolated
failure semantics, dependency context, and concurrency bound through
`Workflow`. See [docs/workflows.md](docs/workflows.md) for the full shell API,
SDK examples, cancellation, result handling, and security limits.

## Build & test

```sh
make            # release build → build/tny (stripped, size printed)
make test       # unit tests (greatest, ASan/UBSan) + fixture-driven
                # integration tests for every backend — no live keys needed
make bench      # hyperfine startup benchmark
python3 tests/bench/bench_ttft.py --tny build/tny --repo . --bench tui
                # time-to-first-token benches (tui, ask-stdin, ask-spawn,
                # ask-attach) against the scripted codex host
```

Requirements: a C11 compiler and make; python3 for the integration fixtures.
Vendored deps (yyjson, picohttpparser, wslay, greatest) are pinned in
`third_party/*/VERSION` — nothing is downloaded at build time.

CI (`.github/workflows/ci.yml`) builds the stripped binary on Linux
x86_64 and aarch64 (glibc + musl static), Darwin arm64 (Apple Silicon
only — not Intel x86), and Windows x86_64 (MSYS2). See [docs/ci.md](docs/ci.md).

## SDKs

The experimental [Python/cffi and TypeScript/Node-API SDKs](docs/sdks.md)
embed the same native `libtny` loop. They provide typed events, explicit
lifecycle management, permissions, steering, cancellation, sync/async Python
iteration, pull-driven TypeScript async iteration, and validated dependency
workflows with bounded parallel runtimes without duplicating provider logic. Supported native SDK targets are macOS arm64 and Linux glibc
x86_64/aarch64.

## Docs

Public site (Geist Mono, fx.sh-style): [thehumanworks.github.io/tny](https://thehumanworks.github.io/tny/).
The landing terminal is a client-side BYOK preview — pass `OPENAI_API_KEY`
(and optionally `OPENAI_BASE_URL`) in the URL hash; both are encrypted in
the tab ([ADR 0005](docs/adr/0005-client-side-landing-terminal.md)).
tnytty's user-facing pages start at
[docs/tnytty.html](https://thehumanworks.github.io/tny/docs/tnytty.html).

The implementation contract lives in [docs/](docs/README.md): product scope,
architecture, CLI/TUI specs, per-backend protocol notes,
sessions/permissions/skills/MCP behavior, and the size/speed budgets.
tnytty's contract is [`tnytty/docs/`](tnytty/docs/README.md). Update
both the contract and `site/` when behavior changes (`make site`).
