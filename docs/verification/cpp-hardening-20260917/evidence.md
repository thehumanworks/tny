# C++ transaction ownership: delivery evidence

## Delivered scope and revision

The production continuation is published in **PR141**, branch
`fix/cpp-ownership-finalization`, source revision
`cb0f74cafa34723af4c650f260100c70370cd951`. It follows the already merged
three-phase ownership migration (PR140; issues137,138,139).

This supplementary verification branch incorporates that exact published code,
build configuration and tests. Outside this evidence directory, its diff against
cb0f74c is empty. A concurrent task owns the primary checkout and combined PR;
all further verification here used an isolated, task-owned worktree. No primary
branch reset, force-push, main merge or unrelated process cleanup was performed.

The transaction slice replaces manual path/JSON lifetime management with the
existing typed unique_ptr aliases. The document and path are destroyed before
the state lock. Destruction never commits. Explicit commit, C interfaces,
cleanup holds, cancellation and process ownership remain intact. Directory OOM
uses the injected allocator, returns ENOMEM with a clear message and performs
no subsequent record loading. ADR0125 records the combined decision; ADR0124
records the accompanying analyzer and build-input repairs.

The strict size cap remains **less than6,000,000 decimal bytes** under ADR0121.
No obsolete one-MiB ceiling, removed check, disabled warning or new dependency
was used to force acceptance.

## Review and failure proof

A fresh Fable medium-effort reviewer approved the transaction representation
and independently built and ran the real resource fixture. Its returned report
and each finding's disposition are preserved in independent-review.md and
review-disposition.md. The combined finalization task also reviewed its broader
integration; this report does not substitute one review for that different scope.

The final fixture checks32 abandoned/exception-unwound transaction lifetimes,
six observed directory/JSON allocation failures, four actual persistence syscall
failures, idempotent allocation-free reset and reuse, disk-byte preservation,
a continuously held lock during writes and explicit successful commit. All
nine runner/transaction behavioral mutants are caught; compilation failures
are not counted as kills.

A separate non-forking macOS `leaks --atExit` invocation exercises the maintained
transaction test: unmodified code has **zero leaked bytes**. Deliberately leaking
only the loader's immutable document passes ordinary behavior assertions but
fails the leak check with88 leaks/25,408bytes. The negative control closes the
reviewer's original ASan-only proof gap. No mutant was installed in production.
Raw report hashes, compiler/source/binary identities and the reproduction
harness are preserved under artifacts/transaction-leak-*.

## Local verification

All listed completed commands returned0. Full command lines, timestamps,
source hashes, historical failed attempts and exit statuses are in
artifacts/gate-results.json; per-gate summaries do not replace those identities.

| Verification | Evidence |
|---|---|
| Fresh code matching PR141: release and strict size guard | published-artifact-build |
| Fresh code matching PR141: format, Clang analysis, strict diagnostics, Python/shell/workflow/JS checks | published-quality |
| Fresh code matching PR141:567 unit tests, instrumented40-test runtime suite, real runner/transaction fixture | published-unit-owners |
| 73-case real-job integration suite; three explicit platform/feature exclusions | transaction-reviewed-jobs |
| Final transaction admission, persistence, lock and reset faults | transaction-integrated-owners |
| Nine actual behavioral mutant kills | transaction-integrated-mutations |
| Real loader-document leak detection and negative control | transaction-integrated-leak-oracle |
| Real library allocation-failure and ASan/UBSan suites | transaction-faults; transaction-faults-sanitize |
| Frozen/current ABI comparisons | transaction-abi; final published-abi rerun |
| Python/Node SDK and conformance checks | transaction-integrated-sdks; final published-sdks rerun |
| Parser/search/runtime ownership and portable parser/library fuzz smoke | transaction-ownership-all |
| macOS configured leak suites and CLI checks | transaction-leaks |

The broad local `make test` run passed in1,280.624seconds, but started before
the final directory-OOM refinement. It is recorded as broad regression evidence,
not falsely relabeled as a complete run of the later tree. The changed paths
were subsequently rechecked by the full real-job suite, fresh unit/resource
checks, faults, sanitizers, ABI/SDK and leak tests. Hosted CI/Nix supplies the
separate whole-published-revision matrix. The collector marks source mismatches
explicitly. Optional/platform-specific skips remain visible, never counted as
successful execution of that missing platform.

## Performance against the continuation's parent

Both parent fdd5aa7 and the published-code candidate were freshly built on the
same macOS arm64 host with Apple clang21.0.0 and the same release flags. These
results compare this continuation with its parent, not with the original C-only
pre-series baseline. The broader series' cumulative measurement is separately
owned by the combined PR's verification record.

| Measurement | Parent | Candidate | Result |
|---|---:|---:|---|
| Stripped executable |1,121,184B|1,121,184B|Unchanged;below6MB|
| `--version` median |3.184ms|3.167ms|PASS|
| `ask --help` median |3.171ms|3.197ms|PASS|
| PTY first-prompt median |2.986ms|3.053ms|PASS|
| Nine parser workloads |reference|0.967–1.052× elapsed time|AllPASS|
| Event workload |reference|1.008× elapsed time|PASS|
| Parser/event peakRSS |reference|1.000×|PASS|
| Local-mock TUI latency median |435.7ms|432.8ms|PASS|
| Local-mock delayed-stdin completion median |885.9ms|866.7ms|PASS|

Startup used102 observations per CLI mode,24 fresh PTY launches and three
alternating batches. Parser/event tests used2,000 iterations and three batches.
The mock-latency tests used20 runs per binary; these are controlled fixture
measurements, not real external-provider inference times. The stdin benchmark
measures completion after delayed input, not first-token display.

The shared C++ runtime remains `/usr/lib/libc++.1.dylib`, alongside libSystem;
it is reported separately rather than hidden inside executable accounting.
Raw measurements and comparison decisions are in artifacts/performance/.

## Hosted verification and remaining delivery state

PR141 remains the canonical combined delivery. At the latest recorded check,
19 of25 checks passed and six were still running; no current failure was
reported. Completed lanes include hosted quality, Darwin/ARM64 Linux builds,
both musl builds, Windows, wasm, ThreadSanitizer, fuzzing, Valgrind and Linux
SDK configurations. Remaining checks were the Linux x86-64 full suite,
three Nix configurations and two hosted macOS SDK jobs.

That snapshot is time-bound. Pending checks are not passes, and this supplement
does not mark the draft PR ready, merge it, or claim final cross-platform signoff.
The current check results and the combined PR's own evidence supersede this
snapshot when those runs finish.
