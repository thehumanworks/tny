# 0057 — Shell-first native loop: one terminal tool, tny verbs as CLIs

Date: 2026-09-02
Status: accepted (direction); the default tool profile is decided by the
A/B in issue #103 and recorded here when measured

## Context

The native OpenAI-compatible loop advertises 26 function tools
(`src/core/tools.c` `SCHEMA_JSON`). Host backends (Cursor bridge, Codex
app-server, ACP clients) own their tools and are unaffected.

Evidence gathered on 2026-09-02: Vercel's d0 agent went from 15+ tools to 2
(80% → 100% success, 3.5× faster, 37% fewer tokens); mini-SWE-agent is bash
only and scores >74% on SWE-bench Verified; Terminus 2 drives a single tmux
session and placed second on Terminal-Bench; CodeAct reports code actions up
to +20 points with 30% fewer steps than JSON tool calls; Anthropic's
code-execution-with-MCP post drops 150k tokens to 2k by presenting tools as
code; Anthropic's tool search raised Opus 4 from 49% to 74% by removing
schemas from context. Against a pure single tool: Anthropic's SWE-bench
agent, Claude Code, and Codex all keep an exact-match editor beside the
shell, and SWE-agent's 2024 shell-only ablation fell from 12% to 3% on
older models.

Three independent reviewers (codex gpt-5.6-sol, grok-4.6, Claude Opus via
ACP; outputs in `agents/out/shell-first-*.md`) converged on the shape below.

## Decision

1. **Shell-first is a vocabulary, not a single-tool purity test.** The model
   sees `terminal` plus the smallest set the evidence defends; every other
   capability is a `tny` subcommand reachable from any shell, inside tny or
   inside another harness (Claude Code, Codex, Cursor, OpenCode, CI).
2. **Tool profiles, not deletion.** `tools: all | terminal+edit | terminal`
   (setting and `TNY_TOOLS`) changes what the native loop advertises and
   accepts. No implementation is deleted until a three-arm A/B on a frozen
   task set (#103) says so; `tools_fs.c`, `tools_ssh.c`, `tools_web.c`
   remain the backends of the CLI verbs and of `--ssh`.
3. **First-party verbs.** `tny edit` (exact match, stdin payload, exit 2 on
   zero or many matches), `tny mcp call server/tool` (identity stays
   `mcp:server/tool`), `tny ask-user`, `tny image attach`, `tny memory`,
   `tny skill show`, `tny fetch`; subagents remain `tny ask -B` +
   `tny session --wait`. Payload never rides argv; `--json` output carries a
   `kind` field; exit codes are 0 ok, 1 usage/config, 2 semantic failure,
   130 interrupted.
4. **Three harness seams stay in tny:** the session socket for human
   interaction (free-text questions, image attach), the attach path for
   non-text content (ADR 0008), and the OS sandbox for authority.
   `ask-user` and `image attach` are socket-bound; with no socket they print
   one line to stderr and exit 1, never touch the TTY.
5. **In-process intercept.** When tny is the harness, a single simple
   `tny …` command inside `terminal` is dispatched in-process so
   permissions, session grants, undo, the warmed MCP client, and `--ssh`
   routing survive. Outside tny the same verbs run standalone.
6. **Security boundary.** argv0 classification is a UX accelerator, not a
   boundary. Shell profiles are offered for `yolo` (the default, ADR 0001)
   only until the permission tokeniser (#101) and the `os` sandbox (#102)
   land.
7. **Out of scope now:** wasm and libtny keep the full structured schema
   (no miniature shell behind the wasm terminal); `--ssh` is not "a prefix
   on the command" (the remote host has no tny, ADR 0022); a persistent pty
   via tnytty is a later, separately measured ADR; size is not a rationale
   (≈35 KB of a 783 KB binary).

## Consequences

- Issues: #96 edit, #97 mcp call, #98 runner control channel, #99
  intercept, #100 profile, #101 permission tokeniser, #102 sandbox, #103
  A/B; epic #105. Per-decision ADRs: 0058 runner control channel, 0059
  permission tokeniser, 0060 os sandbox, 0061 toolchain (mise, valgrind),
  0062 tool profiles, 0063 in-process intercept, 0064 CLI verb
  conventions.
- The A/B result and the chosen default are appended to this ADR when
  #103 lands.
- Every `tny` verb becomes public API with compatibility obligations to
  other harnesses; `docs/cli.md` is the contract.
