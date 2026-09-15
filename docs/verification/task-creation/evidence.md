# Verification evidence: bundled task creation

Contract: [contract.md](contract.md), with the full immutable
[initial snapshot](contract.initial.md).
Source state: baseline `fb232e8002b1ba18bfdf12ed723146e0ac1295ab` plus
[tested-inputs.json](tested-inputs.json). All listed inputs are unchanged since
final verification began. The primary owns all invariants.

## Environment and timing

Worktree: `/Users/tomas/projects/tny-task-creation`, branch
`feat/bundled-task-creation`; original checkout clean and left untouched.
macOS arm64; Apple clang 21.0.0; clang-format 23.1.0, clang-tidy 22.1.8,
Ruff 0.16.6, Python 3.14.7, Node 26.8.2. `mise install` confirmed the toolchain.
The initial contract, full snapshot, snapshot hash and existing-ADR hash
manifest were written before the first implementation edit. Native goal
creation is not applicable under the active tool authorization rule, as
recorded in the contract. No requested scope has been reduced.

## Executed checks

| Run | Checks | Actual result and evidence |
| --- | --- | --- |
| V0 | C0 | Clean baseline and pre-implementation full contract snapshot; SHA-256 intact. |
| V1 | C1 | Final `build/tny-test -s tasks`: 10 tests, 656 assertions, all pass. Full suite's unit phase: 551 tests, 13,689 assertions, all pass. |
| V2 | C2 | Both-wire authoring and saved-task execution fixtures passed. An unrelated interrupted-stream diagnostic assertion failed later in the full OpenAI suite (see below); the full isolated rerun passed, exit 0, 51.7 seconds: [record](openai-rerun.json). |
| V3 | C3 | Final builtin: real aiproxy / grok-4.6 / xhigh creation, CLI list/show validation, and a second live execution. Both sessions done/exit 0, matching provider/model, real note read, exact final reply `PROJECT: Observatory &#124; ITEMS: 3`, unrelated and created files unchanged: [live evidence](live-final.json). |
| V4 | C4 | `make quality`, `make -j8 leaks`, `make test-shell-workflows` (Bash/Zsh), and `make size-check` all exit 0. Mac release: 1,086,288 bytes below 1,887,436. The first normalized `make test` exits 2 with 66 integration groups passing and only the already isolated/rerun OpenAI diagnostic failing; the subsequent clean aggregate rerun passed on committed source `1857b70`: all 551 unit tests / 13,689 assertions and 67 integration groups, exit 0 in 1,333.07 seconds. [Initial results](checks-initial.json), [final result](checks-final.json). |
| V5 | C4 | Additional aarch64 Ubuntu GCC 13.3.0 release: 986,192 bytes below 1,048,576; real Linux CLI selects the same builtin digest as the live Mac run: [size/runtime record](linux-size.json). |
| V6 | C5 | All 114 pre-existing ADRs match [baseline hashes](adr-baseline.json); finalized ADRs 0112/0113 match [new hashes](adr-new.sha256). New prefixes unique. Historical duplicate prefixes 0030, 0045, 0087 predate this work and remain untouched. |
| V7 | C6 | One independent read-only subagent review completed. Two findings addressed, with no second pass requested: [findings and resolutions](review.md). |
| V8 | C8 | Changed site pages match the published docs mirrors byte-for-byte; generated source matches. `git diff --check` passes. |

No new runtime decision logic, API or dependency was introduced. The builtin
registry and existing filesystem/shell tools remain the smallest coherent
implementation; a new writer tool or automatic selection mechanism was
unnecessary. Nix's existing src/tests/scripts/shell filesets cover the changed
files, and no make target, fixture directory or external dependency was added.

## Failed and superseded checks

- The first `make test` inherited the workstation's `TNY_TOOLS=terminal` and
  failed existing fixtures that expect `list_files`/`glob_files`. It was stopped
  with exit 130 and retained at `/tmp/tny-task-creation-test.log`. The normalized
  run removes that variable after Python startup, before spawning make/tests.
  Removing it only outside the Mise Python shim is insufficient: the shim can
  restore the global setting. The first baseline probe hit that issue too:
  [retained failure](baseline-stream-environment-failure.json).
- The normalized full-suite run hit an interrupted-stream case expecting
  `stream aborted mid-response` but observing `stream stalled (no data for 1s)`.
  The answer and retry succeeded; the assertion checks the diagnostic category.
  No streaming code was changed. The original baseline stream test passed
  against a binary linked with original tasks/help source and otherwise
  unchanged release objects: [baseline result](baseline-stream-check.json).
  The full current OpenAI suite also passed on isolated rerun. The original
  failed suite command remains a failed run, not a claimed aggregate pass.
- The first live authoring/execution run passed before the browser wording
  correction and was superseded: [initial record](live-initial.json).
- A strict comparison against aggregated `result.output` failed because that
  field concatenates progress narration with the final answer: [failure](live-format-failure.json).
  A second explicit no-narration request also produced progress text. Inspection
  of the real session shows a separate final assistant message exactly matching
  the required result. C3 uses that final-message boundary and retains the
  combined text plus actual tool and assistant messages in [live-final.json](live-final.json).
  No claim is made that the preset suppresses all progress narration.
- The first Linux container bind mount did not expose the new host worktree.
  Source was instead streamed through stdin into a new offline container. No
  existing daemon/container was stopped or changed; the owned container was
  removed after its artifacts were collected.

## Controlled mutations

[M1](mutation.json) renames `task-creation` in an isolated translation unit and
links it with the unchanged test objects. Baseline passes; the new test fails
at builtin selection, exit 1. [M2](mutation-shell.json) removes the shell catalog
entry in a private copy. The actual new listing assertion kills it under both
Bash and Zsh; both baselines pass. Neither mutation touched the delivered tree.
No other mutation of unchanged resolver/parser/permission logic is claimed.

## Reconciliation

| Invariant | Status |
| --- | --- |
| I1: builtin discovery and delivery | PASS: V1/V2 |
| I2: correct authoring instructions | PASS: V1/V2/V3/V7 |
| I3: aiproxy/grok live workflow | PASS: V3 |
| I4: quality, regression, simplicity | PASS: clean aggregate V4, quality, leaks, shell workflows, size and V1/V2/V5 |
| I5: ADR decisions and integrity | PASS: V6 |
| I6: independent single review | PASS: V7 |
| I7: committed/pushed branch and PR | PASS: source commit `1857b70`, remote feature ref verified before deletion, [PR #135](https://github.com/thehumanworks/tny/pull/135) now merged, [delivery record](delivery.json) |
| I8: timing, complete scope, handoff | PASS: initial snapshot and hashes intact; all R1-R5/I1-I8 reconciled; usage and proof linked here and in the PR |

Gate: PASS for the original requested scope, with no reductions. All active local/feature/process/delivery gates pass; hosted platform checks are reported separately as specified in C4/C7. Native goal finalization is not applicable under the tool authorization rule.

The macOS leak gate reported zero leaked bytes in all selected suites and CLI probes. It retains the project's documented process-spawning-suite exclusions; this is not a Linux valgrind result. Quality retains the explicit macOS GCC-analyzer skip; Linux analysis remains a hosted-CI check.

## Hosted checks and final delivery

The source commit was pushed and PR #135 was opened against main, then merged
externally at 2026-09-14 22:27:33 UTC while the final record was being prepared.
The remote feature branch was deleted after merge and was not recreated.
Final verification records therefore use the documentation follow-up branch
`docs/task-creation-verification`, based on current main `c6a0bbf`. Comparing it
with the tested source commit shows only the regenerated published wasm binary;
every input in the retained source/test/config manifest still matches.

The [initial hosted snapshot](hosted-ci-initial.json) includes a Windows
native-job fixture error (empty output decoded as JSON), while its build and
size gate passed. No task preset was selected in that failing test. An isolated
rerun request was unavailable while its workflow remained active; it was
requested again after completion. The merge and documentation follow-up also
trigger fresh hosted workflows; no claim is made
that all hosted jobs have completed. Local full-suite, live model, quality,
leak and size evidence is complete and source-bound.

Tool activation also installed the globally configured latest gcloud 584.0.0
through Mise; no Mise configuration file was changed. The task itself adds no
new tool or runtime dependency. The original tny checkout, other worktrees,
and pre-existing Colima containers remain outside this change.
