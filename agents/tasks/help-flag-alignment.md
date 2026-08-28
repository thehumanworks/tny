tny: --provider codex --model gpt-5.6-sol --effort high
cwd: /Users/tomas/projects/tny

## Goal
Add an enforced check that every CLI subcommand's `--help` text mentions every
flag its argv parser accepts, and vice versa (flags in help must be parsed).
Trigger: `tny session <id> --wait` (docs/adr/0041) — help.c and docs/cli.md do
describe it, but nothing would have caught it if they did not.

You MAY edit files in this repo. Do not commit.

## Requirements
1. Write `tests/integration/test_help_flags.py` (Python 3, stdlib only):
   - For each subcommand in `src/cli/cmd_*.c` (and global flags in main/cli
     parsing), extract the accepted flags by grepping `strcmp(argv[i], "--x")`
     / equivalent patterns (inspect the actual parsing style in src/cli and
     src/main.c; handle short aliases like `-B`).
   - Run `$TNY <subcmd> --help` (TNY env var, default build/tny) and assert
     every parsed flag appears in the help text, and every `--flag` token in
     the help text is parsed. Keep an explicit, commented allowlist for
     genuine exceptions (e.g. hidden/debug flags), kept as small as possible.
   - Also assert `tny --help` (top-level) lists every subcommand that exists.
   - Must pass on the current tree. If it reveals real gaps, fix help.c
     (and docs/cli.md) rather than allowlisting.
2. Wire it into the Makefile `test` target (new `test-help-flags` target, add
   it to `test:`), and register it in `nix/source.nix` / `nix/tests.nix`
   per the "Verification" section of AGENTS.md so the nix sandbox has it.
3. Update docs: add a short note in docs/cli.md (Testing/contract section)
   and a short new ADR `docs/adr/00XX-help-flag-alignment.md` (next free
   number; follow the existing ADR format) describing the rule: a parser flag
   without help text, or help text without a parser flag, is a failing test.
   Link it from the ADR index if one exists (docs/adr/README.md).
4. Run `make test` and `make quality CLANG_FORMAT='uvx clang-format@21.1.2' CLANG_TIDY='uvx clang-tidy@22.1.8' RUFF='uvx ruff@0.14.0'`;
   both must pass (Ruff lints the new Python file).

## Deliverable
End with: list of files changed, the allowlist contents with justification,
any help/doc gaps you fixed, and the exact `make test` / `make quality`
result lines.
