# Verification contract: C++ phase 2 (#138)

Created: 2026-09-16, before implementation.
Baseline: `1d8ad71d66c06c726b3c5b35e367fec678031e85`; clean integration worktree.
Platform: macOS arm64; Apple clang 21.0.0; Python 3.14.7; Node 26.8.2.
Scope authority: user requested delivery of #137 -> #138 -> #139 using GPT-6 Astra low/high subagents, separate worktrees for parallel work, verified merges to local main and cleanup after completion.
Native goal: none (`get_goal` confirmed). Active tool rules permit creation only on an explicit goal request; this ordinary delivery request does not authorize one. That higher-priority authorization rule takes precedence over the skill's default goal-linked mechanism. The complete invariant set below remains binding; no implementation or verification scope is reduced.
Workflow: verification-contract and worktrees; root coordinates all review, ADR serials, integration, hosted checks, local-main merge and cleanup. Agents cannot spawn children or merge main. Existing unrelated worktrees and live sessions are excluded.
The issue snapshot below is the complete normative specification. Its plan steps and required tests are requirements, not optional suggestions. Each P*-I* row retains its exact pass criteria. Deliver local main only when all original gates pass. PRs may supply hosted platform proof; remote main is not to be merged or pushed without separate authorization.

## Requirement and check mapping

- R2: deliver every implementation-plan item, acceptance invariant and required test in the immutable issue snapshot below; preserve explicit scope boundaries.
- C0: contract.initial.md byte equality before implementation and saved initial SHA256; source/ADR manifest captured before writing production code.
- C1: focused independent design review before implementation, different first-slice review before repeating the ownership pattern, and additional risky-boundary review.
- C2: implementation and behavior checks named in each P2-I1 through P2-I6 row; every row must pass together on the integrated source. Owners assigned in the series contract and evidence.
- C3: all named required build, quality, sanitizer, leak, fault, ABI/SDK, platform and fixture commands below; all warnings/errors resolved. Missing environments remain unmet.
- C4: all planned critical mutations below; baseline passes and intended behavioral assertion fails, then restore and rerun. Invalid mutants do not count.
- C5: startup/performance measurements below, raw samples and exact inputs retained. Phase 3 includes cumulative pre-series comparison.
- C6: immutable ADR comparison, final source/test/config/dependency manifest, requirement reconciliation, merge to local main, task-owned worktree cleanup only after completion.

## Independent reviews

Design: challenge allocator failure containment, ownership/view lifetimes, platform/build coverage and accidental scope expansion. First slice: inspect actual owned bytes/JSON/event/resource boundary with executable tests before conversion is propagated. Reviewer has fresh bounded context and does not implement or spawn agents. Root records each finding and disposition.

## Issue snapshot

## Outcome

Replace manual runtime-event payload cleanup and asynchronous tool-call lifetime bookkeeping with C++ ownership types while preserving the libtny C ABI, cancellation behavior, event ordering, and allocation-failure recovery.

### Why this is a priority

The runtime turns temporary provider callback data into events that can outlive the callback. It currently keeps separate field-copy/free paths; custom-tool callbacks also use manual reference counting, mutexes and generation checks. These are central reliability boundaries for the CLI, TUI and Python/Node embedders. Expect adding an owned event field to stop requiring a matching manual free-table change; do not promise that smart pointers solve protocol races.

## Product intent and baseline

The user prioritizes fast startup, extensibility, and reliability over the old strict executable-size ceilings. Remaining pocket-sized and smaller than a comparable fx build is desirable; size alone must not force weaker error handling. This is a scoped migration to modern C++20, not a rewrite of the whole repository.

Planning baseline: `main` at `1d8ad71d66c06c726b3c5b35e367fec678031e85` (2026-09-16). Re-inspect current main and outstanding changes before implementation. Source paths below describe that baseline; no migration or performance improvement has been demonstrated yet.

## Dependencies and scope

Depends on **[#137](https://github.com/thehumanworks/tny/issues/137)** for mixed-language builds, ownership/allocator conventions, exception containment, quality discovery and performance tooling. Rebase on that delivered state before implementation.

Starting points:
- [Event ownership and queue](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/core/runtime.c) (`free_fields`, `event_copy`, reserve creation, queue accounting and teardown), `src/core/runtime.h`, and `src/core/events.h`.
- [Public handle adapters](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/lib/tny.c), `src/lib/error.h`, and the existing installed headers under `include/tny/`.
- [Custom-tool registry/calls](https://github.com/thehumanworks/tny/blob/1d8ad71d66c06c726b3c5b35e367fec678031e85/src/lib/custom_tools.c) and `src/lib/custom_tools.h`; narrowly affected host-service and provider callback adapters.
- Existing contracts: `docs/libtny.md`, ADRs 0023, 0030, 0033, 0037 and 0038.

Migrate the listed ownership responsibilities, extracting private C++ modules as appropriate. A wholesale rewrite of the runtime's extension/scheduling logic, session JSON model, public SDK APIs or all providers is excluded. No global shared-pointer graph, new worker pool, public C++ ABI, new embedding platform, or new provider feature.

## Implementation plan

1. Map the runtime -> session -> engine -> queued event graph and each async-call reference. For every boundary, record owner, borrower, allowed thread, lifetime end, callback reentrancy rule and transfer operation.
2. Introduce private owned-event records with owned strings/bytes, explicit immutable borrowed views for synchronous adapters, and automatic destruction. Keep addresses/views valid for the documented lifetime, including after container growth and moves. Do not cache `data()` pointers across moves/reallocation without rebuilding the view.
3. Convert queue nodes, pending terminal/error reserves and partial construction to those records. Preserve logical event-count/payload-byte accounting; separately measure actual allocation overhead. Preserve the allocation-free settlement path and transactional reserve replenishment.
4. Convert async tool registration/call ownership and mutex management. Use unique ownership for normal objects and shared ownership only for the real registry/async-worker lifetime. Keep generation/epoch checks and inactive/completed states as explicit logic: reference counting does not reject a stale completion.
5. Adapt C exports and internal consumers without changing exported names, frozen layouts, sized initialization, capability flags, event schema, or release functions. Preserve owner-thread and owner-process checks, cross-thread cancel, callback non-reentrancy, and inputs-copied-before-return semantics.
6. Exercise actual library and SDK clients across success/failure/cancel cycles. Check the first owned-event implementation independently before converting the async registry.

## Acceptance invariants

| ID | Observable requirement | Required proof |
| --- | --- | --- |
| P2-I1 | Enqueued events own every retained payload; destroying/mutating the callback document or input immediately after callback return cannot change a queued event. Event views stay valid until event release. | Tests covering every payload field, embedded lengths, empty values, event moves/queue growth and destruction after engine/session teardown where the current API permits it; ASan/UBSan; no separate per-field free table in converted ownership code. |
| P2-I2 | Queue admission, backpressure, suppression, cancellation and drain preserve exact ordering and exactly one terminal event for every successfully started turn. | Exact event traces through private runtime, public libtny, CLI JSONL and representative SDK callers; boundaries at event/byte caps; slow reader, repeated cancel, permission wait and transport death. |
| P2-I3 | All newly introduced allocations are fault-injected; constructor failure publishes no partial handle. Active OOM uses the reserved error/terminal pair without allocating or aborting the host. | Exhaustive discovered allocation-index sweeps, including C++ container/control-block allocations; OOM during reserve replenishment/error creation, two failed turns then a successful turn; no host stdout/stderr leakage. |
| P2-I4 | Completion after cancel/unregister/registry destruction, duplicate completion and wrong-generation completion obey existing status semantics without use-after-free, leaks or deadlock. | Real async worker fixtures, retained handle release, completion-vs-cancel/unregister/teardown stress in the documented supported concurrency envelope; Linux TSan and sanitizer-backed library tests. Unsupported concurrent session destruction must not be represented as newly supported. |
| P2-I5 | ABI, allocator ownership, thread/process affinity and callback rules remain unchanged for existing C, Python and Node consumers. | ABI baseline/export checks, sized-record canaries, wrong-thread/reentrancy/fork fixtures, clean-prefix installed C/C++ consumers and both SDK suites. No exceptions cross exports or callbacks; moved views remain correct. |
| P2-I6 | The shared startup gate passes and queue processing throughput/peak memory stay within 10% of the recorded baseline for a fixed event/callback workload. | Repeated turn/event/callback benchmark, allocations per event and peak queue memory, startup/TTFT/size/dependency report; explain measured trade-offs. |

## Required tests and faults

Run `make test`, `make quality`, host leak gates, `make test-abi`, `make test-sdks`, `make test-libtny-fault`, `make test-libtny-fault-sanitize`, `make test-libtny-fuzz-smoke`, Linux `make test-libtny-tsan`, and Linux `make test-libtny-fuzz`. Update and run relevant `make test-libtny-mutation` cases after moves/extractions.

Extend `tests/test_runtime.c`, `tests/integration/test_libtny.py`, `test_libtny_custom_tools.py`, `test_libtny_faults.py`, their C/C++ fixture hosts, and SDK tests rather than only testing the new helper classes. Retain native platform and wasm runtime regression lanes from phase 1.

Critical mutations: replace a retained copy with a borrowed view; omit a generation check; release the async reference early; omit queue-byte accounting; emit a second terminal event or allocate during reserved OOM settlement. A test must fail for the intended behavior, not merely fail to compile.

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
