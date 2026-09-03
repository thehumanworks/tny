tny: --provider codex --model gpt-5.6-sol --effort high
cwd: /tmp/wt-tny-edit

# Task: stateless tny edit CLI (issue #96)

Worktree: /tmp/wt-tny-edit (branch agent/edit)

## Ground rules (every agent)
- You work ONLY in the worktree named above (branch agent/<name>, cut from feat/shell-first at 5dd6f27). Never touch /Users/tomas/projects/tny or other worktrees except to READ docs/adr/0057-shell-first-native-loop.md at /Users/tomas/projects/tny/docs/adr/0057-shell-first-native-loop.md (the umbrella decision; cite it as ADR 0057).
- Read AGENTS.md (CLAUDE.md), docs/product.md, docs/architecture.md, docs/cli.md and the ADRs your task names BEFORE writing C. C11 only, no new dependencies, no ncurses, stripped binary < 1.0 MiB.
- Match the existing style exactly (see neighbouring files); run `make format` before finishing. Quality gate: `make quality CLANG_FORMAT="uvx clang-format@21.1.2" CLANG_TIDY="uvx clang-tidy@22.1.8" RUFF="uvx ruff@0.14.0"` (clang tools are not installed natively on this Mac; uvx is). Fix what it reports in files you touched.
- Tests are mandatory: unit tests in tests/test_*.c (greatest) for new C, integration fixtures under tests/integration/ where the change crosses a process or wire boundary, and add your new functions to TARGETS in tests/mutation/mutate.py if the unit suite might cover them only nominally. `make test` must pass (it builds with ASan/UBSan). For new allocation-heavy paths run the relevant test binary under `leaks --atExit -- <binary>` (macOS; valgrind is unsupported on arm64 Macs) and fix reported leaks.
- Documentation is part of the change: update docs/ (cli.md, features/, backends/ as relevant) and WRITE the ADR assigned to you in docs/adr/NNNN-<slug>.md using the existing ADR format (Context / Decision / Consequences, Date, Status). State the wasm behaviour (works / remote-only / clean error) of anything new, per AGENTS.md.
- If you add a make target, test fixture directory, or a tool the suite shells out to, update nix/source.nix and nix/tests.nix in the same change. Do NOT run nix.
- Do not commit secrets. Do not push. When done: `git add -A && git commit` on your branch with a conventional message that names the issue (e.g. "feat(cli): add tny edit (#96)"), then print a summary: files changed, tests added, how you verified, open questions.
- Keep to your scope; other agents are concurrently editing Makefile SRC lists, src/main.c, src/cli/, docs/cli.md, so keep edits to shared files minimal and additive to ease merging.

## Spec (from issue #96 and ADR 0057)
Add `tny edit FILE`: exact-match string replacement, exactly one match or fail loudly.
- Factor the implementation out of `t_edit_file` in src/core/tools_fs.c (:429-464: exact match, ambiguity rejection, atomic temp+rename, replace_all) into a shared function both the tool and the CLI call. No behaviour change for the tool.
- Payload NEVER rides argv. stdin accepts (a) a fence: lines `*** SEARCH`, `*** REPLACE`, `*** END` (caller may choose another marker prefix with `--marker STR`), or (b) `--json` with `{"old":"...","new":"...","replace_all":false}` on stdin.
- Output: human text by default; `--json` prints one object `{"kind":"edit","path":...,"matches":N,"replaced":N}` on stdout. Progress/errors on stderr.
- Exit codes: 0 exactly one match (or replace_all with >=1); 2 zero or multiple matches (stderr prints the count and, for zero, the nearest unique context line so the caller can widen `old` without guessing); 1 usage/IO; 130 interrupted. No partial write on any failure.
- Works with zero TNY_* environment, any cwd, relative or absolute path; `--help` stays on the config-free fast path (src/main.c parses --help/--version before loading config; keep it that way).
- `--ssh` is out of scope for the CLI (the intercept issue #99 handles remote); do not implement it.
- Tests: tests/test_*.c cases for one match, zero, multiple, replace_all, multi-line old, CRLF file, missing file, symlink target, relative path, no partial write on failure; an integration test under tests/integration/ that runs the built binary with both stdin forms and checks exit codes and --json. Mutation TARGETS entry.
- Docs: docs/cli.md section with copy-paste examples (fence and --json), exit code table. ADR 0064 "CLI verb conventions": stdin payload rule, fence format, `--json` `kind` objects, exit code table (0/1/2/130), stderr for progress — other verbs (#97, #98) will follow it.
- wasm: the CLI verb behaves like the tool (works on the virtual FS); say so in docs.
