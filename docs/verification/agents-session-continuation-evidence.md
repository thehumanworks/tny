# Agents session inspection and continuation — evidence

Date: 2026-09-19
Contract: [A1–A8](agents-session-continuation.md)
Decision: [ADR 0154](../adr/0154-agents-session-inspection-and-continuation.md)
Baseline: `f90a6103b3b9d3e982bc88a8edd12fd0b69ddaf3`
Status: independent implementation review accepted; parent native gates passed
with the documented toolchain/environment settings and platform limits.

## Delivered behavior

- Enter retains successful live-owner attachment. If the owner is unavailable,
  it shows the saved transcript with a read-only label and continuation guidance.
- Completed sessions remain conversations: a prompt or `/continue` requests
  ownership of the selected ID. No healthy owner is stolen.
- Viewing does not resolve provider credentials, refresh OAuth, start a runner,
  activate a checkpoint, or persist a session/settings/auth replica. Missing or
  removed provider configuration does not hide saved text.
- New execution acquires the writer and reloads current history and selectors
  under that lock. It uses the selected workspace, provider and model.
- Saved checkpoints require explicit `/continue`. Rejected prompts are neither
  submitted nor queued. Existing recovery validation and consumption remain.
- Replica mutations and inherited provider-setup input are guarded. Background
  exit detaches; ordinary foreground cancellation is not redefined.

New execution uses current settings/launch flags for historical options not
stored in ordinary session metadata. Exact restoration of legacy permissions,
effort, credentials and other absent metadata is not claimed. A live runner
retains its existing effective configuration.

## Independent assignments and review

| Role | Agent | Result |
| --- | --- | --- |
| Read-only investigator | `236723587a413e94` | Identified the `done`/writer-lock distinction, attachment-only dead end, eager credential resolution and checkpoint activation. |
| Implementer | `494398b31237d74f` | Implemented code, docs and deterministic regression tests; did not commit or own the verification contract. |
| Independent read-only reviewer | `431c78755a755fca` | Initially requested changes for a provider-wizard guard bypass; accepted the revised implementation after independent re-review and tests. |
| Parent | Lead session | Owns the contract and acceptance; reran focused checks and full project gates rather than accepting worker test claims. |

The reviewer reproduced an unfinished `/provider setup` wizard surviving Ctrl-X
into the dashboard and then writing settings from a held-lock saved view.
Dashboard entry now cancels that wizard. Submission defensively discards stale
wizard state for either replica flag before command routing. The reviewer
confirmed the reproduction no longer mutates settings and that ordinary
foreground setup still works. New unit and PTY cases cover both paths.

The reviewer additionally exercised killed-idle-runner retry before it was made
an automated regression. No review-blocking findings remained after re-review.
The reviewer did not rebuild during the parent gates or modify the implementation.

Reviewed code/test diff SHA-256, unchanged during parent verification:

```text
a7bde1588a35d6047bb37ebb762ae18f17be9b7e1cf5fd2e924c35e237bfe4e3
```

This is `git diff` over `src/tui`, `src/cli/help.c`, the three changed integration
files and `tests/test_tui.c`, against the baseline above. A pre-existing long-line
formatting error in `tests/fixtures/checkpoint_ownership.c` was also corrected
when the first parent quality run found it. That change only wraps one call.
The parent confirmed the same formatter error against the baseline source.

## Contract-to-test mapping

| ID | Parent regression evidence | Coverage limit |
| --- | --- | --- |
| A1 | `completed_session_continuation`: real loopback-backed completion, observed writer release, fresh dashboard, same ID/history, exactly one additional request/turn. | No real account inference. |
| A2 | `locked_saved_inspection_retry` and competing-owner paths in `run_case`: saved output remains visible under a held lock or refused owner handshake. | Reads saved text, not a new live observer stream. |
| A3 | `provider_free_inspection`: missing profile, removed provider, expired synthetic OAuth, no request/refresh, byte snapshots of persistent state; checkpoint inspection makes no activation. | Passive saved inspection only; successful existing live attachment continues consuming its stream. |
| A4 | Held-lock refusal/retry preserves listener/writer identity; fresh-under-lock history/provider/model assertions; two killed-idle-runner cases cover observed EOF and queued input with a stale client. Runner suite and ownership/fault oracles also pass. | No new fault injection for all context allocation/spawn failures. |
| A5 | Mutation command/exit snapshots in `locked_saved_inspection_retry`; `dashboard_cancels_provider_wizard`; `saved_view_discards_stale_provider_wizard`; retry succeeds after lock release. | Background replica commands intentionally use a small allowlist. |
| A6 | Existing live permission/multitool/steering/restart cases; fresh provider/model assertions; `test_agents_attach_in_selected_worktree` executes a tool in the selected checkout and verifies its instructions. | No claim to reconstruct absent legacy configuration. |
| A7 | Inspection and late-checkpoint refusal tests; explicit disk recovery with and without permission; consumed/invalid/configuration-mismatched checkpoints refused. | Exactly-once is scoped to controlled handoff/recovery, not arbitrary external-effect crashes. |
| A8 | Background exit, interruption, isolation and ordinary TUI suites; explicit in-process refusal. | macOS and wasm execution not run on this Linux host. |

## Parent commands and results

All runtime checks used temporary HOME/XDG paths, `env -i`, synthetic credentials
and local mock providers. No user sessions or credentials were used. Tool PATH
entries were preserved explicitly. The absolute release binary was passed as
`TNY`. The main build used GCC/G++ 16.2.1; the debug binary enabled ASan/UBSan.

| Command | Result |
| --- | --- |
| `make release debug` | Exit 0. |
| `./build/tny-test -s runner_suite` | Exit 0; 19/19 tests, 3,139 assertions. |
| `./build/tny-test -s session_bg_suite` | Exit 0; 20/20 tests, 195 assertions. |
| `./build/tny-test -s tasks_suite` | Exit 0; 10/10 tests, 656 assertions. |
| `./build/tny-test -s tui_suite` | Exit 0; 48/48 tests, 289 assertions. |
| `make test-runner-ownership` | Exit 0; ownership and fault oracles passed. |
| `make test` | Exit 0 in the final non-Git, disk-backed environment: 497/497 sanitizer unit tests (29,083 assertions) and all 76 integration entrypoints. Expected optional/platform skips remain; 25 background-agent cases and all 27 worktree tests passed. |
| `make -j2 quality ANALYZER_CC=<GCC14>/gcc ANALYZER_CXX=<GCC14>/g++` | Exit 0. Full format, Clang C/C++ analysis, strict warnings, Python/shell/workflow/JS lint and GCC analysis passed using CI's analyzer major version. |
| `make leaks` with cached Nix GCC/G++/Valgrind and OpenSSL RUNPATH | Exit 0; 497/497 unit tests, 29,101 assertions, unit plus all four CLI smoke checks. Zero Memcheck errors and zero definitely/indirectly lost bytes. |
| Contract-negative controls in an isolated source copy | Four valid controls killed; unmodified/restored checks pass. See below. |
| `git diff --check` | Exit 0 at review and parent checks; repeat before commit. |

The completed parent background-agents run reported 25 PASS cases. Its assertions
include request counts, saved bytes, session identity/history, writer availability,
listener identity, pending permission and actual tool effects. Output labels
alone are not the oracle.

Quality uses the pinned clang-format 23.1.0, clang-tidy 22.1.8, Ruff 0.16.6,
ShellCheck 0.11.0, shfmt 3.14.0 and actionlint 1.7.12. A cached Zsh 5.9.2 is added
to PATH for the shell syntax check. Python is 3.14.7 and Node is 26.8.1.

### Full-suite and quality retries

The first full `make test` ran all 76 integration entries. Only `test_jobs`
failed: one snapshot test raised `OSError: Disk quota exceeded`, and the adjacent
publication test saw an interrupted supervisor instead of the expected failed
turn. The host mounts `/tmp` with a per-user quota. The parent moved its own
completed mutation scratch tree to ignored `build/agents-qa/`, without removing
other users' or unrelated workspace data. All 22 `JobsReviewedRaces` tests then
passed, followed by the complete 143-test jobs suite (two expected wasm skips).

The same quota incident broke clang-tidy's output stream in an earlier quality
run. Subsequent logs use the workspace disk rather than the quota-limited tmpfs.
The next full test run passed every entry except `test_worktree`: putting TMPDIR
inside this Git checkout invalidated two tests' deliberately non-Git directories.
Moving HOME/XDG/TMPDIR to `/var/tmp/tnyqa.YTzsU7`, verified outside any Git working
tree, made all 27 worktree tests pass. The final full run uses that environment.
Only the exact worktree/branch created by the failed fixture was removed; other
pre-existing worktree records were preserved. No product code, test assertion or
skip policy was changed to work around these environment problems.

The disk-backed quality run exposed 15 GCC 16 analyzer diagnostics in unchanged
`jobs.cpp`/ownership headers. The parent extracted the pristine baseline into an
isolated directory and ran `make analyze-cpp-gcc`: it failed with the **same 15
error lines, byte-for-byte**. They are not introduced by this dashboard change;
this record does not classify every diagnostic as a false positive. CI explicitly
uses GCC 14 for this lane (`.github/workflows/ci.yml`). The final quality command
therefore uses cached GCC/G++ 14.4.0 only for the analyzer, retaining the existing
host compiler and pinned formatter/Clang tools elsewhere:

```sh
analyzer=/nix/store/kqw7jiqkr9l2wn2n2ky4ndssji9hwcz2-gcc-wrapper-14.4.0/bin
make -j2 quality ANALYZER_CC="$analyzer/gcc" ANALYZER_CXX="$analyzer/g++"
```

The GCC 14 C++ lane and its deliberate lifetime-defect controls pass. The full
quality result and disk-backed `make test` result are recorded in the table above.
No ownership code or warning suppressions were changed for the GCC 16 findings.

### Leak-tool environment resolution

The first host `make leaks` failed because Valgrind was not on PATH. Adding the
cached Valgrind found the stripped host loader's missing mandatory redirection
symbols, so that attempt could not check memory. A separate build using cached
Nix GCC/G++ 15.3.0 and Valgrind 3.27.1 avoided the loader problem.

That build initially failed the TLS-library test because its OpenSSL RUNPATH was
missing. An unsuffixed `NIX_LDFLAGS` retry did not affect the cached wrapper.
Relinking with explicit Makefile linker flags supplied the matching cached
OpenSSL path; the complete leak gate then exited 0. No tests or leak suppressions
were weakened. The primary release and sanitizer builds were not replaced.

Reproduction of the passing leak lane (with clean temporary HOME/XDG and the
cached Valgrind bin directory on PATH):

```sh
cc_dir=/nix/store/3d1c302vw7kc8a5vknhmn34c0pd7zm6m-gcc-wrapper-15.3.0/bin
ssl_dir=/nix/store/1mf3lj0mldr8732yvzjc12fig2407b3d-openssl-3.6.3/lib
make -j4 BUILD=build/agents-leaks-nix \
  CC="$cc_dir/gcc" CXX="$cc_dir/g++" \
  DBG_LDFLAGS="-pthread -ldl -Wl,-rpath,$ssl_dir" \
  REL_LDFLAGS="-Wl,--gc-sections -pthread -ldl -Wl,-rpath,$ssl_dir" leaks
```

A separate attempt to provision the complete Nix development shell failed on
uncached upstream source/patch fetches. The passing leak lane used already cached
compiler/library binaries directly; it does not claim `nix flake check` passed.

### Targeted negative controls

The parent copied the current tracked/untracked source and build objects into an
isolated temporary tree. It reused the mutation harness's source-write/object
invalidation helper. The working tree and gate binaries were never mutated.

| Deliberate defect | Detecting test | Result |
| --- | --- | --- |
| Change the stale-wizard replica guard from OR to AND. | `saved_view_discards_stale_provider_wizard` | Valid build; test exited 1. |
| Disable the replica command allowlist guard. | `locked_saved_inspection_retry` | Valid build; test exited 1. |
| Skip the fresh-under-lock session reload. | `locked_saved_inspection_retry` | Valid build; test exited 1. |
| Invert the checkpoint test in the prompt refusal guard. | `provider_free_inspection` | Valid build; test exited 1. |

An initial fourth control that replaced the entire prompt condition with `false`
did not compile because it made the parameter unused. It is **not** counted as a
killed control. The valid inverted-condition replacement was run separately.
Unmodified baselines passed. Restored source files were byte-compared with the
working tree, rebuilt, and the detecting checks rerun. This is four scoped
negative controls, not a claim of exhaustive mutation coverage.

## Artifact and local evidence

Primary stripped release `build/tny`: **1,000,064 bytes**.
Linked runtime dependencies: libstdc++, libm, libgcc_s, libc and the Linux loader.
OpenSSL is loaded dynamically for TLS. No speed or size improvement is claimed.

Verified release SHA-256:

```text
0660db05b77aa6b63af15e3644d7fb242620b1b6cae251c8094faf2bfd9087f8
```

Local parent logs, exit records, input diff and negative-control script are under
`/tmp/tny-agents-parent-qa.cGhTO1/`. Independent re-review used copies under
`/tmp/tny-rereview.fUssdO/`. These paths are local evidence locations, not shipped
artifacts or durable CI links. This committed summary retains the results.

## Remaining limits

- No live-provider product tests were authorized or run; all product inference
  fixtures used synthetic credentials and local mocks.
- macOS/wasm execution and `nix flake check` were not run. Playwright-dependent
  browser tests and separately selected browser acceptance checks were skipped
  or explicitly reported not run by the suite.
- The reviewer did not fault-inject every allocation/spawn/connection failure or
  exercise live cross-worktree pending permissions.
- Another attached owner must detach before the viewer can obtain control.
  There is no forced takeover.
