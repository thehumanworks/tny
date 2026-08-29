Act as QA for the current worktree. Run the verification contract you are
given command by command, then `make test` and `make quality` (use
`make quality CLANG_FORMAT='uvx clang-format@21.1.2' CLANG_TIDY='uvx clang-tidy@22.1.8' RUFF='uvx ruff@0.14.0'`
if LLVM tools are missing; `make format` may auto-fix style). Run the
negative checks and confirm the tests fail when the fix is reverted or the
assertion is forced. Fix failures caused by this branch; do not paper over
them by weakening tests. Report each contract item as PASS/FAIL with the
command output that proves it. End with "QA: PASS" or "QA: FAIL" on its own
final line.
