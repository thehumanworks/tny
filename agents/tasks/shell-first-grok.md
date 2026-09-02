tny: --provider grok --effort xhigh

# Brief: shell-first native loop for tny

You are one of three "collaborator reasoners" (codex/gpt-5.6-sol, grok-4.6,
claude opus) reviewing the same proposal in parallel. Your job is NOT to
adversarially destroy it. Constructively identify weaknesses, suggest
improvements, and say what you would accept, reject, or reshape. Be concrete
and cite files in this repo (path:line) when you make a claim about tny.
Read-only: do not edit files, do not run make or tests.

## The proposal

tny's native OpenAI-compatible loop (src/core/tools.c SCHEMA_JSON) advertises
26 built-in function tools: list/glob/grep/read/write/edit/delete/rename/copy
files, create_folder, file_info, semantic_search, open_file, terminal,
web_fetch, web_search, memory, read_tool_result, skill, install_skill,
subagent, mcp_search_tools, mcp_select_tool, mcp_features, ask_user_question,
read_image. Host backends (cursor bridge, codex app-server, ACP clients) own
their own tools and are out of scope.

Proposal: the native loop advertises ONE tool, `terminal`. Everything else
becomes a CLI the model reaches through the shell, primarily `tny`
subcommands, so the same CLIs work from any harness that has a shell
(claude code, codex, cursor, opencode...), not just tny:

- `tny edit FILE` — exact-match str_replace, old/new on stdin, nonzero exit on
  0 or >1 matches (sed fails silently; every top harness kept this semantic).
- `tny image attach PATH` — writes a marker over the session socket
  (<session>/sock, docs/adr/0053) so the harness attaches the pixels as an
  image message on the next request (docs/adr/0008). Pixels cannot travel
  through stdout text.
- `tny ask-user "question"` — blocks on the session socket until the
  frontend (TUI / ACP client) answers; cross-platform because the transport
  is tny's own. Falls back to a clean error when no frontend is attached.
- `tny mcp call server/tool '{json}'` — keeps the permission identity
  mcp:server/tool; the MCP catalog stays as text in the system prompt
  (docs/adr/0049 already does the catalog-not-schemas pattern).
- `tny skill show NAME`, `tny ask -B` / `tny session --wait` (subagents),
  `tny memory get|set|list`, `tny fetch URL` or plain curl.
- Large outputs: bounded preview + the result stored as a FILE whose path is
  printed, so the model reads more with sed -n instead of read_tool_result.
- `--ssh` (docs/adr/0022, src/core/tools_ssh.c ~620 lines) collapses to a
  prefix on the one command.
- The terminal tool may get richer (persistent pty session via tnytty,
  tnytty/docs/README.md, so vim/REPLs/ssh prompts work), but the tool COUNT
  stays one.
- Rollout: a setting/flag (e.g. `tools: terminal`, TNY_TOOLS=terminal) that
  advertises only terminal, A/B measured against the full set on a fixed
  task set (pass rate, steps, tokens; tests/bench/bench_ttft.py exists for
  latency), then delete tools_fs/tools_ssh/tools_web with numbers in an ADR.

Evidence already gathered: Vercel d0 (15+ tools -> 2, 80%->100% success,
3.5x faster, 37% fewer tokens); mini-SWE-agent (bash only, >74% SWE-bench
Verified); Terminus 2 (single tmux tool, #2 on Terminal-Bench); CodeAct
(code actions up to +20 pts, 30% fewer steps); Anthropic code-execution-with-
MCP (150k -> 2k tokens); Anthropic tool search (Opus 4 49% -> 74% by removing
schemas from context); Anthropic SWE-bench agent = bash + str_replace editor
(highest reliability with exact-match replace); SWE-agent 2024 shell-only
ablation 12% -> 3% (old models). Claude Code (Terminal-Bench #1) keeps
Read/Edit/Bash; Codex keeps shell + apply_patch.

## Known constraints and open problems (engage with these)

1. Permissions (src/core/perm.c, docs/features/permissions.md): the engine
   keys on tool identity; reads in the workspace are free, writes prompt,
   MCP calls are mcp:server/tool. One tool reduces this to argv0 prefix
   matching, the model behind nine Claude Code permission-bypass CVEs
   (command chaining, option injection). Default mode is yolo (ADR 0001) so
   most users never hit the engine; ask/auto users do. The docs define an
   `os` sandbox mode (seatbelt/bubblewrap) that currently resolves to none.
2. wasm (docs/adr/0017): fs/skills/permissions tools run unmodified in the
   browser; terminal returns a clean error. Shell-only makes the browser
   native loop host-only unless a tiny built-in command set sits behind the
   wasm terminal.
3. Model priors: current models are RL-trained on Read/Edit/Bash tool
   shapes; `tny edit` etc. must be taught in the system prompt.
4. Size (invariant: stripped < 1.0 MiB): the fs/web/ssh tool layer is ~35 KB
   of a 783 KB release binary; the tool schema is ~1-2k tokens per request.
   Size is not the argument; trajectory quality and deleted code are.
5. Cross-harness compatibility is a stated goal: the `tny` subcommands must
   be usable by other harnesses' shells (stateless where possible, or with
   an explicit session/socket env var).
6. `tny acp` server mode serves the native loop to ACP clients (Zed etc.),
   which have their own permission/question UI.

## Deliverable (max ~900 words, markdown)

1. ACCEPT — parts of the proposal you endorse, one line of reasoning each.
2. RESHAPE — parts you would keep but change; say exactly how.
3. REJECT / LOW VALUE — parts not worth doing, with the reason.
4. WEAKNESSES you found that the brief missed, each with a concrete
   mitigation (cite repo paths).
5. ISSUE BREAKDOWN — 3 to 6 GitHub issues that could be worked in parallel:
   title, scope (files), acceptance criteria, dependencies between them.
6. Your one-paragraph statement of the USP of a shell-first tny versus
   claude code / codex / cursor / opencode, and of how an agent would work
   with the harness turn by turn.

Then a dedicated deep-dive on your assigned lens (see below).

## LENS: agent ergonomics and cross-harness compatibility. How do current models actually behave with a single shell tool over long turns (error recovery, quoting, cwd drift, output bounding)? What system-prompt text and CLI ergonomics (exit codes, stdin conventions, --json) make tny subcommands usable from claude code, codex, cursor, opencode without tny running as the harness? What must be stateless vs socket-bound?
