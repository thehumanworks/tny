# Issue #144 implementation evidence

Overall acceptance: **INCOMPLETE — supervisor handoff**. Implementation and
focused checks are complete; the immutable V01–V10 scope has not been reduced.
The supervisor owns frozen full gates, final review, performance, wasm and PR.

Branch `feat/cpp-native-request-144`; HEAD
`08b07a01f41586c3509555bfb12a61a72ab8f5e5` (initial contract commit only).
Base `4e760be09908d61e23029486e1c9a3b91fbfcbbe`. No branch switch, commit, push,
merge, worktree operation, subagent/reviewer or live model request was performed.
Initial contract SHA-256:
`9340402ca22d57e4434748520b8b21825f4cb626c30db50c6f538a62a3761bed`;
bytes match commit `08b07a0`. All 132 historical ADR files match the base.
Public headers/ABI files are unchanged. #142 remains untouched.

## Current focused proof

All commands use `python3 /Users/tomas/.cache/tny-issue-144/run-gate.py NAME
"$PWD" COMMAND`, from `/Users/tomas/projects/tny`. The helper allowlists the
environment and logs command, exit, source hashes and timestamps. No user
configuration/secrets are edited. `TNY_VERSION=1.0.0` is pinned in builds.

The following completed runs all report unchanged source during execution and
identical source/test/configuration manifest SHA-256:
`b18b0d5ef108bdd4fd9081cac00611b84fb45884770424f43181d8cca45f578a`.
The cache's per-run JSON contains every individual file hash, including new
untracked source. Documentation is outside the helper's source manifest.

| Gate / cache log and JSON stem | Result | Command |
| --- | --- | --- |
| `native-focused-sanitize` | exit 0 | `make -j4 TNY_VERSION=1.0.0 BUILD=build-issue144-san SANITIZE=1 test-native-mutation` |
| `native-focused-nosan` | exit 0 | `make -j4 TNY_VERSION=1.0.0 BUILD=build-issue144-focus SANITIZE=0 test-native-mutation` |
| `native-changed-quality2` | exit 0 | `make TNY_VERSION=1.0.0 BUILD=build-issue144-quality SANITIZE=0 tidy warn-strict TIDY_SRC="src/backends/openai/openai.c src/backends/openai/request_owner.cpp src/backends/openai/turn_owner.cpp src/backends/openai/responses.cpp src/core/tools.c" && actionlint .github/workflows/ci.yml && ruff check tests/mutation/native_ownership.py tests/mutation/mutate.py tests/bench/bench_requests.py && clang-format --dry-run --Werror src/backends/openai/openai.c src/backends/openai/openai.h src/backends/openai/request_owner.cpp src/backends/openai/request_owner.h src/backends/openai/turn_owner.cpp src/backends/openai/turn_owner.h src/backends/openai/responses.cpp src/core/tools.c src/core/tools.h tests/test_openai.c tests/fixtures/native_request_ownership.cpp tests/bench/bench_requests.c` |
| `native-integrity` | exit 0 | `Python byte comparisons of branch/HEAD, initial contract, historical ADRs, public headers/ABI and benchmark trio; `git diff --check` (full command in JSON)` |

- Both sanitizer and nonsanitizer runtime runs: **33 tests, 18,763 assertions,
  zero failures/skips**. The owner fixture discovers **17 allocation indices**
  across request aggregate, scratch, JSON, body/auth/headers/path and transport.
- Runtime construction discovers the complete post-save/post-open construction
  range through provider request control on both wires, including structured
  output; sweeps each index. Separate real HTTP/parser failures and prior-usage
  tool/steer rounds assert one reserved ERROR/TURN_END pair, no settlement
  allocations, no unsubmitted POST and identical persisted session bytes.
- Pending modes discover **5 custom admission, 7 permission admission, 5
  permission-to-custom transfer and 33 result-consumption indices** on each
  wire. All injected indices must actually inject; each failure recovers on the
  same engine. Eight direct transfer/reset cycles also restore owner baselines.
- **Nine mutants compiled/linked and were killed by behavioral oracles** in both
  sanitizer modes: prepare-once, connection-reset, auth-wipe, view-release,
  move-source, transfer-before-copy, pending-lifetime, cancel-authority and
  generation-replay. Sources/objects/baselines stayed unchanged during runs.
  Reports: `build-issue144-san/native-mutations/run-huv3bq3z/report.json` and
  `build-issue144-focus/native-mutations/run-4j6x463x/report.json`.
- Changed production C/C++ passes clang-tidy and strict warning checks; changed
  C/C++/benchmark formatting, Python Ruff checks and CI actionlint pass.
- `leaks --atExit` stalled on this host. The task-owned probe was stopped;
  `native-final-focused` is **exit 2**, not leak evidence. Mac ASan disables LSan;
  aggregate counters exclude ordinary C/JSON allocations. Linux LSan/valgrind
  and a completed host leak run remain required acceptance work.

## Behavioral coverage and full-fixture map

| Required behavior | Actual named oracle / current status |
| --- | --- |
| Entire request-owner construction/reset/wipe/reuse | `tests/fixtures/native_request_ownership.cpp`, current PASS |
| Backend/request aggregate constructor failures | `native_request_constructor_allocation_sweep`, current PASS |
| Real request construction after earlier usage, HTTP body construction, parser and steer round OOM | `request_construction_oom_after_usage_skips_finalization`, current PASS, both wires, structured output |
| Permission allow, denial, permission cancel, successful custom transfer, transfer OOM, queued result cancellation/late completion, provider recovery | `native_pending_lifecycle_and_allocation_sweeps`, current PASS, both wires |
| Source preserved on failed copy, source emptied only after success, metadata lifetime, retained buffers and repeated empty resets | `native_pending_transfer_preserves_source_on_failure`, current PASS |
| Write-side stale keep-alive, path/auth preserved, attempt increments once, second-control stop sends no bytes | `native_request_real_stale_replay_and_control_stop`, current PASS on Darwin; POSIX fixture skips Windows |
| Cancel ST_HEADERS, non-SSE ST_BODY, partial error-body wait and ST_RETRY_WAIT, then same-engine reuse | `native_cancel_headers_raw_error_and_retry_waits`, current PASS, both wires; POSIX fixture skips Windows |
| Reentrant decode cancellation, borrowed text valid until callback unwinds, later turn reuse | `cancellation_inside_decode_preserves_callback_and_reuse`, current PASS |
| Byte-split provider frames, Responses reasoning retained fields | `provider_decoding_every_split`, `responses_reasoning_owns_unknown_fields`, current PASS |
| Continuation ordering and deadline parsing | `continuation_trails_partial_then_user_turn`, `stall_window_parses_and_clamps`, current PASS |
| Full Chat/Responses request/response fixture success | `tests/integration/test_openai.py::main`; first-slice isolated full run PASS, final candidate run pending supervisor |
| Retries before output, transient status/error bodies, retry-after and bounded retry budget | `test_openai.py::check_stream_recovery`, both wires; final candidate pending supervisor |
| Retries after partial text and tool arguments; clean/abort/stall, exactly-once continuation | `test_openai.py::check_stream_interruption`, both wires; final candidate pending supervisor |
| Null chunks, reasoning continuity, broken-session repair | `check_error_null_chunks`, `check_reasoning_passthrough`, `check_broken_session_repair`; final candidate pending supervisor |
| Affinity/header and wire assertions | `test_openai.py::main`, `tests/integration/test_codex_chatgpt.py`; final candidate pending supervisor |
| Full active-turn and public API fault scopes / pending reserved OOM scenarios | existing `test-libtny-fault`, `test-libtny-fault-sanitize`, `test_libtny_faults.py::provider_turn_sweeps` and `reserved_settlement_fixture(text,permission,custom,later)`; reused complete graph for focused tests, full sweep pending supervisor |
| Detach/stop/recovery and no orphans | `test_background.py::main` labels `stop -> interrupted, lock free`, `detach hygiene`, `no orphans`; untouched-baseline full run PASS, final candidate pending |
| Checkpoint consumed index, exactly-once restart/recovery, denial of replayed checkpoint | `test_background_agents.py::run_case`, including `restart_fault="post-go"`, `"post-run"`, permission/post-run; `cancellation_before_boundary`; final candidate pending supervisor |
| Saturated streams, cancellation, hard-stop paths | `test_interrupt.py::main` cases `flood-ctrl-c`, `responses-ctrl-c`, `in-process-ctrl-c`, frozen/CLI/hangup cases; final candidate pending supervisor |

## Scope reconciliation

| Invariant | Current evidence / acceptance state |
| --- | --- |
| V01 | All inventoried owners implemented; focused lifetime/transfer/reset tests PASS. Final independent review pending. |
| V02 | Resource-only destruction; C invalidation and callback deferral; focused mutants PASS. Final review pending. |
| V03 | Injectable owners, caught throwing facades, reserved OOM pair and zero settlement allocations in focused real runtime PASS. Full fault graph execution pending. |
| V04 | Named current/new and existing fixture map above; full provider/restart runs pending on frozen source. |
| V05 | Discovered request, constructor, admission/transfer/consumption ranges PASS; complete public/provider sweeps remain full gate. |
| V06 | ASan/UBSan, nonsanitized lifecycle/resource baselines and nine maintained mutants PASS; host leak gate incomplete. |
| V07 | Changed-unit quality and CI/Nix integration complete; full make test/quality/provider/parser/runtime/custom/fault/ABI, wasm, hosted lanes and Nix execution pending. |
| V08 | Optional benchmark trio copied unchanged and integrated in source inventory. No candidate benchmark, TTFT/startup/RSS/size/runtime-dependency result claimed. Supervisor measurements pending. |
| V09 | Both actual first-slice reports read; every finding dispositioned in reviews.md. Final independent review pending. |
| V10 | Final scoped draft ADR/docs/inventory/contract/evidence and cache summary supplied; supervisor owns commit/push/PR. |

## Historical failures and corrections

The first-slice isolated full provider run `first-slice-provider-isolated` and
untouched-baseline `baseline-provider-clean` both passed; the supervisor also
confirmed baseline background success. The earlier partial-env diagnosis is
superseded. See [first-slice evidence](first-slice-evidence.md).

Development failures were retained, not counted as passes: a broadened CLI-mode
probe reached the unchanged `skills_discover` null-home OOM path; runtime tests
were corrected to actual embedding configuration (see reviews.md); initially
incorrect expected denial status and overly broad peer-thread fault scope were
corrected; a real second-control stop error was fixed in C; mutation harness
object naming, source anchors and an unused-parameter mutant compile failure
were corrected and every final mutant rebuilt; initial benchmark tidy used
production-only flags without its required allocator-test define and failed;
changed production quality and the unchanged externally validated benchmark are
reported separately. The initial Darwin sanitizer symbolization/debugger probes
stalled and were stopped, then a symbolization-disabled run identified the null
home call. Final supported sanitizer runs passed normally.

All exact development commands, exit statuses, hash manifests and logs are
listed in `/Users/tomas/.cache/tny-issue-144/implementation-summary.md` and their
adjacent per-run JSON files. The summary is an implementation handoff, not a
whole-issue delivery claim.
