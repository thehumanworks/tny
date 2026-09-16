# Verification contract: C++ phase 3 (#139)

Created: 2026-09-16, before implementation.
Baseline: `1d8ad71d66c06c726b3c5b35e367fec678031e85`; clean integration worktree.
Platform: macOS arm64; Apple clang 21.0.0; Python 3.14.7; Node 26.8.2.
Scope authority: user requested delivery of #137 -> #138 -> #139 using GPT-6 Astra low/high subagents, separate worktrees for parallel work, verified merges to local main and cleanup after completion.
Native goal: none (`get_goal` confirmed). Active tool rules permit creation only on an explicit goal request; this ordinary delivery request does not authorize one. That higher-priority authorization rule takes precedence over the skill's default goal-linked mechanism. The complete invariant set below remains binding; no implementation or verification scope is reduced.
Workflow: verification-contract and worktrees; root coordinates all review, ADR serials, integration, hosted checks, local-main merge and cleanup. Agents cannot spawn children or merge main. Existing unrelated worktrees and live sessions are excluded.
The issue snapshot below is the complete normative specification. Its plan steps and required tests are requirements, not optional suggestions. Each P*-I* row retains its exact pass criteria. Deliver local main only when all original gates pass. PRs may supply hosted platform proof; remote main is not to be merged or pushed without separate authorization.

## Requirement and check mapping

- R3: deliver every implementation-plan item, acceptance invariant and required test in the immutable issue snapshot below; preserve explicit scope boundaries.
- C0: contract.initial.md byte equality before implementation and saved initial SHA256; source/ADR manifest captured before writing production code.
- C1: focused independent design review before implementation, different first-slice review before repeating the ownership pattern, and additional risky-boundary review.
- C2: implementation and behavior checks named in each P3-I1 through P3-I6 row; every row must pass together on the integrated source. Owners assigned in the series contract and evidence.
- C3: all named required build, quality, sanitizer, leak, fault, ABI/SDK, platform and fixture commands below; all warnings/errors resolved. Missing environments remain unmet.
- C4: all planned critical mutations below; baseline passes and intended behavioral assertion fails, then restore and rerun. Invalid mutants do not count.
- C5: startup/performance measurements below, raw samples and exact inputs retained. Phase 3 includes cumulative pre-series comparison.
- C6: immutable ADR comparison, final source/test/config/dependency manifest, requirement reconciliation, merge to local main, task-owned worktree cleanup only after completion.

## Independent reviews

Design: challenge allocator failure containment, ownership/view lifetimes, platform/build coverage and accidental scope expansion. First slice: inspect actual owned bytes/JSON/event/resource boundary with executable tests before conversion is propagated. Reviewer has fresh bounded context and does not implement or spawn agents. Root records each finding and disposition.

## Issue snapshot

## Outcome

Make runner/job descriptor, lock and process-scope ownership explicit through small C++ resource types. Preserve detached execution, continuous writer ownership through restart, safe cancellation and truthful cleanup status when failures occur.

### Why this is a priority

Runner/job paths manage sockets, pipes, advisory locks, process scopes, temporary state and sensitive handoff buffers across many early exits. Premature release can affect another session or lose ownership of live work. RAII can reduce missed cleanup and accidental duplication, provided the shutdown protocol remains explicit. Destructors cannot replace quiescence, process-reaping or persistence proof.

## Product intent and baseline

The user prioritizes fast startup, extensibility, and reliability over the old strict executable-size ceilings. Remaining pocket-sized and smaller than a comparable fx build is desirable; size alone must not force weaker error handling. This is a scoped migration to modern C++20, not a rewrite of the whole repository.

Planning baseline: `main` at `1d8ad71d66c06c726b3c5b35e367fec678031e85` (2026-09-16). Re-inspect current main and outstanding changes before implementation. Source paths below describe that baseline; no migration or performance improvement has been demonstrated yet.

## Dependencies and scope

Depends on **[#137](https://github.com/thehumanworks/tny/issues/137)** and **[#138](https://github.com/thehumanworks/tny/issues/138)**. Implement after their integrated gates pass; use their C++ ownership, allocator/error-boundary and benchmark conventions.

Starting points:
- [Runner spawn, restart and client resources](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/core/runner.c), `src/core/runner.h`.
- [Job supervisor resources and transactions](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/core/jobs.c), `src/core/jobs.h`.
- Narrow ownership adapters around `src/util/process.c`, `src/util/process_scope.c`, `src/util/jobs_host.c` and the session writer-lock operations in `src/core/session.c`.
- Behavioral contracts: ADRs 0053, 0081, 0093, 0099, 0104, 0107 and 0108. This refactors the existing job feature related to #124; it does not reimplement it or close that issue as a side effect.

Keep OS-specific operations behind the current C seams. Do not port the whole CLI/TUI, redesign the on-disk session/job schema, change permission policy, introduce a daemon, switch process platforms, or convert `tnytty`.

## Implementation plan

1. Inventory every owned and borrowed descriptor/lock/process-scope in runner spawn, restart, client teardown, supervisor admission, cancellation and terminalization. Record the exact acquisition/transfer/release ordering and error behavior.
2. Introduce move-only descriptor and lock owners with explicit adopt/borrow/release operations. Destructor cleanup must be nonthrowing and must not publish status, run a provider, allocate an error message, or perform an unbounded wait. A parent-close after fork must not explicitly unlock the child-inherited open-file description.
3. Extract runner and supervisor resource aggregates so ownership survives ordinary returns and failure paths. Convert the inventory end-to-end; avoid retaining parallel raw owning fields and wrappers for the same resource. Keep C++ object construction/destruction valid rather than using calloc/memset on nontrivial objects.
4. Keep fallible lifecycle operations explicit: request cancel -> poll/reap/drain -> final state and snapshot persistence -> socket cleanup -> ownership release/bye. A process-scope destructor must not silently discard a failed cleanup proof. Preserve the existing hold/reservation behavior when cleanup is unknown.
5. Make restart/handoff transfer explicit. The writer description and listener must remain continuously held through checkpoint/exec; restore safely on failed exec without repeating committed tools/provider work. Separate borrowed caller-owned locks from locks acquired by the spawn operation.
6. Keep pre-exec child and signal-handler paths restricted to their existing permitted operations. Do not introduce C++ runtime initialization, allocation, exceptions or destructors into async-signal-safe-only paths. Preserve the macOS TLS fork-safety fallback and MSYS retained Job authority.
7. Review one complete spawn/failure/cleanup slice independently, then apply it to runner restart and durable jobs. Preserve secret wiping and anonymous credential IPC.

## Acceptance invariants

| ID | Observable requirement | Required proof |
| --- | --- | --- |
| P3-I1 | Each inventoried resource has one explicit owner; partial acquisition, failed spawn/exec/handshake, reset and successful teardown cannot double-close or leak it. Borrowed resources remain usable. | Move-only type checks, resource ownership inventory tied to code, real descriptor/handle counts before/after repeated lifecycle loops, forced descriptor reuse, allocation and OS-error injection. |
| P3-I2 | Session writer ownership is acquired before listener/state mutation and held through shutdown, final persistence/log drain and socket removal. Busy/unknown ownership never permits takeover. | Actual competing lock probes around each boundary, held-lock startup refusal with byte-identical protected state, immediate resume race tests, correct bye ordering and inherited-description tests. Follow ADR0104 exactly. |
| P3-I3 | Background/restart preserves the existing listener/writer ownership and resumes only unconsumed work. Failed checkpoint/exec never fabricates success or repeats committed work. | Real detach/reattach and restart-failure fixtures with provider/tool invocation counts, ownership contention, disconnect/reconnect, saved-state and socket identity assertions. |
| P3-I4 | Cancellation only affects processes owned by the operation; completion status reflects observed exit and cleanup. Unknown cleanup retains the existing persisted holds/reservations. | Live fixture child trees plus unrelated sentinel process, cancellation during admission/execution/finalization, parent/supervisor loss, EOF/blocked pipes, snapshot/write failures, selective retry, and existing cleanup-hold cases. No signalling from stored PID metadata. |
| P3-I5 | MSYS admission/ACK/GO, retained Job handles, sole-reaper rules and cleanup accounting remain correct; native POSIX, macOS fallback and wasm unsupported behavior remain unchanged. | Runtime tests on the actual supported OS environments, including `test_jobs_msys.py`; a successful cross-compile is not runtime proof. Browser/node wasm negative-path fixtures; no newly advertised capabilities. |
| P3-I6 | Explicit shutdown operations retain existing deadlines and error reporting; destructor work cannot block the event loop indefinitely or release authority early. Shared startup gates pass. | Slow/unresponsive child tests with monotonic deadlines, busy/failed cleanup and persistence faults, allocation-free teardown checks where promised, startup/TTFT and repeated job-cycle resource/size report. |

## Required tests and faults

Run `make test`, `make quality`, host leak gates, existing ABI/SDK and allocation-failure regression gates, and the relevant native/wasm CI matrix inherited from phase 1.

Directly run and extend the existing `tests/integration/test_background.py`, `test_background_agents.py`, `test_interrupt.py`, `test_jobs.py`, `test_jobs_cleanup_hold.py`, `test_job_artifacts.py` and `test_jobs_msys.py`, plus runner/session unit suites. Keep their real process, pipe and advisory-lock boundaries; mocks must not replace the ownership property under test. Run in isolated temporary state roots and clean up only fixture-owned children.

Test errors after each acquisition and before/after each transfer: open/pipe/dup/fork/spawn, handshake, allocation, state write/fsync/rename, checkpoint, exec, cancel, reaping and log drain. SIGKILL bypasses destructors: test crash recovery separately from normal RAII cleanup.

Critical mutations: release the writer before final save/socket removal; close a transferred descriptor; signal a metadata PID instead of an owned scope; treat unknown cleanup as complete; resend an already-consumed tool batch. Each must fail a behavioral oracle; compilation failures do not count.

## Performance and compatibility gate

Reuse the mixed-language policy and benchmark tooling established by the first issue in this series. Compare a clean pre-change baseline with the final candidate on the same recorded reference host, compiler, release flags, and input corpus. Include loaded C++ runtime dependencies in the accounting.

- Help/version: at least 100 samples per binary in three alternating batches; median below 5 ms and added median latency no more than max(0.25 ms, 10% of baseline). Record p95 and raw samples.
- PTY first prompt, before backend work: at least 20 fresh launches per binary; median below 10 ms and added median latency no more than max(0.5 ms, 10%). This is distinct from Enter-to-first-token.
- Run `tests/bench/bench_ttft.py --tny <absolute-binary> --repo <checkout> --bench tui --iters 20 --label <baseline-or-candidate>` and the `ask-stdin` variant against its local mock. Record deltas; investigate any repeatable regression over 10%.
- Report stripped native size, wasm plus glue size, dynamic dependencies, peak memory, and relevant allocation/resource counts before/after. These are measurements, not promises that changing language makes them smaller. Preserve existing platform capability behavior.

## Agent execution and completion

Before implementation, create `docs/verification/<issue-slug>/contract.md`, a full immutable `contract.initial.md` snapshot, and `evidence.md`. Map the invariant IDs below to checks; capture source revision, dirty state, tool versions and baseline failures. Apply the verification-contract workflow when available; these issue requirements remain self-contained if that local skill is unavailable.

Obtain focused independent design review before implementation and a different review of the first working ownership boundary before extending its pattern. Record findings and dispositions. Add a uniquely numbered new ADR under `docs/adr/` for material decisions; leave finalized ADRs unchanged.

Complete only with current evidence for every invariant on the integrated delivered source: commands, exit status, exact tested revision/input hashes, relevant hosted platform results, and mutation outcomes. Update test inventories when files move. Unavailable required platforms or unresolved critical failures are explicit unmet gates, not passing checks. Deliver a focused PR linking this issue, the contract, the new ADR, and evidence. Do not modify unrelated dirty work or live sessions.


## Final series reconciliation

This is the final phase of #137 -> #138 -> #139. Re-run affected parser/event/ABI/allocator checks against the combined final source, and report cumulative startup, size, dependency and memory changes against the pre-series baseline as well as this phase's parent. The combined startup must satisfy the same absolute and relative startup thresholds against that pre-series baseline; three individually acceptable changes must not hide cumulative regression. Preserve all earlier invariants; historical passing logs alone are not proof of the combined delivered state.

## Amendments

None.


## Delivery amendment — 2026-09-16

The current user request is to implement #137 -> #138 -> #139 in
`/Users/tomas/projects/tny`, with exactly one independent review using
`claude --model fable --effort medium -p <prompt>`, a single pass whose
feedback is dispositioned. This supersedes the prior multi-review/model
runbook, not the behavioral, performance, compatibility or failure gates.
Implementation helpers may implement bounded slices but may not review or
spawn further agents. The requested checkout is now on
`feat/cpp-ownership-137-139`; other existing worktrees, live processes and
local/remote main are preserved. No remote-main merge, issue closure or
release is authorized by this amendment. Deliver reviewable commits/PRs
with exact current evidence; unresolved gates remain explicitly unmet.
