# C++ series evidence

Contract: [contract.md](contract.md)
State: INCOMPLETE. Baseline captured before implementation; all implementation gates pending.
Baseline: 1d8ad71d66c06c726b3c5b35e367fec678031e85. Primary main remains fb232e8; integration starts from current origin/main.
Platform: macOS arm64, Apple clang 21.0.0, Python 3.14.7, Node 26.8.2.
Discovery: GitHub issues 137/138/139 open and saved into full initial contracts. No native goal; tool authorization takes precedence over skill default. Docker CLI exists but default daemon absent; remote/hosted platform inventory pending.
Reviews, delegation, gate results, merge and cleanup: pending.

## Startup/size tooling lane (issue #137 item 6, 2026-09-16)

Assigned worktree: `/Users/tomas/projects/tny-cpp-build`, branch
`migration/cpp-build`, starting revision `c6d938a` (contracts over `1d8ad71`).
Initially clean. This lane owns startup/size tooling and policy only; it does
not establish the full P1-I6 or S-I1 parser/queue/TTFT/platform gates.
Existing series/phase contracts were read before implementation; no replacement
contract or goal was created for this bounded assignment. No subagents,
provider calls, pushes, main-checkout edits, or baseline-checkout writes.

Policy frozen before migration candidate evaluation:
[ADR 0115](../../adr/0115-startup-size-reporting.md), SHA-256
`b17f2cd806b5156a0133819f8b70b4c165d5b4e745c75cfb06e162a1dfa36997`.
No native or wasm ceiling was changed. Existing ADRs remain untouched.

Coverage: `tests/bench/bench_startup.py`, its seven tests, README, appended
Makefile targets and size documentation. `test-bench-startup` is an additional
`make test` prerequisite, keeping all owned test code under tests/bench/.
Nix source.nix already includes all tests/ and Makefile; tests.nix already
supplies Python. The focused suite uses Python executable stubs and mocks
strip/dependency inspection, so it adds no Nix package dependency. Actual
reporting uses the reference host's strip/otool tools.

Initial checks and environment diagnosis:

| Command | Exit | Result |
| --- | ---: | --- |
| `python3 tests/bench/test_bench_startup.py` (initial five cases) | 0 | PTY split marker, child exit/timeout, isolation, threshold edges, order/counts, CLI failure |
| `ruff check` on both new Python files (initial) | 1 | Two import-order findings, fixed with Ruff |
| `make test-bench-startup` (expanded seven cases) | 0 | Adds size-copy/runtime/wasm accounting and forged-statistics rejection |
| `ruff check tests/bench/{bench_startup,test_bench_startup}.py` | 0 | Final focused lint |
| `ruff format --check tests/bench/{bench_startup,test_bench_startup}.py` | 0 | Final focused formatting |
| `make -C /Users/tomas/projects/tny-cpp-baseline release BUILD=/Users/tomas/projects/tny-cpp-build/build/startup-baseline` | not collected | Failed; Apple LTO: output-stream bad file descriptor with default temporary directory |
| Same baseline build with `TMPDIR=/Users/tomas/projects/tny-cpp-build/build/tmp` | 0 | Successful release compile/link/strip; all writes in assigned worktree |
| `make test` (first attempt) | 2 | Same LTO temporary-directory failure, before full suite |
| `git add` / `git commit` | 128 | Sandbox denied shared worktree index.lock; no commit created |

The Git metadata path is outside the writable worktree, under the main
checkout's `.git/worktrees/tny-cpp-build/`. It was not modified. Committing and
leaving a clean tree require the coordinator's permitted Git write environment;
this lane cannot override its sandbox or request escalation.

An exploratory same-prebuilt-binary run passed (help 3.3792/3.3643 ms,
version 3.3821/3.3962 ms, prompt 3.1499/3.1292 ms), but overlapped quality checks
and used an existing binary with unattested build flags. It is superseded by
the rebuilt reference capture below, not used as candidate performance proof.
Full local build/check logs are retained in ignored `build/startup-logs/`.

`make quality` completed with exit 0 (format, clang-tidy, strict warnings,
Ruff, shell checks, workflow and JS checks; normal Darwin GCC analyzer skip).
`make size-report` and unchanged `make size-check` both exited 0: 1,086,288
bytes against the 1,887,436-byte Darwin budget, libSystem only, no C++ runtime.
The first `TMPDIR=$PWD/build/tmp make test` run is INVALID as product
verification: the coordinator identified inherited `TNY_TOOLS=terminal` and
`CURSOR_API_KEY` contamination. Its tool-status assertion failures do not
establish baseline or product defects. No code was added to tolerate them.
The coordinator reports the unmodified baseline passes with both unset.
Separately observed sandbox denials (mktemp and ps) are retained as environment
observations only. A fresh explicitly sanitized run supersedes this attempt.

### Rebuilt baseline and reporting validation

Reference build metadata and exact compile/link commands:
[build provenance](artifacts/startup-baseline-build-2026-09-16.txt).
Final measurements: [JSON](artifacts/startup-baseline-2026-09-16.json),
[Markdown](artifacts/startup-baseline-2026-09-16.md).
The baseline checkout remained read-only; the rebuilt binary is
`/Users/tomas/projects/tny-cpp-build/build/startup-baseline/tny`.
Both roles use that exact binary, SHA-256
`34bb033a40129e937bbc4ab68a1d938adee6553861f5f2acc2b822d0a35fee51`.

Command: `python3 tests/bench/bench_startup.py --baseline "$PWD/build/startup-baseline/tny" --candidate "$PWD/build/startup-baseline/tny" --json docs/verification/cpp-series/artifacts/startup-baseline-2026-09-16.json --label pre-series-1d8ad71 --build-metadata '<compiler/flags/provenance recorded in JSON>'` → **0**.
102 help and 102 version observations per role; 21 PTY observations per role,
three alternating batches. No discarded observations. Quality had completed;
existing integration fixtures still ran concurrently, as the metadata states.
This is a low-noise same-binary check under that load, not an idle-host claim.

| Metric | Baseline median ms | Other role median ms | Baseline p95 ms | Added median ms | Gate |
| --- | ---: | ---: | ---: | ---: | --- |
| help | 3.403313 | 3.363271 | 3.946333 | -0.040042 | PASS |
| version | 3.244271 | 3.241042 | 3.529500 | -0.003229 | PASS |
| PTY composer | 2.976250 | 2.970125 | 3.158708 | -0.006125 | PASS |

Stripped bytes: **1,086,288**. Dependency: `/usr/lib/libSystem.B.dylib`;
no libc++/libstdc++ detected. Peak baseline child RSS: help/version 2,179,072
bytes, PTY 2,588,672 bytes. This is peak-through-termination, not idle RSS.
Wasm/glue absent on this host and explicitly marked unavailable.

Two-result comparison of the saved JSON against itself exited **0**;
the deterministic regression fixture exits **1** as required. Source state
for the completed tooling checks is recorded in
[startup-tooling-source.sha256](artifacts/startup-tooling-source.sha256).

`make bench-startup BASELINE_TNY="$PWD/build/startup-baseline/tny" STARTUP_LABEL=tooling-lane` → **0**; saved `build/startup.json`/`.md`. This verifies the Make wrapper against this C-only tooling checkout, not a migrated candidate.

### Assigned-scope reconciliation

| Requested deliverable | Result |
| --- | --- |
| Help/version, three interleaved batches, minima/median/p95/raw/gates | PASS: full real-binary run plus deterministic edges/order tests |
| PTY first-composer paint before backend, fresh launches, thresholds | PASS: exact marker, isolated OpenAI/no input, real baseline plus split/timeout tests |
| Stripped size, dependencies, explicit C++ runtime, available wasm/glue | PASS reporting; native real report and deterministic wasm/runtime tests; actual wasm artifact unavailable |
| Peak child RSS for the same launches | PASS: per-launch fresh-worker RUSAGE_CHILDREN, raw bytes retained |
| JSON/Markdown and two-report comparison/nonzero regression | PASS: real output/comparison and regression-exit fixture |
| Appended Make targets and make-test coverage | PASS: wrapper run and seven tests executed by make test |
| ADR 0115 frozen policy, no relaxed/disabled size check | PASS: policy hash and existing size-check exit 0 |
| Fresh pre-series reference build and retained metadata/raw evidence | PASS: clean read-only 1d8ad71 sources; build output confined to assigned worktree |
| make quality | PASS, exit 0; no added warnings/suppressions |
| Full make test | INCOMPLETE: sanitized run exit 2; failed groups recorded below; contaminated attempt discarded |
| Small commits and clean worktree | BLOCKED: shared Git index is outside permitted writes; no commits/staging succeeded |

Overall assigned task: **INCOMPLETE**, despite functional startup/size tooling
checks passing. The contaminated full-suite run supplies no product verdict;
the sanitized run also has unmet gates listed below. Final migrated-candidate performance,
TTFT/parser/queue/resource and cross-platform gates remain owned by the series
coordinator. No claim that P1-I6 or S-I1 as a whole is complete.

### Coordinator correction and sanitized rerun

On resume, inspected git status/diff first; all owned changes remained intact.
Tool-launched shells still expose TNY_TOOLS (presence checked without printing
values), so the new run explicitly removes both variables using `env -u`.
The benchmark's own launch environment is allowlisted, so neither contaminant
was inherited by any measured child; baseline timing remains valid.
No production code, fixture tolerances, or check bypass was introduced.

Sanitized command: `env -u TNY_TOOLS -u CURSOR_API_KEY TMPDIR="$PWD/build/tmp" make test`.
Verified both variables absent in a child launched with this prefix. OpenAI,
background and ephemeral assertions now pass; this confirms the coordinator's
correction and replaces the contaminated tool-status conclusions. The clean
run independently reproduces denied system-temp creation in `test_acp_ws` and
`test_provider_setup`, and denied `ps` execution in `test_ask_events`.
A resumed `git add` attempt also returns 128 on the same index.lock denial.
No production/fixture workaround has been added. Sanitized aggregate pending.

Final frozen-source quality rerun:
`env -u TNY_TOOLS -u CURSOR_API_KEY TMPDIR="$PWD/build/tmp" make quality` → **0**.
No tooling source changed between the source manifest and this run.
The sanitized full suite also passed the formerly contaminated background,
ephemeral, extension, intercept and isolation groups. Remaining denied `ps`
operations affect job/cleanup and subagent process-observation fixtures.
`test_libtny_custom_tools` additionally reports a denied Swift module-cache
path. A direct typecheck with `-module-cache-path "$PWD/build/swift-cache"`
returned **1**, `error: permissionDenied`; it supplies no positive proof.
`test_image_workflow` reports two Python SDK `IOError (io, code -7)` cases;
their cause is not established here. Neither the benchmark nor production
code was changed to tolerate any of these failures.

### Final sanitized aggregate and handoff

Sanitized `make test` completed with exit **2**. Failed integration groups:

- `test_acp_ws`
- `test_provider_setup`
- `test_ask_events`
- `test_image_workflow`
- `test_job_artifacts`
- `test_jobs`
- `test_jobs_cleanup_hold`
- `test_libtny_custom_tools`
- `test_subagent`
- `test_subagent_diagnostics`
- `test_tui`

[Machine-readable check results](artifacts/startup-tooling-checks-2026-09-16.json).
The contaminated run is excluded from this verdict. Full-suite proof remains
unmet; no benchmark code change or weakened fixture is proposed as a remedy.
`git diff --check` and source-manifest verification exited **0**. Only the
assigned files are modified/untracked. No commits could be created because
Git staging remains denied (exit **128**); the worktree is therefore not clean.
Coordinator must rerun unmet gates in a suitable environment, resolve the
unclassified SDK I/O failures if still present, and create the requested
logical commits. No push was attempted. All launched checks have completed.

### Review findings 3 and 4: startup tooling corrections (2026-09-16)

Assigned P1-I6 tooling follow-up on clean `a4fd3a4`, confined to
`migration/cpp-build`; no sub-agents, commits or pushes. This disposition
supersedes the earlier two-report regression-exit claim above, not the wider
series completion status.

- Finding 3: `--compare` now labels its deltas **informational, not a contract
  verdict**, prints no PASS/FAIL and exits 0 for valid reports regardless of
  thresholds. Invalid inputs retain exit 2. Fresh paired measurement remains
  the contract-verdict path. README and CLI help clarify that distinction;
  ADR 0115 already requires alternation and does not promise a cross-report
  verdict, so its frozen text is unchanged.
- Finding 4: threshold subtraction and allowance calculation use decimal
  arithmetic; JSON report numbers remain numeric. `4.0 -> 4.4 ms` passes,
  `4.0 -> 4.400001 ms` fails, and existing exact 5/10 ms ceiling tests still
  fail as required.
- Regression evidence: before the implementation fix, the new tests failed
  for both cross-report scenarios (`1 -> 8` and historical `4.5 -> 3.5`, with
  paired baseline 3) and the decimal boundary. Final suite has nine tests.
  Isolated mutations restoring verdict output or float subtraction each
  exited 1 on the intended assertions; delivered sources were not mutated.
- The first quality attempt stopped at Ruff formatting (exit 2); applied the
  formatter only to the two owned Python files and reran all requested gates.
  Source manifest refreshed and independently verified (exit 0).

Final requested gates on the refreshed source manifest:

| Gate | Exit code |
| --- | ---: |
| `python3 tests/bench/test_bench_startup.py` | 0 |
| `make test-bench-startup` | 0 |
| `make lint-py` | 0 |
| `make quality` | 0 |

Quality emitted no warnings; macOS reports the expected Linux GCC analyzer
skip. Local logs are retained under `build/startup-review-*.log`. Both assigned
findings are resolved; this is tooling-fix proof, not final candidate performance
or whole-series completion proof. Changes remain uncommitted as requested.
