# Workspace dashboard navigation

Date: 2026-09-22. Baseline: `50ef80d`.

## Acceptance

- The current cwd's workspace precedes other saved workspaces even when its
  sessions are older. Other paths are alphabetical; each section is newest first.
- Directory headings appear above indented sessions, and only sessions select.
- Typing restricts workspace paths using case-insensitive fuzzy subsequences.
  Backspace, clearing, empty results, Unicode and pasted input are covered.
- Refresh retains the selected physical bucket and session ID; Enter opens the
  visible selection, including duplicate IDs in different workspaces.
- Scrolling and resizing keep the selected row and its directory heading visible.
- Listing and viewing do not start a provider or write saved session state.
- Existing global discovery, continuation and native background ownership remain
  covered by their existing fixtures.

## Evidence

The new section/navigation PTY regression fails against the installed baseline
`50ef80d`: the expected directory heading is absent. It uses isolated saved
fixtures and requires no inference.

Commands run through `mise exec -- env -u TNY_TOOLS -u TNY_SELF_IMPROVE`.
Integration tests use the absolute `build-background-verified/tny` path, temporary
state directories and synthetic credentials; provider fixtures are loopback-only.

| Check | Result |
| --- | --- |
| Native release/debug build | Passed |
| `make BUILD=build-background-verified test-unit test-help-flags` | Passed: 550 unit tests, one Linux-only skip, 30,126 assertions; help contract passed |
| TUI unit suite | Passed: 51 tests, 315 assertions, including all review regressions |
| Final background/agents PTY suite | Passed: 30 scenarios, including current-workspace legacy filtering |
| Final general TUI PTY suite | Passed: 16 scenarios |
| `make BUILD=build-background-quality quality` | Passed; GCC analyzer explicitly skipped on Darwin |
| `make BUILD=build-background-verified leaks` | Passed; zero leaks in supported macOS suites and CLI checks |

Additional integration checks before the three review fixes passed: worktrees
(33 tests), native search (6 scenarios, including handoff), and the separate
`agents --run` task dashboard (3 tests). Final unit and dashboard/TUI checks cover
the changed input, grouping and rendering paths after those fixes. Unchanged
native runner/backend coverage is recorded in [global background verification](global-background.md);
this UI follow-up does not claim a new full backend suite run.

One final PTY attempt initially expected the draft header label `filter:` instead
of the implemented `path:`. The observed screen showed the expected filtered
legacy row; the assertion was corrected and the entire suite rerun.

## Independent review and improvements

Sol xhigh implemented the change. Luna max explored the UI boundary and performed
one independent review pass, followed by confirmation of its three fixes:

1. Legacy sessions whose physical bucket is the current workspace now use its
   known path for grouping and filtering. Unit and real PTY tests cover this.
2. A capped filter reserves space for a complete Unicode character, including
   split paste chunks. Unit tests cover the 4,096-byte boundary.
3. When only one display row remains, the selected session is shown instead of
   spending that row on a heading. The unit viewport test covers this fallback.

The reviewer confirmed selection still preserves the physical workspace bucket
and session ID. Filtering and refresh cannot retarget Enter to a hidden row.

## Artifact and scope

Stripped Darwin arm64 binary: **1,286,432 bytes**. Runtime dependencies:
`libc++.1.dylib` and `libSystem.B.dylib`. No performance claim is made.

Source/test patch SHA-256 relative to baseline `50ef80d`:
`d016916a9c541077f3f8607fd7a1b2f5bbdcb82420891ce5f711852fd7d62e96`.
Documentation-only result recording follows validation. The installation rebuild
may change version metadata after commit.

No live inference or hosted/Linux/Windows/browser-wasm verification is claimed
by these local macOS results. Darwin quality explicitly leaves GCC analysis to
Linux CI; leak coverage retains the gate's documented macOS exclusions.
