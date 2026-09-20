# Purposeful swarms: delivery evidence and state of play

Date: 2026-09-20. Baseline `28011be`; runtime/test checkpoint `84a0d12`.
This consolidates the earlier lane notes, whose sandbox limitations describe
historical attempts rather than the completed host checks below.

## Implemented

Versioned strict JSON definitions, required coordinators at every nesting level,
explicit purpose and identity, bounded validation, executable flattened teams,
scoped peer messaging, directory-notification completion waits, durable activation
identity/goal recovery, provenance validation, and immutable inherited instruction
snapshots. Existing numeric/slash swarm mode remains available. See ADRs 0157/0158
and [the user guide](../../purposeful-swarms.md) for v1 boundaries.

## Verification

| Check | Recorded input | Result |
|---|---|---|
| Release + purposeful/lifecycle/context/collective/wait/benchmark integration pack + unit suite | `84a0d12` | Exit 0, 106.6 s; 520 main unit tests, 29,392 assertions; two additional ownership test executables pass. |
| `make quality` | `84a0d12` | Exit 0, 212.3 s: formatting, C/C++ Clang analysis, strict warnings and language/workflow linters. GCC analyzer explicitly skipped on Darwin. |
| `make -j4 leaks` | `84a0d12` | Exit 0, 65.8 s; all checked suites report zero leaked bytes. |
| Focused manifest mutation experiment | Historical `0694449` parser checkpoint; predates the FIFO-read hardening | Five valid mutants killed; zero survived; four uncompilable mutants excluded. |
| Full `make -j4 test` | Earlier `00e717f` runtime | Exit 2. Only `test_background_agents` and `test_tui` failed. All other reported suites passed. |
| Baseline/candidate reproduction of both full-suite failures | Baseline `28011be` and candidate `00e717f` executables, identical unchanged PTY tests | Both reproduce on both executables: synthetic held-session dashboard discovery; banner scrolled out by the help overlay. Not attributed to this PR. |

The complete aggregate was not rerun after the narrow capability/metadata changes;
the affected regression pack and all quality/leak gates were rerun. This is not an
all-platform or fully green aggregate claim. Linux/GCC and platform CI remain to
be observed separately. The current stripped Darwin arm64 artifact is 1,170,416
bytes and links only the system libc++ and libSystem dylibs.

Private command/status/log records are retained under
`~/.cache/tny-purposeful-swarms-20260920` and its `-resume` sibling. Their JSON
status records establish terminal state and exit status; a running PID was never
treated as a pass. Raw benchmark HOME/login references are not committed.

## Reviews

The earlier independent review identified permission, activation recovery,
saved-state validation, context inheritance, compiler provenance and legacy
compatibility issues. Regression-backed fixes are present in `843e548`,
`67a6b02`, `d2cd60d`, `cc8e3f0`, and `00e717f`.

A fresh read-only `gpt-5.6-sol` high-effort review completed successfully and
independently traced the retained live failures. It confirmed `be72ec2` as the
appropriate narrow capability fix and `84a0d12` as the collaborator-accounting
fix. Its cancelled-without-turn-end accounting edge case was added. Original
accounting prose was corrected: job session IDs are success-only, not cleared.

Remaining diagnostic limitation: unsuccessful ask jobs still use the existing
generic `JOB_IO_FAILED`; the canonical permission-denied stop remains visible in
the attempt log. Root-initiated premature cancellation is a model/task failure,
not a spontaneous transport failure. No permission is widened to suppress either.

## Live evaluation before capability alignment

[Sanitized per-trial evidence](live-before.json) preserves all 12 original trials:
two paired repetitions of three increasing-complexity tasks. Both arms used
`gpt-5.6-sol` with medium effort, checked against every recorded model session.
All artifact correctness checks passed. Orchestration passed 3/6 for tny versus
6/6 for Codex. All tny trials actually launched three collaborators; Codex chose
zero despite enabled matching-model subagent settings. See the
[methodology](benchmark-methodology.md) for unmatched permissions/tools, cache
limitations and the distinction between artifact correctness and orchestration.

| Task | tny median seconds | Codex median seconds | tny orchestration | Codex orchestration |
|---|---:|---:|---:|---:|
| Pure Unicode function | 112.139 | 50.162 | 1/2 | 2/2 |
| Durable concurrent ledger + CLI | 188.169 | 172.925 | 0/2 | 2/2 |
| Retrying DAG scheduler | 159.800 | 169.781 | 2/2 | 2/2 |

These are retained diagnostics, not performance claims about the changed binary.
The original raw result stays unchanged; published counts were recomputed from
attempt logs using the corrected accounting, with original result hash recorded.

## Post-fix evaluation

A separate one-pair-per-task ladder is being measured against frozen `84a0d12`:
SHA-256 `af3f1c3110aacbd692925cebce9e6d9225ac8d45da41356ee3501c1459b8d5f4`.
No development inference or local quality gate overlaps its timed trials.
Its results are pending; the pre-fix timings must not be reused as post-fix proof.

## CI portability finding

The initial PR Linux lanes rejected the recovery path's `snprintf` under GCC's
`-Werror=format-truncation`: GCC did not propagate the preceding exact 32-character
hex validation through that call. The patch copies the already-validated ID plus
its terminator directly, without suppressing diagnostics or weakening validation.
A regression also feeds 31-, 33-, 255-character and non-hex directory names.
This changes neither valid IDs nor swarm semantics; the live binary remains frozen.
Final local gates and a current-input mutation run are scheduled after the live
ladder, then the corrected branch will be pushed for another CI run.

