# C++ series evidence

Contract: [contract.md](contract.md)
State: INCOMPLETE. Baseline captured before implementation; all implementation gates pending.
Baseline: 1d8ad71d66c06c726b3c5b35e367fec678031e85. Primary main remains fb232e8; integration starts from current origin/main.
Platform: macOS arm64, Apple clang 21.0.0, Python 3.14.7, Node 26.8.2.
Discovery: GitHub issues 137/138/139 open and saved into full initial contracts. No native goal; tool authorization takes precedence over skill default. Docker CLI exists but default daemon absent; remote/hosted platform inventory pending.
Reviews, delegation, gate results, merge and cleanup: pending.

## Current execution checkpoint — 2026-09-16

- Requested checkout started clean at `fb232e8002b1ba18bfdf12ed723146e0ac1295ab`;
  GitHub main was independently fetched as `1d8ad71d66c06c726b3c5b35e367fec678031e85`.
- Reused document-only contracts from `c6d938a0846e0cabd7235b56468abc5cf65cfaf5`
  without changing initial snapshots. Current branch: `feat/cpp-ownership-137-139`.
- Language-policy/authority amendment committed as `edbd6e7`; no production
  migration was present in that commit. Previous worktrees and live sessions
  were left untouched.
- Clean reference worktree: `/private/tmp/tny-cpp-delivery-20260916/baseline`.
  Apple clang 21.0.0, Python 3.14.7, Node 26.8.2; native baseline release
  1,086,288 bytes, SHA-256
  `34bb033a40129e937bbc4ab68a1d938adee6553861f5f2acc2b822d0a35fee51`,
  with only `/usr/lib/libSystem.B.dylib` listed by `otool -L`.
- The first baseline suite inherited the host's `TNY_TOOLS` restriction;
  expected fixture filesystem/subagent operations failed. That run is retained
  as contaminated-environment evidence, not a migration regression. A clean
  rerun unsets the host override. Other fixture-authorized tool restrictions
  remain test-controlled. Final status is not yet established.
- Startup tooling: seven deterministic contract tests pass; real baseline
  version/help/PTY smoke completes. Smoke timings were taken under active
  tests and are not performance acceptance measurements.
- Implementation and final integrated/platform/performance gates remain
  pending. No independent review has yet been invoked.

## Baseline and instrumentation checkpoint — 2026-09-16

- Clean `make test` rerun finished nonzero with exactly one suite failure:
  OpenAI Responses reset-socket fixture expected an abort diagnostic but
  observed the existing one-second stall diagnostic. All other listed
  integration suites completed successfully. That failure also occurred in
  the isolated build helper's unchanged-C tests; investigate/reproduce before
  attributing it to ownership migration. Raw log: task `baseline-clean.log`.
- Parser benchmark smoke compiled both copies from identical baseline sources
  and all nine output/checksum oracles agreed. At only 20 iterations during
  concurrent builds, one timing ratio exceeded 10%; this is retained as a
  failed performance-gate run, not counted as acceptance.
- Actual engine event benchmark baseline-vs-baseline two-iteration smoke
  passed all six executions (three paired batches), including retained binary
  payloads and exactly-once terminal settlement. Its timing is functional
  smoke only, not the required 2,000-iteration quiet-host comparison.
- Baseline phase-specific runtime/platform results and environment limitations
  are retained in task `platform/RESULT.md` and `platform/inventory.json`.
  They are not evidence that unintegrated migration code passes those lanes.
