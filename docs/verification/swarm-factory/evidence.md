# Contribution-aware swarm delivery evidence

Date: 2026-09-20. Product source: `fa22354f439bcbf2e49d26d19d8bc34f67439fe8`.
Branch: `feat/swarm-factory-effectiveness`; base includes current main through
`e5721a7`. The primary checkout remains unchanged. ADRs 0160 and 0161 describe the
contribution and typed-message design; ADR 0159 writable/yolo defaults are retained.

## Completed local verification

The final **serial** verification script completed with exit **0** in 475.1 seconds.
Its input revision and terminal process status are retained in the private
`~/.cache/tny-swarm-factory-delivery-20260920` directory, alongside logs.

| Gate | Result |
|---|---|
| `make quality` | Pass: formatting, C/C++ Clang analysis, strict warnings, Ruff, shell/workflow/JS checks. GCC analyzer is explicitly skipped on Darwin, not claimed. |
| Release and main unit suite | 529 tests: 528 passed, zero failed, one Linux-only decoder skip on Darwin; 29,504 assertions. Both explicit tool-profile follow-up invocations passed. |
| Contribution/dependency/workspace integration | 11 passed, including failed preparation, failed retained-workspace resume, truthful shared-writable evidence, corrupt dependencies and bounded fan-in. |
| Typed-message integration | 16 passed, including exact prepared identity, unrelated name/group changes, retry/conflict handling, permission refusal and queued-coordinator delivery. |
| Purposeful, lifecycle, inherited context | 8 + 6 + 6 passed. Includes optional-field absence, writable defaults and no duplicate resume launches. |
| Collective mode, completion watches, mailbox | 13 + 10 + 33 passed. |
| Effectiveness and existing benchmark self-tests | 7 + 8 passed, with frozen oracles, exact roster checks and retained failures. |
| Runner, checkpoint, subagent, runtime ownership | Pass. Runtime's platform-specific skip is explicit; allocation/fd/lifetime and checkpoint ownership fixtures passed. |
| `make leaks` | Pass; all checked suites and CLI probes reported zero leaked bytes. |

The superseded pre-review gate was intentionally cancelled with a nonzero exit
before applying material findings. It is not counted as passing. Earlier failing
regressions and the completed independent review remain recorded; see
[review.md](review.md) for the distinction between hypotheses and reproduced bugs.

This is the complete selected local gate, **not** a claim that the repository-wide
`make test` or every platform was rerun. The previous swarm delivery documented two
PTY failures reproducing on baseline and feature builds; they were not concealed
or waived here. PR CI supplies additional Linux/GCC, packaging and SDK coverage.

The stripped Darwin arm64 release measured **1,203,488 bytes**. No public C ABI or
new scheduler/broker is introduced. Unsupported execution contexts retain explicit
refusal before provider/job effects. Validation remains provider-free.

## Matched live experiment

The frozen candidate is `fa22354`, SHA-256
`9eb3d3cc02a6d42cd4bcd0332161f66534cb864994a7b0af90ff280e4f647761`.
The frozen historical v1 baseline is `8f77e71`, SHA-256
`219012a5f736e3899358a1a4c5adc5fedfcdad77add2079b5796bb0bdbf20a25`.
Both conditions use the same three named collaborators, `gpt-5.6-sol`, medium
reasoning effort, fresh workspaces and externally checked tasks. Candidate peers
explicitly choose read-only to match that historical baseline; current product
defaults remain writable. The comparison does not measure writable-main behavior
or independent isolated-implementation speed.

The six-trial experiment completed at **2026-09-20 16:17:40 UTC**, after successful
verification and completed development inference. All six trials passed external
correctness, protected-file, exact-roster and orchestration checks. Every trial
had four observed model sessions: root plus exactly three collaborators. Usage
coverage and actual model identity are complete. No failed trial was removed.
See [live-results.json](live-results.json) for all observations and source hashes.

| Task | Baseline seconds | Candidate seconds | External checks per run | Acceptance |
|---|---:|---:|---:|---|
| Unicode function | 106.971 | 140.263 | 37 | Both passed |
| Durable concurrent ledger and CLI | 195.578 | 221.907 | 21 | Both passed |
| Retrying DAG scheduler | 140.726 | 214.855 | 121 | Both passed |

Across these three pairs, the candidate used 1,881,148 input tokens versus
2,411,157 (22.0% fewer), but took 577.025 versus 443.275 summed seconds (30.2%
longer). Output totals were 37,960 versus 39,951. Input already includes cached
input. Token savings do not establish effectiveness: both conditions accepted
3/3 tasks and this sample shows **no acceptance or speed improvement**.

There were nine candidate tool errors versus four baseline errors. Candidate
communication nevertheless produced observed findings, challenges, questions,
answers and handoffs; their presence does not prove those messages improved the
answer. Five of its six typed-message errors attempted the aliases `root` or
`lead` instead of the declared root name; the sixth addressed a terminal member.
One remaining team-control error and two file-deletion errors are retained too.

The final delivery corrects that root-address information gap: every participant
now receives the exact JSON-escaped name of its upward coordinator, separately
from the raw mailbox index. Unknown-recipient diagnostics also name the root.
A nested queued-coordinator regression checks both routes. No aliases are
silently authorized, and this post-evaluation guidance change is **not retimed**.
The measurements remain about frozen `fa22354`, not a claim for the changed prompt.

Declared dependencies trade concurrency for usable causal evidence and may add to
the critical path. This experiment changes several mechanisms together, has one
pair per synthetic task and uncontrolled provider caching, and does not isolate
causes or establish general statistical gains. Larger, repeated repository tasks
are needed to assess task-solving effectiveness. The verified product gains here
are contribution control, causal handoffs and correctness under tested failure
cases, not demonstrated superiority on the live ladder.

Raw credentials, login references, model transcripts and generated workspaces
remain private; only sanitized aggregate/per-trial records are committed.

Acceptance criteria in a definition are declarations for review, not automatically
proven checks. A passed tool, terminal process or acknowledged message never
substitutes for external correctness. Isolated commits still require explicit
integration; completed participants are not silently relaunched.

## Review submission and portability follow-up

PR #174 was submitted as an **open non-draft PR** at **2026-09-20 16:02:14 UTC**
(**17:02:14 Europe/London / BST**). The primary checkout is unchanged.

Linux CI identified a Clang diagnostic for copying the four-character message-ID
prefix without its intermediate terminator. The fix explicitly copies the null
byte too, then the digest overwrites it; the resulting 64-character IDs are
unchanged. No warning was suppressed. Together with the observed root-address
guidance correction, this is being checked by a new final serial gate. Superseded
checks are retained with their nonzero exits; CI and final verification statuses
are recorded separately rather than inferred from a started command.
