# Feature parity with fx

fx sources: [README](https://github.com/vercel-labs/fx), [fx.sh/llms.txt](https://fx.sh/llms.txt). The user-visible inventory and measured performance baseline below are the historical v0.0.3 bake-off captured 2026-08-18. The current v0.0.5 extension-hook baseline is tracked separately in [extension-hook-parity.md](extension-hook-parity.md).

## v1 must match — status as built (2026-08-19, all `make test` green)

| Area | fx | tny | Status |
| --- | --- | --- | --- |
| Interactive shell | `fx` | `tny` | ✅ raw-termios ANSI shell, one poll loop, lazy backend, first paint ~4 ms |
| One-shot | `fx ask`, stdin, `--json`, `--image`, `--resume`, `--no-save` | same shape | ✅ plus `--continue-recovery`; exit codes 0/1/2/130 |
| Sessions | `~/.fx/sessions/`, `fx sessions`, `resume last`, compact after 8 turns, `/continue`, recover | `~/.tny/sessions/`, same UX | ✅ compact 8→keep 4, recovery checkpoints, `session recover` |
| Permissions | `ask` / `auto` / `yolo`, rules, session grants | same (native loop; map host approvals) | ✅ rules last-match-wins, workspace>global; host approvals mapped (ACP, codex); cursor bridge is headless — no per-call approvals (documented) |
| Sandbox | `os` (macOS), `none`, `auto` | `none` documented until seatbelt lands | ⚠️ deferred: `doctor` discloses "os sandbox not implemented in this build"; permission engine is the guard |
| Tools | files, grep/glob, `terminal`, web_search/fetch, vision, memory, skill, subagent, MCP lazy select | same names where possible | ✅ 27 tools; `run_command` aliased to `terminal`; `web_search` advertised only with a configured provider (`web_search_command` / `web_search_url`, ADR 0055); `read_image` (`vision` alias) shows png/jpeg/gif/webp |
| Skills | `SKILL.md`, `$`, multi-root discovery | same roots plus `~/.tny/skills/` | ✅ `.agents/.claude/.codex/.cursor/.opencode` roots, `$` picker in TUI |
| MCP | trusted `~/.fx/mcp.json` only, stdio + HTTP + legacy SSE | `~/.tny/mcp.json`, same isolation | ✅ stdio JSONL plus JSON-only Streamable HTTP ([ADR 0051](../adr/0051-mcp-streamable-http.md)); request-scoped SSE, deprecated HTTP+SSE GET, and OAuth deferred; disabled entirely in `tny acp` server mode |
| Subagents | session-backed children, ctrl+x, `subagent` tool | native loop only | ✅ `subagent` tool spawns child `tny ask --json`; children cannot raise perm mode |
| ACP server | `fx acp` | `tny acp` | ✅ initialize fails closed w/o credential, session/load replays history |
| Project instructions | `AGENTS.md` chain, target-scoped | same + `CLAUDE.md` alias | ✅ ~/.tny → ancestors below $HOME → cwd; over `--ssh`, ~/.tny (labeled local) → remote cwd ([ADR 0040](../adr/0040-ssh-agents-md.md)) |
| Extra dirs | `--add-dir`, `/workspace` | same | ✅ persisted per-workspace in settings |
| Models | catalog + `/model` | per-backend catalog | ✅ `models` + `/model` persists choice |
| Doctor / status / usage | yes | yes | ✅ all three, `--json` variants |
| Undo last file tool | `/undo` | native loop | ✅ one-deep undo (blob + metadata) |

## Historical bake-off vs fx v0.0.3 (macOS arm64, same machine, hyperfine)

| Metric | fx 0.0.3 | tny 0.1.0 | Result |
| --- | --- | --- | --- |
| Stripped binary | 6,748,416 B (6.4 MiB) | 392,384 B (0.37 MiB) | **17.2× smaller** (budget < 2.0 MiB) |
| `--version` | 2.2 ms ± 0.3 | 1.9 ms ± 0.2 | **1.18× faster** |
| Max RSS (`--version`) | 3.0 MiB | 2.1 MiB | **1.4× less memory** |
| TUI first paint | — | 3.3–4.3 ms (pty-measured) | target < 10 ms met |

Startup note: linking Security/CoreFoundation eagerly cost ~1.2 ms per launch
and initially lost the race; SecureTransport is now `dlopen`'d at first TLS
use (`src/net/stream.c`).

## tny extras (required)

| Extra | fx | tny |
| --- | --- | --- |
| Cursor SDK Bridge | no | `--backend cursor` |
| Codex app-server WebSocket | no | `--backend codex` |
| ACP client | no (fx *is* an agent) | `--backend acp` |
| OpenAI-compatible BYOK | Gateway-only; wire is **AI SDK LM spec v4**, custom URLs are loopback HTTP | `--backend openai` (`/v1/chat/completions`) |

## Explicit deferrals (not parity failures)

| fx feature | Why deferred |
| --- | --- |
| WASM `createFxAgent` / `createFxTerminal` | Size; not needed to beat the native CLI |
| `fx login` Vercel OAuth / teams / Gateway credits | Not our vendor |
| Completion sounds | Noise, extra assets |
| `fx pr` / `fx issue` | Thin `gh` wrappers later |
| `fx replay` / `FX_RECORD` | Test luxury |
| Auto-upgrade | `tny upgrade` later |
| Automatic permission reviewer model `openai/gpt-5.4` | Native `auto` can use the **active** model or skip review and fail closed in `ask` |

## Naming

Keep slash commands and `ask` flags familiar so users coming from fx do not relearn. Config files use `.tny` prefixes. Do not read `~/.fx/` except as an optional import documented under `tny setup --import-fx` (later).
