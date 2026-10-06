# ADR 0184: Code-cell deadlines accommodate delegated inference

Status: accepted

## Context

The execution-server migration placed synchronous `subagent` create/message
inside the enclosing `run_code` deadline. The five-second default from ADRs
0174 and 0179, retained by ADR 0180, could cancel ordinary child inference and
tool work. Existing end-to-end fixtures always supplied thirty seconds, so
they did not exercise the omitted-timeout path.

A live Codex `gpt-6-luna` reproduction started a child whose code cell waited
seven seconds before writing its result. The parent's omitted deadline expired
first: its tool failed, the child's persisted exit was 130, and the result file
was absent. A plain-text child completed within the old budget, showing that
launch and account propagation alone were insufficient acceptance checks.

## Decision

Default `run_code.timeout_ms` to 600,000 milliseconds, the existing maximum.
Explicit budgets remain integers from 1 through 600,000. One whole-cell deadline
covers Python work, nested tools and synchronous child inference; it is never
reset per nested operation. Trusted human-wait extensions retain their existing
separate bound. This supersedes only the default-deadline clauses of ADRs 0174,
0179 and 0180.

The runtime, provider schema and model instructions derive their numeric bounds
from the same constants. The tool description explains synchronous delegation.
Model instructions require schema discovery before the first nested call, so
hidden provider-level schemas do not invite guesses about required fields such
as `subagent.action`.
Cancellation, process ownership, state persistence and no-replay rules retain
their existing behavior. Wasm and unsupported embedded contexts still fail
explicitly; a longer default adds no execution fallback.

## Consequences and verification

Ordinary delegation and builds can finish without the model supplying a special
timeout. Forgotten explicit deadlines also allow blocked or runaway code to run
longer before automatic interruption, within the unchanged ten-minute ceiling.
Callers can set a shorter budget or interrupt active work. Jobs and teams remain
the interface for work that must outlive a cell.

Production CLI end-to-end checks cover both HTTP wires, omitted timeout with
delayed create/message, actual child tool effects and stored sessions, explicit
short-timeout cancellation, parent interrupt and no late effects. The human-wait
regression explicitly requests five seconds so its delayed approval still tests
budget exclusion. Native CI lanes run the delegation checks, and an opt-in live
Codex `gpt-6-luna` check verifies real inference and child work. See the
[evidence and learnings](../verification/subagent-code-mode.md).
