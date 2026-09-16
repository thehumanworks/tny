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

## Continuation checks — 2026-09-16 04:27 UTC

Source started at ed355a9 plus preserved dirty issue changes. See
[continuation contract](continuation-20260916.md). No completion claim.

- Independent read-only review `/root/phase1_review`: no confirmed new
  memory-safety defect in converted owners. Found inherited search OOM
  misclassification, missing direct cancellation-release proof, and missing
  callback-error regression. These remain pending. Review approved unique
  mutation anchors and Nix ownership-target inclusion before implementation.
- Parser mutation first run failed infrastructure (two borrowed-view anchors);
  review found the same problem in the swallowed-OOM anchor. Both are now
  unique. Second run compiled all four mutants; frame limit, ID precedence,
  dangling document view and swallowed OOM were behaviorally killed. The
  unmodified instrumented baseline passed. Report retained below.
- Initial ABI suite: 41/43 passed, two toolkit fixture link failures because
  they used CC to link C++ objects. Keep C fixture compilation under CC and
  query CXX separately for linkage. Both regressions now pass (2 tests,
  13.986 seconds). Full ABI rerun pending; SDK/fault/fuzz gates running.
- Native quality and full make test are running; no results yet.
- Active independent worktrees are not modified. Phases 2/3 remain unintegrated.

### Parser continuation review and measurements

- `/root/cancel_design` independently found that terminal cleanup already
  frees the owners: cancellation *during* parsing could free active SSE state.
  The first reviewer retracted the earlier retention-only diagnosis.
- New callback-cancel fixture failed before the repair on duplicate terminal
  delivery. After deferring cleanup through feed/flush/whole-document decode,
  all 27 OpenAI unit tests pass (11,334 assertions). Callback bytes stay valid,
  no terminal is emitted inside the callback, later thinking is suppressed,
  exactly one interrupted terminal appears, and the same backend handles a
  later turn. Terminal usage callbacks can cancel once without recursion.
- Callback OOM now has a multi-event regression: first callback rejects, later
  callbacks stop, DONE preserves sticky error, reset permits success.
- Search decoder fault fixture reproduced wrong classification at allocation
  1. The repaired decoder passes all 10 discovered allocation failures,
  malformed-input distinction and later successful decode, under ASan/UBSan.
- Initial parser comparison failed tools/whole 1.134x, tools/byte 1.582x and
  tools/split 1.239x. A reviewed single-slot view refresh improved them, but
  the second run still failed 1.330x and 1.102x for byte/split. Replacing
  repeated string-length comparisons with strcmp preserves the C ID semantics.
  Third run passes all nine throughput and RSS comparisons (2,000 iterations,
  three paired batches). Tools ratios: 1.004x, 1.083x, 0.987x; maximum RSS
  ratio across workloads 1.022x. All raw comparison reports are retained.
- All four parser mutants still compile and fail behaviorally after the
  optimization. No production mutation remains.
- Intermediate make quality, test-sdks, test-libtny-fault,
  test-libtny-fault-sanitize, test-libtny-fuzz-smoke and test-parser-fuzz-smoke
  exited 0. Later edits invalidate dependent final claims; reruns remain due.
- Frozen phase-1 platform tar SHA256:
  6fb9b22f733e8427aa745b68c5b973c6718f2b7f8a6b6abd967741c19926c70e.
  Linux arm64 and wasm/browser runs are in progress, not counted as passes.

### Latest phase-one checkpoint — 2026-09-16 04:50 UTC

- `make quality` third continuation run: exit 0, C/C++ formatting, analysis,
  strict diagnostics, Python/shell/workflow/JS gates. The second run encountered
  an intermediate allocator edit and is invalid as final evidence; it failed
  closed and was rerun after edits stopped. GCC analyzer is Linux-only.
- `make test-parser-backend-ownership test-parser-mutation test-search-ownership`:
  exit 0. 27 backend tests, actual before-teardown C++ live-allocation
  assertions for three decode forms, four behavioral mutant kills, and ten
  search allocation failures plus malformed JSON/recovery.
- `make test-abi` rerun: exit 0, 43 tests and both ABI baseline comparisons.
- `make leaks`: exit 0, zero leaks on supported macOS suites and CLI checks;
  the existing process-spawning suite exclusions still need Linux valgrind.
- Frozen phase-one wasm run: exit 0, all configured node fixtures, unchanged
  size gate and actual Playwright Chromium browser smoke. This is the earlier
  source snapshot listed above, before test-only live counters were added.
- Startup comparison passes: version 4.042 -> 3.936 ms, help 4.014 -> 3.983 ms,
  first prompt 3.892 -> 3.942 ms (102 CLI observations and 20 PTY launches
  per binary in the prescribed three-batch comparison; raw samples retained).
- TTFT, 20 local-mock runs each: TUI 541.8 -> 538.9 ms; stdin 1112.1 ->
  1119.7 ms. All within the ten-percent investigation threshold.
- macOS stripped binary: 1,086,288 -> 1,087,440 bytes (+1,152). Candidate
  adds /usr/lib/libc++.1.dylib alongside libSystem. Startup candidate source
  tar SHA256 5076a0efe40c067ae308be83aabdffdb9fda867396aae9d3b8287a215ea036d3.
- Full root native test and Linux arm64 frozen-snapshot suite remain running.
  Hosted x86-64, Windows, Nix and Linux fuzz/TSan gates are not claimed.
- No new commit or remote push has been performed in this continuation.
  User clarification is pending on early phase-one publication required for
  the issue's hosted-before-next-phase order versus final-series publication.

## Resumed delivery — 2026-09-16 (phases 2 and 3 integrated)

Source: `feat/cpp-ownership-137-139` at ed355a9 plus the inherited phase-1
continuation, with local-main phase-2 fixes (f7b2b70, e56d01e, 58e92bf, 4af1c02)
and phase-3 commits (aa2c68a, 7258d70, 6b76784, 99445e4) ported onto the newer
parser stack. The parser/decoder sources were not replaced; owner helpers stay
`src/util/ownership.hpp` and `src/json/ownership.hpp`. New ADRs 0116, 0117 and
0116; root ADR 0114/0115 unchanged. Per-phase records: [phase 2](../cpp-phase-2/evidence.md),
[phase 3](../cpp-phase-3/evidence.md). Raw logs: `/private/tmp/tny-finish-137-139-20260916/`.

| Gate | Result |
| --- | --- |
| make (release, macOS arm64) | exit 0; stripped `build/tny` 1,189,456 bytes (pre-series baseline 1,086,288) |
| make test-runtime-ownership / test-parser-backend-ownership | 38 passed / 27 passed + 1 skipped (needs fully instrumented host) |
| build/lib-fault/provider-faults | 98 tests, 17,342 assertions passed |
| make test-libtny-fault | exit 0; provider whole-turn sweeps openai=308, openai-chat=204, cursor=162, acp=95, acp-ws=92 |
| make test-runtime-mutation | 13/13 mutants killed |
| make test-runner-ownership | exit 0 (real fd/pipe/lock loops, host faults, cleanup holds, checkpoint flags) |
| make test-runner-mutation | 7/7 mutants killed |
| make test-unit (ASan/UBSan) | 567 tests, 32,019 assertions passed |
