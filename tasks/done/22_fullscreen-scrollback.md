# 22 — Fullscreen transcript scrollback

Status: done

## Atomic checklist

- [x] Retain configurable logical transcript lines (default 50,000).
- [x] Scroll wrapped rows with wheel, Shift arrows, pages and Ctrl-Home/End.
- [x] Keep browsed output anchored while streaming; follow at the bottom.
- [x] Preserve composer/history input and inline/plain behavior.
- [x] Restore fullscreen mouse reporting and terminal modes on exit.
- [x] Persist validated `ui.scrollback_lines` through CLI and TUI settings.
- [x] Document TUI controls, settings, CLI usage and ADR 0183.
- [x] Run focused renderer/settings and PTY integration checks.
- [x] Complete repository verification and record observed results.

## Verification

Input: branch `fix/fullscreen-scrollback`, based on `origin/main` `d0ef2f7`.
Remote `main` was rechecked before completion and still pointed to that commit.

- `make -j4 build/tny build/tny-test` passed. Stripped binary: 26,937,712 bytes;
  runtime dependencies: libm, libstdc++, libgcc_s and libc.
- `build/tny-test -s tui_suite` passed: 65 tests, 2,685 assertions, including
  a repeat after the final style-only correction.
- `build/tny-test -s core_suite -t ui_settings_defaults_and_generated_roundtrips`
  passed: 1 test, 1,838 assertions.
- `python3 tests/integration/test_settings_schema.py` passed: 8 tests.
- `python3 tests/integration/test_tui.py` passed the full PTY suite.
- Full unit suite passed: 631 tests, 0 failures, 0 skips, 51,705 assertions,
  with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, using
  `python3 /tmp/fullscreen-subreaper.py /workspace/tny/build/tny-test`.
  The external wrapper uses Linux `PR_SET_CHILD_SUBREAPER` and reaps adopted
  descendants; it neither changes nor skips test cases.
- The raw full suite encountered two process-reaping failures; both reproduced
  on unchanged baseline `d0ef2f7`. The external subreaper resolved this container
  condition without project edits.
- Formatting, strict compiler warnings, Ruff, ShellCheck, shfmt, zsh syntax,
  actionlint, ast-grep, and JavaScript syntax checks passed.
- The full clang-tidy pass found one comparison-style diagnostic in
  `tui_draw.c`; it was corrected and `make tidy TIDY_SRC=src/tui/tui_draw.c`
  passed. Unchanged files retained their successful full-pass evidence.
- The aggregate `make quality` gate is blocked by GCC 14's
  `-Wanalyzer-fd-leak` at unchanged `src/core/jobs.cpp:6206`. Its scoped
  descriptor closes the adopted fd in its destructor; this change does not
  modify that code. C++ analyzer control tests passed.
- The full C analyzer pass stopped at existing stdout/stderr `dup2` capture
  and restoration in `src/tui/tui_commands.c:run_cli`, also reported as
  `-Wanalyzer-fd-leak`. That function is unchanged by this feature.
- Focused GCC analysis passed for the six changed implementation files
  outside that blocked translation unit: `tui.c`, `tui_draw.c`, `tui_input.c`,
  `config.c`, `cmd_settings.c`, and `help.c`. Full quality is not claimed green.
- `git diff --check` passed.

No live provider inference. All provider integration tests use local mocks.
