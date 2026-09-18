# Agents dashboard verification

Date: 2026-09-18. Baseline: `89bcd5918da0e225e1806813a206d007daafac0a`.
Platform: macOS arm64. Decision: [ADR 0138](../adr/0138-full-screen-agents-dashboard.md)
(renumbered from the original 0136 draft after `main` allocated that number).
This record captures the delivered checks; it is not a pre-implementation plan.

## Requirements and checks

| ID | Invariant | Evidence |
| --- | --- | --- |
| R1 | Interactive CLI, Left handoff, Ctrl-X and `/agents` open a clean dashboard at row one. | Unit display-buffer checks and PTY screen assertions in `test_background_agents.py`. |
| R2 | Clear pending/partial display text, but retain the saved session and draft. | `clear_screen_discards_only_display_text`; real-runner checkpoint, replay and follow-up assertions. |
| R3 | Arming/refusing handoff and dashboard refresh do not clear the screen. | No-erase assertions before handoff and after refusal; bounded wait for a new repaint with unchanged erase count. |
| R4 | Plain/JSON listings remain escape-free; dashboard startup needs no credentials/provider work. | Empty-workspace CLI checks, both normal and `--color=never` PTYs, plus `clear_screen_preserves_non_tty_output`. |
| R5 | Keep the approved C11/private C++20 boundary, document the decision, and meet project gates. | ADR 0138, quality/leak checks, size measurement and focused review below. |

## Results

Commands run from the repository root with `mise exec --` and the pinned tools.

| Check | Result |
| --- | --- |
| New `empty_dashboard()` regression against the baseline Release binary | Expected failure: `OLD-SHELL-TEXT` remains on row one above the dashboard. |
| `make -j6 test` | Passed: all 575 unit tests plus the full integration/contract suite; zero failures. Platform/optional skips are noted below. |
| `python -u tests/integration/test_background_agents.py` | Passed on the final test revision, including observed refreshes and reattachment. |
| `make -j4 quality` | Passed. Darwin explicitly skips GCC `-fanalyzer`; Linux CI owns that check. |
| `make lint-py` after the refresh-test refinement | Passed. |
| `make -j4 leaks` | Passed: zero leaks in the configured macOS suites and CLI smoke checks. Existing process-spawning-suite exclusions remain; Linux Valgrind coverage is unchanged. |
| `python -u tests/mutation/mutate.py --focus agents-dashboard --test clear_screen` | Passed in an isolated source copy: one valid repaint-state mutant, killed by the unit tests; zero survivors. The real working tree was not mutated. |
| `make size-check` and `wc -c build/tny` | Passed: stripped native Release is 1,121,424 bytes, below 6,000,000. |
| `otool -L build/tny` | Existing `/usr/lib/libc++.1.dylib` and `/usr/lib/libSystem.B.dylib`; no new runtime dependency. |
| `node tests/site/test_term.js` | Passed. |
| `uv run --no-project --with jsonschema python tests/integration/test_settings_schema.py` | Passed: all four checks, including optional schema validation. |
| `uv run --no-project --with playwright python tests/integration/test_site_mobile.py` | Passed on an unchanged retry; see the optional-browser observation below. |
| ADR link and registration check | Passed on the original branch as unique 0136. After merging `main`, the decision is ADR 0138 with one index entry and a valid verification-record link. |
| `git diff --check` | Passed. |

Tool versions: clang-format 23.1.0, clang-tidy 22.1.8, Ruff 0.16.6,
Python 3.14.7, and the host C/C++ compiler. No speed claim is made.

The source/test patch relative to the baseline has SHA-256
`72a4ebd74da60b86893e86735fc0843dbcfdbf49425b159e732b3fba57143e38`
(`git diff 89bcd5918da0e225e1806813a206d007daafac0a -- src tests | shasum -a 256`).
This excludes documentation-only evidence updates.

The standard local suite reports platform/runtime skips for Linux-only TLS,
Windows-specific execution and Emscripten/wasm checks. Browser and optional
JSON Schema checks initially skipped because their Python packages were absent.
The schema and mobile-layout checks were then run in ephemeral uv environments.
The first mobile run observed an empty terminal intro and failed; an unchanged
retry passed both phone viewports. No landing-site source was changed by this
fix. The browser-wasm smoke remains unverified locally without a fresh wasm-web
artifact. These are not claims of cross-platform verification; CI lanes are
unchanged.

## Review and test refinements

An independent read-only Codex review found one low-severity test issue: a fixed
650 ms sleep could finish before the 500 ms refresh because the idle loop polls
in 400 ms intervals. The test now waits, with a five-second deadline, for an
observed dashboard repaint before checking that no new clear occurred. The
final focused fixture and Python lint passed after this change. No product-code
correctness issue was reported. Keeping ANSI painting in C11 follows ADR 0114;
this fix introduces no ownership module that benefits from a C++ migration.

An initial test seeded old shell output through a shell wrapper. On macOS that
wrapper made the post-exit terminal-state check fail with `ENOTTY`. The shared
PTY helper now seeds bytes directly before spawning the binary. The final test
verifies cooked terminal restoration without an extra shell process.

The full gate started before the refresh assertion was refined. Product code,
unit tests and the shared PTY helper were unchanged during that gate. The entire
background-agents fixture and Python lint were rerun on the final test revision.
