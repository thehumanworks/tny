# Purposeful swarms: delivery evidence and state of play

Date: 2026-09-20. Baseline `28011be`; delivery source `8be8d4e`; timed binary `84a0d12`.
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
| Release + purposeful/lifecycle/context/collective/wait/benchmark integration pack + unit suite | `84a0d12` | Exit 0, 106.6 s; 520 main unit tests, 29,392 assertions; two additional tool-profile test invocations pass. |
| `make quality` | `84a0d12` | Exit 0, 212.3 s: formatting, C/C++ Clang analysis, strict warnings and language/workflow linters. GCC analyzer explicitly skipped on Darwin. |
| `make -j4 leaks` | `84a0d12` | Exit 0, 65.8 s; all checked suites report zero leaked bytes. |
| Focused manifest mutation experiment | Clean detached `580094f` worktree; parser and parser tests unchanged at `8be8d4e` | Exit 0; five valid mutants killed; zero survived; four uncompilable mutants excluded. |
| Release, focused regression pack, unit tests and leak rerun | `580094f` | Both commands exit 0; repeat after the exact-length recovery-copy change. |
| `make -j4 test-runner-ownership` | `580094f` plus the fixture-only change committed as `1815d8c` | Exit 0; descriptor/resource acquisition, cancellation/reaping, allocation faults, persistence and checkpoint cleanup oracles pass under ASan/UBSan. |
| Final serialized quality + focused/unit + runner ownership + leak gate | Production source `8be8d4e`, documentation-only checkpoint `d4d2d8d` | Exit 0, 417.9 s. Main unit invocation: 521 tests, 520 passed, zero failed, one Linux-only decoder test skipped on Darwin; 29,392 assertions. Full local quality and zero-leak checks pass. |
| Additional ownership/fault parity | `fab6fad` (test-only checkpoint-schema extension) | Exit 0, 67.7 s: checkpoint mutation, subagent ownership, runtime ownership, parser/backend ownership, parser ownership, search ownership, native request ownership and formatting checks. Platform-specific skips remain explicit. |
| Full `make -j4 test` | Earlier `00e717f` runtime | Exit 2. Only `test_background_agents` and `test_tui` failed. All other reported suites passed. |
| Baseline/candidate reproduction of both full-suite failures | Baseline `28011be` and candidate `00e717f` executables, identical unchanged PTY tests | Both reproduce on both executables: synthetic held-session dashboard discovery; banner scrolled out by the help overlay. Not attributed to this PR. |

The complete aggregate was not rerun after the narrow capability/metadata changes.
This is not an all-platform or fully green aggregate claim. A subsequent quality
invocation during overlapping builds failed while expanding the regenerated
version header; it is not counted as a pass. The final serial quality, focused,
ownership and leak invocation completed successfully, with a distinct terminal
status, as did the additional ownership/fault parity invocation. No final product
source changed after `8be8d4e`; later source changes only extend fixture coverage.
The timed Darwin arm64 artifact at `84a0d12` is 1,170,416 bytes and links only the
system libc++ and libSystem dylibs. Its size is not attributed to an unmeasured
later binary.

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

[Sanitized post-fix evidence](live-after.json) contains all six trials, completed
at 2026-09-20 12:26:24 UTC: one matched pair for each task. Both arms used
`gpt-5.6-sol`, medium effort and Codex CLI `0.156.0-alpha.8` where applicable.
Every observed session's model matches; input, output and cache usage coverage is
complete. All three tny runs and all three Codex runs passed external correctness,
protected-file, model-identity and orchestration-completion checks. There were no
timeouts or cleanup-induced successes. Each tny run launched three participants;
each Codex run chose zero despite subagent availability.

| Task | External checks per run | tny seconds | Codex seconds | tny/Codex time | tny input tokens | Codex input tokens |
|---|---:|---:|---:|---:|---:|---:|
| Pure Unicode function | 37 | 128.154 | 47.206 | 2.715 | 476,849 | 73,451 |
| Durable concurrent ledger + CLI | 21 | 137.616 | 191.669 | 0.718 | 518,620 | 175,582 |
| Retrying DAG scheduler | 121 | 169.584 | 138.862 | 1.221 | 628,020 | 167,333 |

Input counts include their cached subset and include root plus every participant;
do not add cached tokens again. tny cached-input fractions were 76.9%, 81.2%, and
76.5%, respectively. The full JSON also records output and per-session usage.
Token usage remained higher for tny in every pair: approximately 6.49x, 2.95x,
and 3.75x input. There is no token-efficiency or general speed superiority claim.

Collaboration is observed rather than inferred from launch count: the runs contain
10, 5, and 7 direct peer messages and 1, 1, and 2 coordinator-upward messages.
There were still 1, 1, and 2 recoverable mailbox tool-call errors. Successful job
completion does not mean every message was consumed/acknowledged; final durable
receipt states are retained. At-least-once delivery and bounded waits do not
prove consensus, full message consumption or task correctness.

The sampled permission-denial failure pattern did not recur after aligning tools
with participant capabilities. This is 3/3 post-fix runs versus 3/6 before, not a
statistical reliability guarantee. The ledger pair was about 28.2% faster; the
small function and scheduler were slower. Prefer a single agent for small work
unless an independent perspective is worth the overhead; the data does not yet
justify automatically applying a fixed swarm to every task.

The binary was frozen at `84a0d12`, SHA-256
`af3f1c3110aacbd692925cebce9e6d9225ac8d45da41356ee3501c1459b8d5f4`.
No development inference or local quality gates overlapped these timed trials.
Subsequent delivery changes are an equivalent validated-ID copy, ownership-test
signature compatibility, and a Linux-only notification decoder repair. They are
not retimed here. Results are a small synthetic ladder with different harness
policies/tool surfaces and uncontrolled provider caching, not a causal estimate
of swarm benefit. Both pre-fix failures and post-fix data remain published.

## CI portability findings and delivery status

Draft PR: https://github.com/thehumanworks/tny/pull/173.
Branch: `feat/purposeful-nested-swarms`; primary worktree remains on clean `main`.

The initial Linux lanes rejected recovery's `snprintf` under GCC
`-Werror=format-truncation`; `580094f` copies the already exact-length-validated ID
and terminator directly. Its regression feeds 31-, 33-, 255-character and non-hex
directory names. No compiler warning was suppressed or validation weakened.

At `580094f`, all Linux Python/Node SDK rows, Valgrind, TSAN, and wasm-node passed.
Remaining diagnosed failures were stale private-API calls in the ownership fault
fixture (fixed/tested in `1815d8c`) and Linux-only Clang analysis of an inotify
buffer cast (fixed in `8be8d4e`). The Linux decoder now copies bounded headers
without alignment/type-punning casts and rejects truncation or lost-notification
overflow. Linux-only regression cases exercise valid single/multiple records,
truncated headers, invalid lengths, and each watch-loss mask. These intentionally
skip on non-Linux; Linux CI supplies their native coverage.

After those fixes, CI reached another stale assertion in the checkpoint ownership
fixture: its schema oracle predated both numeric swarm fields and file-definition
metadata. `fab6fad` updates the exact independent schema oracle and adds populated
metadata roundtrips, invalid/inconsistent metadata rejection, public/private field
checks and allocation-fault sweeps. It preserves explicit nulls in public legacy
snapshots rather than silently weakening presence checks. The complete checkpoint
ownership and mutation gates now pass locally.

All diagnosed CI repairs are pushed. The final local serial and additional
ownership/fault gates completed successfully, and all live benchmark processes
completed. Replacement full CI is still not established as green. The PR remains
a draft, not merged or advertised as merge-ready; the two reproduced baseline PTY
failures and unobserved final cross-platform results remain explicit limitations.
