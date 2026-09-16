# Evidence: C++ phase 2

Contract: [contract.md](contract.md)
State: implemented on `feat/cpp-ownership-137-139`; local native gates recorded
below on the integrated (phase 1 + 2 + 3) source. Hosted platform, Linux TSan,
Linux libFuzzer and Nix results are supplied by the pull request, not claimed here.
Baseline: 1d8ad71d66c06c726b3c5b35e367fec678031e85 (pre-series); the
integrated candidate is the final working tree recorded in the series evidence
manifest. Initial contract bytes retained in contract.initial.md.
Native goal: unavailable by authorization (ordinary task, no explicit goal request).

## Implementation record — 2026-09-16

- Ownership inventory: [ownership.md](ownership.md). Decisions:
  [ADR 0116](../../adr/0116-runtime-event-and-async-ownership.md) (owned events,
  async registration/call lifetimes) and
  [ADR 0117](../../adr/0117-allocation-free-provider-oom-settlement.md)
  (allocation-free provider settlement and its bounded exceptions).
- `src/core/owned_event.cpp` owns retained event payloads in one allocator-backed
  vector; `src/core/runtime.c` keeps queue policy, reserves and settlement in C
  and no longer carries a per-field free table. `src/lib/custom_tools.cpp`
  replaces `custom_tools.c` with unique registrations, a shared call/registry
  state limited to the async worker, one scoped mutex and explicit
  generation/epoch checks. `src/core/tools.c` consumes the pending provider
  handle on take, invalidate and generic call cleanup.
- Providers mark OOM at their quiescent boundaries (`tny_alloc_provider_failed`)
  and stop constructing owners, callbacks or persistence: OpenAI request
  construction returns a distinct status and parser OOM routes through the
  emergency cancel; Cursor closes streams/callbacks/bridge without RPCs and
  unlinks consumed leases before serialization; ACP closes its transport with
  bounded TERM/KILL escalation; WebSocket/TLS close without allocating.
  The runtime keeps the pending terminal private until finalization succeeds.
- Owner helpers are `src/util/ownership.hpp` and `src/json/ownership.hpp`; no
  second allocation-owner system was introduced. Test-only process-wide owner
  counters (`tny_alloc_test_owned_live`) observe C++ container/control-block
  allocations in every instrumented binary.

## Execution records (macOS arm64, Apple clang, integrated working tree)

Logs live under `/private/tmp/tny-finish-137-139-20260916/`.

| Check | Result | Log |
| --- | --- | --- |
| `make test-runtime-ownership` (ASan/UBSan, instrumented runtime + owners) | 38 passed | runtime-ownership-p2.log |
| `make test-parser-backend-ownership` | 27 passed, 1 skipped (request-construction test needs the fully instrumented host below) | runtime-ownership-p2.log |
| `build/lib-fault/provider-faults` (real ACP/Cursor/OpenAI backends, injected object graph) | 98 tests passed, 17,342 assertions; exhaustive callback/store/thread/recovery sweeps | provider-faults-run-p2.log |
| `make test-libtny-fault` (public API sweeps, reserved settlement text/permission/custom/later, provider whole-turn sweeps) | passed; openai=308, openai-chat=204, cursor=162, acp=95, acp-ws=92 indices | libtny-fault-p2.log |
| `make test-runtime-mutation` | 13/13 behavioral mutants killed (see below) | runtime-mutation-p2.log |

Later integrated gates (unit suite, quality, full `make test`, ABI/SDK,
sanitizer lane, leaks, fuzz smoke, benchmarks) are recorded once in the
[series evidence](../cpp-series/evidence.md).

## Mutation results

`tests/mutation/runtime_critical.py`, private copies only, production hashes
verified unchanged after each mutant:

| Mutant | Oracle | Killed |
| --- | --- | --- |
| borrowed-payload (retain callback pointer) | ASan heap-use-after-free | yes |
| wrong-generation (omit generation check) | leases test FAIL | yes |
| early-async-release (non-owning host state) | ASan heap-use-after-free | yes |
| queue-byte-accounting (omit byte accounting) | byte-limit test FAIL | yes |
| second-terminal (duplicate terminal) | duplicate-terminal test FAIL | yes |
| allocating-settlement (allocate in reserved settlement) | settlement counter FAIL | yes |
| provider-allocating-settlement (allocate in emergency cancel) | "allocation during reserved OOM settlement" | yes |
| parser-ordinary-finalization (persist + ordinary turn end after parser OOM) | request_construction_oom counter | yes |
| decoder-observe-recovery (allocate after Cursor decoder OOM) | settlement allocations != 0 | yes |
| acp-block-before-kill (omit KILL escalation) | emergency cancel exceeds 2 s | yes |
| request-oom-persists-usage (session_save after request OOM) | request_construction_oom counter | yes |
| sdk-error-oom-fallback (allocate after SDK error OOM) | error_decode_oom counter | yes |
| acp-parser-oom-next-line (allocate after ACP message OOM) | message_oom counter | yes |

## Invariant mapping

- P2-I1: owned_event copy semantics; `runtime_all_payloads_survive_queue_transfer_and_teardown`, `runtime_owned_event_allocation_sweep`; borrowed-payload mutant.
- P2-I2: runtime unit traces, second-terminal and queue-byte mutants, reserved settlement fixtures through the public library.
- P2-I3: exhaustive public and provider whole-turn sweeps, zero settlement allocations, two failed turns then success (`text`, `permission`, `custom`, `later`), provider-faults host regressions.
- P2-I4: `runtime_async_leases_survive_all_invalidation_orders`, `runtime_async_pending_call_free_releases_owner`, custom-tool C and C++ worker fixtures (including completion OOM); Linux TSan remains a hosted gate.
- P2-I5: ABI/SDK suites recorded in the series evidence; no exported name, layout or callback rule changed.
- P2-I6: startup/size/dependency and event-throughput comparisons recorded in the series evidence.

## Reviews

The single independent review is coordinator-owned after implementation; findings
and dispositions are recorded in the series evidence when available.
