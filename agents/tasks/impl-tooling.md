tny: opus
cwd: /tmp/wt-tny-tooling

# Task: toolchain: mise pins, valgrind/leaks targets, CI and nix (issue #0)

Worktree: /tmp/wt-tny-tooling (branch agent/tooling)

## Ground rules (every agent)
- You work ONLY in the worktree named above (branch agent/<name>, cut from feat/shell-first at 5dd6f27). Never touch /Users/tomas/projects/tny or other worktrees except to READ docs/adr/0057-shell-first-native-loop.md at /Users/tomas/projects/tny/docs/adr/0057-shell-first-native-loop.md (the umbrella decision; cite it as ADR 0057).
- Read AGENTS.md (CLAUDE.md), docs/product.md, docs/architecture.md, docs/cli.md and the ADRs your task names BEFORE writing C. C11 only, no new dependencies, no ncurses, stripped binary < 1.0 MiB.
- Match the existing style exactly (see neighbouring files); run `make format` before finishing. Quality gate: `make quality CLANG_FORMAT="uvx clang-format@21.1.2" CLANG_TIDY="uvx clang-tidy@22.1.8" RUFF="uvx ruff@0.14.0"` (clang tools are not installed natively on this Mac; uvx is). Fix what it reports in files you touched.
- Tests are mandatory: unit tests in tests/test_*.c (greatest) for new C, integration fixtures under tests/integration/ where the change crosses a process or wire boundary, and add your new functions to TARGETS in tests/mutation/mutate.py if the unit suite might cover them only nominally. `make test` must pass (it builds with ASan/UBSan). For new allocation-heavy paths run the relevant test binary under `leaks --atExit -- <binary>` (macOS; valgrind is unsupported on arm64 Macs) and fix reported leaks.
- Documentation is part of the change: update docs/ (cli.md, features/, backends/ as relevant) and WRITE the ADR assigned to you in docs/adr/NNNN-<slug>.md using the existing ADR format (Context / Decision / Consequences, Date, Status). State the wasm behaviour (works / remote-only / clean error) of anything new, per AGENTS.md.
- If you add a make target, test fixture directory, or a tool the suite shells out to, update nix/source.nix and nix/tests.nix in the same change. Do NOT run nix.
- Do not commit secrets. Do not push. When done: `git add -A && git commit` on your branch with a conventional message that names the issue (e.g. "feat(cli): add tny edit (#96)"), then print a summary: files changed, tests added, how you verified, open questions.
- Keep to your scope; other agents are concurrently editing Makefile SRC lists, src/main.c, src/cli/, docs/cli.md, so keep edits to shared files minimal and additive to ease merging.

## Spec (no issue number; ADR 0061)
Goal: this repo must run its quality gates and memory checks anywhere with one `mise install`, and the gates must include a leak check.
Read docs/ci.md, docs/adr/0039-quality-gates.md, Makefile (quality section ~640-720, SANITIZE ~62-100, test-unit), .github/workflows/ci.yml, nix/devshell.nix, nix/tests.nix, docs/nix.md.
Deliver:
1. A project `.mise.toml` pinning the quality toolchain: clang-format 21.1.2 and clang-tidy 22.1.8 (find the mise backend that works on macOS arm64 and Linux; `mise registry | grep clang` shows conda/asdf/vfox options — test `mise install` here), ruff 0.14.0, shellcheck, shfmt, actionlint, python (3.12+), node (for tests/site). Make the Makefile pick them up via PATH with no flags (`make quality` must work after `mise install`); keep the uvx fallback documented. Verify by running `make quality` in the worktree and report the result.
2. Leak checking. valgrind does not exist on arm64 macOS; `leaks` (/usr/bin/leaks) does. Add `make leaks`: runs the unit test binary (and the integration smoke it can) under valgrind on Linux (`valgrind --leak-check=full --error-exitcode=1 --suppressions=tests/valgrind.supp`) and under `leaks --atExit --` on macOS, honest skip message elsewhere. Add `make valgrind` as the Linux-only explicit target and a `make leaks-docker` that runs the valgrind flavour in a Linux container via docker when available (docker is installed here). Provide tests/valgrind.supp for known third-party/ssl noise. Run it here (macOS `leaks`) and report findings; fix trivial leaks you find in first-party code only if the fix is obviously safe, otherwise list them.
3. CI: add a `valgrind` job to .github/workflows/ci.yml on ubuntu (apt valgrind) running `make valgrind`; wire the mise-pinned tools into the quality job if it simplifies it (keep CI green: only change what you can reason about; actionlint must pass).
4. nix: add valgrind (Linux only) and the pinned tools to nix/devshell.nix, and a leak-check derivation or a note in docs/nix.md explaining why it is not in the sandbox if valgrind cannot run there. Update nix/source.nix / nix/tests.nix for any new file the suite needs. Do NOT run nix.
5. Docs: docs/ci.md (mise section, leak checks), AGENTS.md verification bullet (one line: `mise install` then `make quality`, `make leaks`), ADR 0061 "toolchain via mise; leak gate with valgrind and leaks".
Keep `make quality` output clean (format your own additions).
