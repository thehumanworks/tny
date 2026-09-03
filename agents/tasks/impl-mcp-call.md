tny: opus
cwd: /tmp/wt-tny-mcp-call

# Task: tny mcp call CLI (issue #97)

Worktree: /tmp/wt-tny-mcp-call (branch agent/mcp-call)

## Ground rules (every agent)
- You work ONLY in the worktree named above (branch agent/<name>, cut from feat/shell-first at 5dd6f27). Never touch /Users/tomas/projects/tny or other worktrees except to READ docs/adr/0057-shell-first-native-loop.md at /Users/tomas/projects/tny/docs/adr/0057-shell-first-native-loop.md (the umbrella decision; cite it as ADR 0057).
- Read AGENTS.md (CLAUDE.md), docs/product.md, docs/architecture.md, docs/cli.md and the ADRs your task names BEFORE writing C. C11 only, no new dependencies, no ncurses, stripped binary < 1.0 MiB.
- Match the existing style exactly (see neighbouring files); run `make format` before finishing. Quality gate: `make quality CLANG_FORMAT="uvx clang-format@21.1.2" CLANG_TIDY="uvx clang-tidy@22.1.8" RUFF="uvx ruff@0.14.0"` (clang tools are not installed natively on this Mac; uvx is). Fix what it reports in files you touched.
- Tests are mandatory: unit tests in tests/test_*.c (greatest) for new C, integration fixtures under tests/integration/ where the change crosses a process or wire boundary, and add your new functions to TARGETS in tests/mutation/mutate.py if the unit suite might cover them only nominally. `make test` must pass (it builds with ASan/UBSan). For new allocation-heavy paths run the relevant test binary under `leaks --atExit -- <binary>` (macOS; valgrind is unsupported on arm64 Macs) and fix reported leaks.
- Documentation is part of the change: update docs/ (cli.md, features/, backends/ as relevant) and WRITE the ADR assigned to you in docs/adr/NNNN-<slug>.md using the existing ADR format (Context / Decision / Consequences, Date, Status). State the wasm behaviour (works / remote-only / clean error) of anything new, per AGENTS.md.
- If you add a make target, test fixture directory, or a tool the suite shells out to, update nix/source.nix and nix/tests.nix in the same change. Do NOT run nix.
- Do not commit secrets. Do not push. When done: `git add -A && git commit` on your branch with a conventional message that names the issue (e.g. "feat(cli): add tny edit (#96)"), then print a summary: files changed, tests added, how you verified, open questions.
- Keep to your scope; other agents are concurrently editing Makefile SRC lists, src/main.c, src/cli/, docs/cli.md, so keep edits to shared files minimal and additive to ease merging.

## Spec (from issue #97 and ADR 0057)
Add `tny mcp call SERVER/TOOL` next to `tny mcp list` (src/cli/cmd_misc.c ~672) so the model, and any other harness with a shell, reaches MCP tools while the permission identity stays `mcp:server/tool`. Read docs/features/mcp-and-skills.md, docs/adr/0049-mcp-background-warmup.md, docs/adr/0052-mcp-import-from-harnesses.md, src/mcp/mcp.c, src/core/tools.c (mcp_select_tool path ~415-429), tests/test_mcp.c.
- JSON arguments on STDIN (argv JSON is a quoting footgun); `--json` result on stdout as one object with `kind:"mcp_call"`, server, tool, result content; errors on stderr; exit 0/1/2/130 (2 = the tool returned isError or the server rejected the call).
- Permission check with identity `mcp:server/tool` immediately before tools/call, using the same engine the tool uses (perm_check); in the default yolo mode it passes; in ask mode with no TTY it fails closed with exit 2 and a clear message.
- Cross-harness: may spawn servers from the user-global ~/.tny/mcp.json (cold start is fine; document it). Project .mcp.json only via the existing mcp.import_from opt-in. Never read a project file on its own. Server output is untrusted data and bounded like a tool result: large results spill to a 0600 file whose path is printed (see ADR 0057 result-file rule), preview capped.
- wasm: stdio spawn stays the existing clean error; HTTP servers remote-only (say so in docs).
- Tests: tests/test_mcp.c fake stdio server coverage for the CLI path (success, isError, unknown server, unknown tool, malformed stdin JSON) and an integration test invoking the binary. Mutation TARGETS. `leaks --atExit` on the new path.
- Docs: docs/cli.md, docs/features/mcp-and-skills.md; ADR 0064 (CLI verb conventions) is being written by the edit task — do not write it; reference it and follow: stdin payload, `--json` kind objects, exit codes 0/1/2/130, stderr for progress.
