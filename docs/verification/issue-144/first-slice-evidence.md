# Issue #144 first-slice evidence

Date: 2026-09-17. **Full contract: INCOMPLETE. Stop for independent first-slice review.**
The supervising agent owns review, further implementation and delivery. No
subagents, branch changes, commits, pushes or other worktree edits were made.
`contract.initial.md` is unchanged and remains the complete acceptance scope.

Baseline: `08b07a01f41586c3509555bfb12a61a72ab8f5e5`, initially clean,
`feat/cpp-native-request-144`. The initial contract and historical ADR bytes were
hashed before implementation and verified unchanged afterward. Only ADR 0127
was added; historical duplicate serials remain unchanged. Local `main` lacks
0126; 0127 avoids the supervisor-identified unmerged #142 reservation.

Host: macOS 27.0, Apple Silicon, Apple clang 21.0.0
(`clang-2100.3.27.1`), repository flags, C11 + private C++20.
Logs/scripts/manifest: `/Users/tomas/.cache/tny-issue-144/`.
Tested input manifest: `first-slice-tested-inputs.sha256`, 682 files, SHA256
`f898568517023b307b6b93c6f5f5b803edfd929ce6c0d5f42c3705c60edd9e63`.
It includes source, public headers, vendored dependencies, tests, build and
CI/Nix inputs. Documentation is described separately in the handoff.

## Checks run

| Exact command | Result and boundary |
| --- | --- |
| `make -j6 test-native-request-ownership test-parser-backend-ownership release` | PASS at initial slice state. Owner discovery: 9 indices. Partially instrumented backend suite: 27 pass, one expected skip requiring the full fault object graph. Later full fault run below covers that regression. |
| `make -j6 test-native-request-ownership build/lib-fault-san/provider-faults release` | PASS after guarding failed reopen before callbacks. |
| `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 build/lib-fault-san/provider-faults -s openai_suite` | PASS, 28 tests, 11,454 assertions, zero skips. Includes real C provider request-construction OOM after recorded usage, reserved terminal delivery and recovery. |
| `make -j6 test-runtime-ownership test-cpp-build` | PASS: runtime 40 tests / 5,074 assertions. Build checks: 11 run, 10 passed, one explicitly skipped Emscripten lane. |
| `make -j4 tidy warn-strict TIDY_C_SRC=src/backends/openai/openai.c TIDY_CPP_SRC=src/backends/openai/request_owner.cpp` | PASS, changed production C/C++ units; not full repository `make quality`. |
| `make -j4 test-native-request-ownership` | PASS on final fixture after adding failed-reopen and 32 successful request/release cycles. ASan/UBSan, all 9 setup indices, wipe-before-free, stable resend and no-allocation settlement. |
| `make -j4 BUILD=build/issue-144-nosan SANITIZE=0 test-native-request-ownership` | PASS with faults enabled and sanitizers disabled. |
| `leaks --atExit -- build/issue-144-nosan/native-request/ownership-test` | PASS: 0 leaks / 0 leaked bytes. Deterministic transport fixture, not a whole runtime resource-count measurement. |
| `python3 /Users/tomas/.cache/tny-issue-144/run-focused-openai.py` | PASS: existing real CLI/loopback Responses and Chat recovery, partial-answer continuation/interruption, broken-session repair, null-error chunks and reasoning passthrough. Wrapper supplies three files expected by the fixture. |
| `python3 /Users/tomas/.cache/tny-issue-144/run-owner-mutants.py` | PASS: three private-copy mutants compiled, then failed runtime oracles: omitted connection reset, omitted auth wipe, freed body before resend. Source tree was never mutated. |
| `make test-native-request-ownership` | PASS final unmutated run after mutant checks. |
| `clang-format --dry-run --Werror src/backends/openai/openai.c src/backends/openai/request_owner.cpp src/backends/openai/request_owner.h tests/fixtures/native_request_ownership.cpp` | PASS. |
| `actionlint .github/workflows/ci.yml` and `git diff --check` | PASS. Nix executable unavailable; inventory inspected, evaluation not run. |
| `wc -c build/tny`; `otool -L build/tny` | Stripped local release: 1,189,472 bytes; dependencies libc++.1.dylib and libSystem.B.dylib. No speedup or cross-platform size claim. |

## Failed exploratory runs retained

The complete `python3 tests/integration/test_openai.py` fails at line 750:
after the tool-profile cases, ordinary `list_files`/`glob_files` have error
statuses (`unknown tool`). Repeated with `env -u TNY_TOOLS` and explicit
`TNY_TOOLS=all`; same failure. The environment initially sets `TNY_TOOLS=terminal`,
but removing it alone does not resolve the failure.

A differential baseline linked the pre-change `HEAD:src/backends/openai/openai.c`
into the same existing release object graph, replacing only its object, with
output `first-slice-baseline-tny`. The new owner TU is unreferenced by that C
backend. Running `TNY=/Users/tomas/.cache/tny-issue-144/first-slice-baseline-tny
TNY_TOOLS=all python3 tests/integration/test_openai.py` reproduces the same line
750 failure. This is a same-object-graph comparison, not a separately rebuilt
clean checkout. Logs and exact compiler/link inputs are retained in the cache.
The full fixture remains a shared blocker; it is not reported as passed.

The first isolated wrapper created one workspace file, while interruption
assertions require exactly three. It correctly failed that content assertion;
the wrapper was corrected to the existing fixture's three-file setup and all
isolated cases then passed. No product/test oracle was weakened.

## Contract reconciliation at this stop

- V01/V02: request/document/header/connection ownership and release-only
  destruction implemented; inventory records transfer and borrow boundaries.
  Pending records and retained turn buffers are still in C.
- V03/V05: new owner setup sweep and existing real runtime OOM regression pass;
  full request-builder/public admission/pending-transfer exhaustive sweeps remain.
- V04: focused native Responses/Chat retry/continuation fixtures pass; full
  fixture has the shared failure above; complete wait-state/cancel/restart matrix
  remains required.
- V06: local ASan/UBSan, owner leaks and three runtime mutants pass. Real whole
  runtime fd/heap accounting and pending lifetime/replay mutants remain.
- V07: focused checks and Make/Nix/CI inventory updates are present. Full test,
  quality, ABI, wasm, hosted platforms and Nix execution are not claimed.
- V08: local artifact size/dependencies recorded only. Same-host baseline and
  candidate startup/TTFT/request/memory/allocation benchmark matrix remains.
- V09: independent first-slice and final reviews remain with the supervisor.
- V10: scoped ADR, documentation and tested hashes are available. Commit/push/PR
  delivery was explicitly excluded from this assignment and remains outstanding.

Important review points: document retention through the POST extends peak
lifetime; request/config header borrows depend on the existing synchronous,
non-reentrant callback contract; constructor admission gains one injected
connection-handle allocation, and each POST gains one request aggregate.
Do not repeat this pattern for pending records until transfer atomicity,
retained results and explicit async generation invalidation are reviewed.

## Supervisor isolation correction

The complete first-slice `test_openai.py` passed with the same fully allowlisted
environment as the untouched baseline. Removing only `TNY_TOOLS` was insufficient
to remove all inherited user overrides. No application behavior change was needed.
`first-slice-provider-isolated`: exit 0, 50.62 seconds, all assertions passed;
source unchanged during the run, source-manifest SHA-256
`6d9084ac87c322607c2d12faae83f7b2ffc46e0d74de284c3d368887a7ade909`.
The prior partially sanitized failures are superseded, not acceptance failures.

The independent first-slice report is in `reviews/first-slice-fable.md`. Its F2
correctly narrows the earlier OOM claim: the old fixed-offset body test now targets
the new request aggregate, so a discovered full construction sweep is required.
F1–F3 are required fixes before the remaining ownership slice.
