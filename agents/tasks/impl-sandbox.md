tny: --provider codex --model gpt-5.6-sol --effort high
cwd: /tmp/wt-tny-sandbox

# Task: implement the documented os sandbox mode (issue #102)

Worktree: /tmp/wt-tny-sandbox (branch agent/sandbox)

## Ground rules (every agent)
- You work ONLY in the worktree named above (branch agent/<name>, cut from feat/shell-first at 5dd6f27). Never touch /Users/tomas/projects/tny or other worktrees except to READ docs/adr/0057-shell-first-native-loop.md at /Users/tomas/projects/tny/docs/adr/0057-shell-first-native-loop.md (the umbrella decision; cite it as ADR 0057).
- Read AGENTS.md (CLAUDE.md), docs/product.md, docs/architecture.md, docs/cli.md and the ADRs your task names BEFORE writing C. C11 only, no new dependencies, no ncurses, stripped binary < 1.0 MiB.
- Match the existing style exactly (see neighbouring files); run `make format` before finishing. Quality gate: `make quality CLANG_FORMAT="uvx clang-format@21.1.2" CLANG_TIDY="uvx clang-tidy@22.1.8" RUFF="uvx ruff@0.14.0"` (clang tools are not installed natively on this Mac; uvx is). Fix what it reports in files you touched.
- Tests are mandatory: unit tests in tests/test_*.c (greatest) for new C, integration fixtures under tests/integration/ where the change crosses a process or wire boundary, and add your new functions to TARGETS in tests/mutation/mutate.py if the unit suite might cover them only nominally. `make test` must pass (it builds with ASan/UBSan). For new allocation-heavy paths run the relevant test binary under `leaks --atExit -- <binary>` (macOS; valgrind is unsupported on arm64 Macs) and fix reported leaks.
- Documentation is part of the change: update docs/ (cli.md, features/, backends/ as relevant) and WRITE the ADR assigned to you in docs/adr/NNNN-<slug>.md using the existing ADR format (Context / Decision / Consequences, Date, Status). State the wasm behaviour (works / remote-only / clean error) of anything new, per AGENTS.md.
- If you add a make target, test fixture directory, or a tool the suite shells out to, update nix/source.nix and nix/tests.nix in the same change. Do NOT run nix.
- Do not commit secrets. Do not push. When done: `git add -A && git commit` on your branch with a conventional message that names the issue (e.g. "feat(cli): add tny edit (#96)"), then print a summary: files changed, tests added, how you verified, open questions.
- Keep to your scope; other agents are concurrently editing Makefile SRC lists, src/main.c, src/cli/, docs/cli.md, so keep edits to shared files minimal and additive to ease merging.

## Spec (from issue #102 and ADR 0057)
Make the documented `os` sandbox mode real for the `terminal` child. Read docs/features/permissions.md (Sandbox section), docs/adr/0001-run-all-agents-in-yolo-mode.md, src/core/tools_shell.c, src/core/config.c (sandbox setting), src/cli/ doctor.
- New src/core/sandbox.c/.h: build the child command wrapper. macOS: Seatbelt via `sandbox-exec -p PROFILE` (or sandbox_init in the forked child before execl; pick the one that works on macOS 27 arm64 and document why). Linux: bubblewrap (`bwrap`) with ro-bind /, rw-bind workspace + extra dirs + $TMPDIR, `--unshare-pid`, keep network (docs say outbound net allowed; localhost listen extra). Writes limited to workspace, extra dirs, temp, required devices. Only the terminal child is wrapped; tny itself stays outside.
- `auto` resolves to `os` when the wrapper is available (sandbox-exec present / bwrap on PATH), else `none`; `yolo` forces `none` for the process (unchanged). `tny doctor` reports the effective mode honestly and never claims `os` on an unsupported host.
- Sandbox-widening approval stays a separate prompt from command approval (docs already say so); implement the minimal hook: a denied write outside the workspace surfaces as a tool error naming the path and the setting to widen (extra dirs), no automatic prompt loop.
- `--ssh`: remote commands are out of scope (the remote host is the boundary); background `terminal` commands (`background:true`) get the same wrapper.
- Tests: unit tests for profile/argv construction (no exec) in tests/test_*.c; an integration test that runs `terminal` under `os` writing inside (succeeds) and outside (fails) the workspace, skipped cleanly when the wrapper is unavailable; make sure tests/integration still pass under `yolo` default. Mutation TARGETS. Run the new path under `leaks --atExit`.
- Docs: docs/features/permissions.md (mode table, what is and is not confined, network stays open by default and why), doctor output, ADR 0060 "os sandbox: seatbelt and bubblewrap for the terminal child" (record the network-open default and reference Anthropic's Claude Code sandboxing result of 84% fewer prompts as the motivation). wasm: clean error / not applicable — state it.
- nix: add bubblewrap to the Linux test inputs in nix/tests.nix if the integration test uses it; do not run nix.
