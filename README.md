# tny

A tiny C11 coding-agent harness: an interactive TUI shell and a
noninteractive-first CLI that drive four kinds of agent backends through one
normalized event loop.

```text
tny                          # interactive shell (lazy backend, ~4 ms first paint)
tny ask --json "fix the failing test"
tny ask --backend cursor "explain this repo"
tny --backend acp --agent gemini -- acp -- ask "hi"
tny acp                      # serve tny's native loop to any ACP client
```

## Backends

| Backend | Transport | Host process |
| --- | --- | --- |
| `openai` (default) | OpenAI-compatible `/v1/chat/completions`, SSE streaming, native tool loop | none — tny owns tools/MCP/skills/permissions |
| `cursor` | [Cursor SDK Bridge](https://cursor.com/docs/sdk/bridge): Connect HTTP/1.1 (`sdk.v1`, JSON codec) | `cursor-sdk-bridge` (spawned, ready-line handshake) |
| `codex` | WebSocket JSON-RPC (`codex app-server`) | `codex` (attach via `--codex-ws` or spawned on an ephemeral port) |
| `acp` | [ACP](https://agentclientprotocol.com/) over stdio JSONL | any ACP agent via `--agent CMD` |

All four normalize onto one event set (text/thinking/tool/permission/plan/
usage/turn-end) rendered by the same TUI and CLI. See
[docs/architecture.md](docs/architecture.md).

## Benchmark vs fx

tny exists to beat [vercel-labs/fx](https://github.com/vercel-labs/fx) on size
and startup while keeping its Unix-shell feature set. Measured against the real
fx v0.0.3 binary, macOS arm64, hyperfine, same machine (2026-08-19):

| Metric | fx 0.0.3 | tny 0.1.0 | Result |
| --- | --- | --- | --- |
| Stripped binary | 6,748,416 B (6.4 MiB) | **392,384 B (0.37 MiB)** | 17.2× smaller |
| `tny --version` | 2.2 ms ± 0.3 | **1.9 ms ± 0.2** | 1.18× faster |
| Max RSS (`--version`) | 3.0 MiB | **2.1 MiB** | 1.4× less memory |
| TUI first prompt | — | 3.3–4.3 ms | budget < 10 ms |

Feature parity notes and deliberate deferrals:
[docs/features/parity-with-fx.md](docs/features/parity-with-fx.md).

## Build & test

```sh
make            # release build → build/tny (stripped, size printed)
make test       # unit tests (greatest, ASan/UBSan) + fixture-driven
                # integration tests for every backend — no live keys needed
make bench      # hyperfine startup benchmark
```

Requirements: a C11 compiler and make; python3 for the integration fixtures.
Vendored deps (yyjson, picohttpparser, wslay, greatest) are pinned in
`third_party/*/VERSION` — nothing is downloaded at build time.

## Docs

The contract lives in [docs/](docs/README.md): product scope, architecture,
CLI/TUI specs, per-backend protocol notes, sessions/permissions/skills/MCP
behavior, and the size/speed budgets. Update the docs when behavior changes.
