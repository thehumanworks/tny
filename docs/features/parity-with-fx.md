# Feature parity with fx

fx sources: [README](https://github.com/vercel-labs/fx), [fx.sh/llms.txt](https://fx.sh/llms.txt). Baseline advertised 2026-08-18.

## v1 must match

| Area | fx | tny |
| --- | --- | --- |
| Interactive shell | `fx` | `tny` |
| One-shot | `fx ask`, stdin, `--json`, `--image`, `--resume`, `--no-save` | same shape |
| Sessions | `~/.fx/sessions/`, `fx sessions`, `resume last`, compact after 8 turns, `/continue`, recover | `~/.tny/sessions/`, same UX |
| Permissions | `ask` / `auto` / `yolo`, rules, session grants | same (native loop; map host approvals) |
| Sandbox | `os` (macOS), `none`, `auto` | same intent; implement macOS seatbelt or document `none` until ready |
| Tools | files, grep/glob, `run_command` + background, web_search/fetch, vision, memory, skill, subagent, MCP lazy select | same names where possible |
| Skills | `SKILL.md`, `$`, multi-root discovery | same roots plus `~/.tny/skills/` |
| MCP | trusted `~/.fx/mcp.json` only, stdio + HTTP + legacy SSE | `~/.tny/mcp.json`, same isolation |
| Subagents | session-backed children, ctrl+x, `subagent` tool | native loop only |
| ACP server | `fx acp` | `tny acp` |
| Project instructions | `AGENTS.md` chain, target-scoped | same + `CLAUDE.md` alias |
| Extra dirs | `--add-dir`, `/workspace` | same |
| Models | catalog + `/model` | per-backend catalog |
| Doctor / status / usage | yes | yes |
| Undo last file tool | `/undo` | native loop |

## tny extras (required)

| Extra | fx | tny |
| --- | --- | --- |
| Cursor SDK Bridge | no | `--backend cursor` |
| Codex app-server WebSocket | no | `--backend codex` |
| ACP client | no (fx *is* an agent) | `--backend acp` |
| OpenAI-compatible BYOK | Gateway only | `--backend openai` |

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
