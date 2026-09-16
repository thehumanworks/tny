# Evidence: C++ phase 3

Contract: [contract.md](contract.md). State: **INCOMPLETE** pending coordinator
reviews, full ps-dependent integrations, external platform/leak/ABI/SDK gates,
and phase/cumulative performance measurements. Passing local subsets are not
issue completion. All work is uncommitted; no agents, commits, pushes, PRs,
live provider calls, main-checkout access or other-worktree changes were made.

Assignment baseline: `4c62c6692bace9e6365e4fa5448dfbcee5bb32b2`, clean branch
`migration/cpp-phase3`, `/Users/tomas/projects/tny-cpp-phase3`. This is the
integrated assignment parent, distinct from the contract's planning baseline.
The existing contract and immutable initial snapshot compared byte-for-byte
before production changes; initial SHA256 is
`4751a2e787662af651da523f3032d248d1bf5d611408550f34e264748f40dee5`.
[Preimplementation manifest](artifacts/baseline-sha256.json) records source,
tests, build inputs and all existing ADRs. The delegated contract remains
canonical; no competing goal or contract was created. Reviews and final
integration remain coordinator-owned under the user's explicit instructions.

## Delivered changes

[ADR 0117](../../adr/0117-runner-and-job-resource-ownership.md) and the
[ownership inventory](ownership.md) describe each resource, its acquisition,
borrow/transfer/release ordering, failure behavior and behavioral oracle.

- `runner.c` and `jobs.c` become private C++ translation units. Public/private
  entry-point signatures retain C linkage. Move-only descriptors and locks
  replace owning integer fields in runner/client, transaction and slot aggregates.
  Existing caller-owned session locks remain borrowed; spawn-acquired parent
  copies close without explicit unlock. Value initialization and allocator-backed
  construction replace calloc/memset on nontrivial owners.
- Runner restart retains the writer/listener through the existing checkpoint
  and R/G/C/RUN protocol. Anonymous payload transport and secure_free remain.
  Transfer calls clear the prior owner. Failed listener/fork/client allocation,
  role handshake and restart validation close their acquired descriptors.
  Session quiescence/save/drain/unlink/release/bye remain explicit and ordered.
- Job transactions release their document/directory/lock on ordinary returns.
  Partial pipe construction, supervisor admission and item descriptors have
  single owners. The C handshake explicitly consumes released pipe ends.
- Native process-scope retirement is fallible and precedes terminal persistence.
  Unknown or failed retirement retains its scope and reservation hold; unresolved
  scopes transfer without allocation to a C supervisor-lifetime intrusive list.
  Destructors do not signal, wait, publish state, allocate errors or unlock an
  inherited open-file description. POSIX cancellation/reaping remains explicit;
  no metadata PID supplies authority. The POSIX PID slot is empty when a native
  scope owns the child.
- OS operations, source-fd staging and pre-exec paths remain behind the C seams.
  Signal handlers still only set flags. The existing fork-only runner is the
  full child runtime, distinct from the async-safe pre-exec process seam.
  macOS TLS fallback, MSYS Job authority and wasm unsupported policy are retained.
- Make wildcard discovery covers every native/PIC/fault/sanitizer/fuzz/wasm lane.
  The C++ discovery gate explicitly checks both converted files. CI and Nix add
  `test-runner-ownership` and critical mutations; Nix inputs already include all
  src/tests files and need no new dependency. The artifact fixture now compiles
  the real private jobs implementation as C++, with C host objects kept separate.
- Narrow enum-size annotations preserve existing C/C++ layouts; the runner's
  existing zero role now has an explicit unhandshaken enumerator. No analyzer
  family or warning gate was disabled. The checked printf error helper retains
  a single documented C-varargs annotation to avoid allocating diagnostics.

## First-slice self-review

The initial spawn/failure/cleanup conversion preceded restart and job conversion.
It passed 19 runner tests / 3,139 assertions with ASan/UBSan. Its new 12-cycle
real-runner loop counted 4 descriptors before and after, verified borrowed caller
ownership, parent-close without child unlock, writer freedom and socket absence
at bye. Existing missing-snapshot, competing-spawn and reload tests passed.
Self-review verified reverse destruction closes the listener before the acquired
writer, failed fork unlinks while still holding ownership, and the child consumes
its listener without running parent-stack destructors. This is not independent
review and does not fill the Reviews section.

## Execution environment and development failures

The local command recorder is `build/phase3-run.py`; complete raw logs remain
under ignored `build/phase3-logs/`. It sets `TMPDIR=$PWD/build/tmp`, local compiler
module caches, and removes TNY_TOOLS and all inherited `_BASE_URL`, `_API_KEY`,
`_API_KEY_CMD`, `_WIRE_API` overrides. Loopback fixtures set only their own
synthetic credentials. No live provider endpoint is invoked. Actual versions
are in [toolchain.json](artifacts/toolchain.json).

Development failures were retained in run records and superseded by passing
reruns: C++ reserved `public` parameter, void-pointer conversions and C99 compound
literals; C enum-width/default-initialization diagnostics; a test's 33-character
job ID; and a restart socket-identity assertion that initially missed the existing
long-path fallback socket. The socket assertion now resolves the actual local
socket from the documented candidates and probes that inode.

The first close-transfer mutant aborted before reaping its test-owned runner;
the fixture now kills/reaps that exact child before its expected assertion.
One C++ gate dry-run encountered an already-built temporary probe while another
build discovered it; an isolated rerun passed both negative controls and every
object inventory. No surviving compiler or linter error is treated as success.
Unit fixtures intentionally print invalid-setting and MCP-import diagnostics;
those expected runtime diagnostics are not compiler warnings.

## Local gate results

The generated command table and per-run records below identify exact exit codes.
Final production sources are frozen; later verification/documentation changes
are distinguished from earlier development runs by manifests. [Run records](artifacts/run-records.json), [final source manifest](artifacts/final-source-sha256.json),
[dirty-tree manifest](artifacts/dirty-tree-sha256.json), [mutation results](artifacts/mutation-results.json)
and [binary hashes](artifacts/binary-sha256.json) retain the tested state.
The final run-manifest identity is
`8876ec17177cbeaa90c0abf14cbabd006290675167bdcfcc2a83f745548aef32`.
The last full quality run differs only by subsequent test-fixture/build inventory
and Nix comment edits; the final format/lint, build, ownership, discovery and
mutation gates cover those. Production C/C++ hashes match quality and final gates. `quality` explicitly reports its Darwin GCC-analyzer
skip; no Linux analyzer proof is inferred from its exit zero.

| Command | Exit | Log |
| --- | ---: | --- |
| `python3 tests/integration/test_jobs_cleanup_hold.py JobsCleanupHold.test_repeated_refusal_preserves_unknown_hold_and_record_bytes` | 1 | [cleanup-hold](artifacts/logs/cleanup-hold.log) |
| `python3 tests/integration/test_jobs.py JobsWorkerHandshake.test_repeated_invalid_admission_preserves_protected_state` | 1 | [jobs-focused](artifacts/logs/jobs-focused.log) |
| `python3 tests/integration/test_job_artifacts.py JobArtifactProducer.test_real_producer_identity_is_preserved` | 1 | [job-artifacts](artifacts/logs/job-artifacts.log) |
| `make -j8 quality` | 0 | [quality-final](artifacts/logs/quality-final.log) |
| `make -j8 debug release test-runner-ownership` | 0 | [verified-final-build](artifacts/logs/verified-final-build.log) |
| `make format-check lint-py` | 0 | [verified-format](artifacts/logs/verified-format.log) |
| `python3 tests/integration/test_background.py` | 0 | [verified-background](artifacts/logs/verified-background.log) |
| `build/tny-test` | 0 | [verified-unit](artifacts/logs/verified-unit.log) |
| `python3 tests/integration/test_interrupt.py` | 0 | [verified-interrupt](artifacts/logs/verified-interrupt.log) |
| `python3 tests/integration/test_background_agents.py` | 0 | [verified-background-agents](artifacts/logs/verified-background-agents.log) |
| `make test-libtny-fault` | 0 | [verified-fault](artifacts/logs/verified-fault.log) |
| `make test-libtny-fault-sanitize` | 0 | [verified-fault-sanitize](artifacts/logs/verified-fault-sanitize.log) |
| `make test-libtny-fuzz-smoke` | 0 | [verified-fuzz-smoke](artifacts/logs/verified-fuzz-smoke.log) |
| `make test-parser-smoke` | 0 | [verified-parser-smoke](artifacts/logs/verified-parser-smoke.log) |
| `make test-runtime-ownership` | 0 | [verified-runtime-ownership](artifacts/logs/verified-runtime-ownership.log) |
| `make test-cpp-gates` | 0 | [verified-cpp-gates](artifacts/logs/verified-cpp-gates.log) |
| `build/tny-test -s runner_suite` | 0 | [verified-runner-unit](artifacts/logs/verified-runner-unit.log) |
| `build/tny-test -s session_bg_suite` | 0 | [verified-session-unit](artifacts/logs/verified-session-unit.log) |
| `python3 tests/mutation/runner_critical.py` | 0 | [verified-mutations-final](artifacts/logs/verified-mutations-final.log) |
| `python3 tests/integration/test_jobs_msys.py OwnershipSourceChecks` | 0 | [verified-msys-source](artifacts/logs/verified-msys-source.log) |
| `python3 tests/integration/test_nix_ci_matrix.py` | 0 | [verified-nix-inventory](artifacts/logs/verified-nix-inventory.log) |

## Mutation results

`tests/mutation/runner_critical.py` builds private source copies, never edits
production files, requires compilation success and the intended behavioral
assertion, then reruns the unchanged baseline. All five mutants were killed:

| Mutation | Behavioral oracle |
| --- | --- |
| Release writer before final save/socket removal | Actual competing lock probe inside final persistence, followed by observed child/bye failure |
| Close a transferred descriptor | Actual client connection becomes unusable; send assertion fails after test-owned child cleanup |
| Signal a metadata PID | A live unrelated sentinel named only in a job record is terminated; observed wait status fails |
| Treat unknown cleanup as complete | The real cleanup-reclaim predicate wrongly grants reclaim despite an observed unknown record |
| Replay consumed batch | Persisted consumed checkpoint becomes readable again; disk reload/second-activation refusal fails |

These are compiled behavioral kills, not compilation failures. The replay mutant
checks durable consumption/refusal; actual tool and provider invocation counts
are separately checked by the real background/restart integration fixtures.

## Resource counts and fault coverage

All counts enumerate real descriptors through `/dev/fd` (or `/proc/self/fd`),
without ps or a fixed fd ceiling. Counts may differ by launcher's inherited fds;
comparisons always use the same process before and after each loop.

| Loop (final restored mutation baseline) | Cycles | Before | After |
| --- | ---: | ---: | ---: |
| Real runner start/borrowed lock/bye | 12 | 4 | 4 |
| Descriptor move/transfer/forced fd reuse | 200 | 4 | 4 |
| Job partial acquisition/failure cleanup | 100 | 4 | 4 |
| Item self-exec/pump/reap/log drain | 40 | 4 | 4 |

[Mutation baseline log](artifacts/logs/verified-mutations-final.log) and
[runner loop log](artifacts/logs/verified-runner-unit.log) retain the measurements.
Native stripped size is **1,104,608 bytes**; dependencies are libc++.1.dylib and
libSystem.B.dylib. This is a candidate observation, not a before/after comparison.

The source-bound fixture exercises actual partial pipe acquisition, missing job
records after lock acquisition, invalid inherited supervisor descriptors,
failed launch, client allocation failure after connection, and allocation-free
client destruction. Instrumented copies of unchanged C host seams inject
open/write/fsync/rename, first/second F_DUPFD_CLOEXEC and spawn failure; the
original state bytes and caller-owned descriptors remain intact.

The session suite separately kills a writer holder with SIGKILL, waits for its
observed death, proves kernel lock freedom, reacquires and compares the protected
snapshot byte-for-byte. The full interruption suite tests saturated and frozen
children using monotonic deadlines, including the slow-lock fixture. The
background/restart suite checks detached reattachment, failed exec, post-G rollback,
post-RUN recovery, listener identity, writer contention and exact tool/provider
counts. Known-unknown job holds and repeated invalid admission assertions ran,
but their ps-based teardown is unavailable in the sandbox.

## Reviews

### Coordinator-supplied independent design/first-slice review, 2026-09-16

Reviewed candidate: rebased phase-3 commit
`4e8a3526f772152ed059e81cdb2e8a707bc80fe7`. Reviewer identity was not supplied;
the coordinator supplied the independent read-only report and its
**APPROVE-WITH-FIXES** verdict. This is design/first-slice approval only, not
completion of #139 or a post-fix independent review. No agents were spawned
for this correction assignment.

| Finding | Disposition | Proof |
| --- | --- | --- |
| 1, major: unknown item cleanup not durably held while siblings run; failed terminal save permits later reclaim (P3-I4, P3-I6) | Fixed. The supervisor commits the existing root hold before acquiring any item child, aborts without launching when that commit fails, latches unknown cleanup in the item's result transaction, and clears the hold only with a committed terminal result proving complete cleanup. This conservatively retains claims after loss during a protected lifecycle; legacy/pre-protection owner-loss recovery remains available. No schema or PID authority was added. | The real-supervisor fixture injects a lost consuming-wait result while a sibling is alive, saves the mixed terminal/running record, reaps fixture children and kills/reaps the supervisor. A separate case rejects the final write. Both prove owner freedom, contender/retry/rm refusal, and unchanged record/claim bytes across retry/rm. Initial-write rejection launches no child; successful terminal persistence permits reclaim. Critical mutants remove the write-ahead commit and drop the partial-result hold. |
| 2, minor: metadata-PID sentinel writer races child death, causing SIGPIPE/EPIPE instead of the intended status oracle (P3-I4, C4) | Fixed. Close the writer to release the sentinel through EOF; retry interrupted waits and always reap before asserting cancellation/exit results. | The signal-metadata-PID mutant must compile and fail the explicit WIFEXITED status assertion, with the sentinel already reaped. |

### Rebased correction verification

The initial `git log -3` and `make -j8 debug` succeeded at the clean rebased
head before these corrections. All correction edits remain uncommitted.
The local gate and mutation tables above are historical, pre-rebase records;
the correction gate table below and refreshed source manifest supersede them
for current local proof. Hosted/platform, leak, ABI/SDK and performance limits
remain coordinator-owned and unchanged below. Existing finalized ADRs are
unchanged; this implements the persisted-hold requirements of ADR0101 and
ADR0117, with the conservative write-ahead behavior documented in `docs/jobs.md`.

**Correction gate: PASS** for findings 1 and 2 and the requested local reruns.
This does not change the full-issue INCOMPLETE verdict or the coordinator's
outstanding gates. All 564 unit tests pass (19,874 assertions); all seven
critical mutants compile and fail their intended behavioral assertions, and the
restored ownership baseline passes. The added mixed-item regression also fails
against `git show 4e8a352:src/core/jobs.cpp` at the missing durable hold assertion,
after fixture children are reaped; that expected-failure experiment is recorded
as a successful regression check, not as a passing pre-fix implementation.

[Correction run records](artifacts/review-run-records.json) bind every command to
`4e8a3526f772152ed059e81cdb2e8a707bc80fe7` plus the same 634-input
[refreshed source manifest](artifacts/final-source-sha256.json), identity
`cdefff16fd58051680ba0659de1a3bd899f41feef771a0260878ec41a5db3b5a`. This refresh includes the rebased runtime, allocator,
transport and build inputs as well as these fixes. [Mutation results](artifacts/mutation-results.json)
and [binary hashes](artifacts/binary-sha256.json) are refreshed as well. The
[dirty-tree manifest](artifacts/dirty-tree-sha256.json) records the uncommitted
handoff, excluding itself to avoid a self-referential hash.

All requested commands exit 0. There are no compiler/linter warnings or
sanitizer findings. The unit suite's expected invalid-settings/MCP-import
warnings remain visible in its raw log; these are intentional negative-test
runtime diagnostics. Darwin quality explicitly skips the Linux-only GCC
analyzer, as before. The added supervisor fault fixture exercises real POSIX
children, pipes, locks and persistence; it does not claim native MSYS runtime
proof. There is no new target or dependency requiring a Nix inventory change.

| Command | Exit | Evidence |
| --- | ---: | --- |
| `make -j8 debug` | 0 | [review-debug](artifacts/logs/review-debug.log) |
| `build/tny-test` | 0 | [review-unit](artifacts/logs/review-unit.log) |
| `make test-runner-ownership` | 0 | [review-ownership](artifacts/logs/review-ownership.log) |
| `make test-runtime-ownership` | 0 | [review-runtime](artifacts/logs/review-runtime.log) |
| `make test-libtny-fault` | 0 | [review-fault](artifacts/logs/review-fault.log) |
| `make test-libtny-fault-sanitize` | 0 | [review-fault-sanitize](artifacts/logs/review-fault-sanitize.log) |
| `python3 tests/mutation/runner_critical.py` | 0 | [review-mutations](artifacts/logs/review-mutations.log) |
| `make quality` | 0 | [review-quality](artifacts/logs/review-quality.log) |
| `make -j8 release` | 0 | [review-release](artifacts/logs/review-release.log) |
| `python3 tests/integration/test_background.py` | 0 | [review-background](artifacts/logs/review-background.log) |
| `python3 tests/integration/test_background_agents.py` | 0 | [review-background-agents](artifacts/logs/review-background-agents.log) |
| `python3 tests/integration/test_interrupt.py` | 0 | [review-interrupt](artifacts/logs/review-interrupt.log) |
| `python3 build/review-regression-baseline.py` | 0 | [review-regression-baseline](artifacts/logs/review-regression-baseline.log) |

## Review 2 dispositions

Baseline: clean `2727f6c`, inspected before edits on 2026-09-16. This is a
bounded coordinator-assigned correction under the existing contract, P3-I4 and
P3-I6; independent review and full-series completion remain coordinator-owned.
The supplied independent reviewer confirmed the write-ahead hold and sentinel
fix and found one remaining major: allocating a replacement boolean can erase
the existing hold when value allocation fails after key allocation succeeds.

Planned acceptance: update existing booleans without allocation, ensure a failed
insertion cannot remove a key, and reproduce the key/value allocation boundary
with the real yyjson allocator. Persist/reload the resulting running/pending
record after observed supervisor loss and prove contender/retry/removal refusal
and unchanged protected bytes. Run every requested local gate, preserve the
prior records, and refresh source, binary, mutation and dirty-tree manifests.
No new schema, platform seam, target, dependency or ADR decision is required.

**Review 2 correction gate: PASS.** The major finding is fixed in `jm_set_bool`:
existing booleans are changed in place, without allocation; new/retyped values
are passed to `yyjson_mut_obj_put` only when both key and value exist. This
also protects final hold updates. Normal terminalization still clears the hold
when cleanup is proven. Job state names and exit semantics are unchanged.

The allocation regression extends the real mixed-item supervisor-loss fixture.
After observing/reaping the supervisor and its fixture children, it reloads the
running/pending record with its existing hold, leaves one yyjson value-pool slot
for a key, and arms a one-shot failure on pool growth. It invokes the actual
re-latch setter, verifies zero allocations and the same boolean node, then
consumes the spare node and observes the still-armed allocation failure. After
allocations resume, a successful store/reload retains the hold. Contender,
retry and removal remain refused, and retry/removal preserve record/claim bytes.
This is a targeted allocator/setter regression combined with real persisted
ownership behavior, not a claim of exhaustive supervisor allocation coverage.

The same fixture compiled with the exact `2727f6c` jobs implementation fails at
the post-store missing-hold assertion (fixture line 503), after every child was
reaped. The baseline experiment exits 0 only because it verifies that expected
failure. The corrected fixture passes under ASan/UBSan. All 564 unit tests pass;
all seven existing critical mutants are killed at their intended behavioral
oracles and the restored baseline passes. No additional agents were spawned;
this disposition responds to the coordinator-supplied independent review.

[Review 2 run records](artifacts/review2-run-records.json) bind every command to
`2727f6cf19b95e58b3fb6dbd787bb58b41e503c4` plus the same 634-input
[canonical source manifest](artifacts/final-source-sha256.json), identity
`fcd62b9e21a3654d386e05a1786ca81108be8ebe8b5420b1a794138b400f7f12`. These current records supersede the historical local
correction hashes above. [Mutation results](artifacts/mutation-results.json),
[binary hashes](artifacts/binary-sha256.json) and the
[dirty-tree manifest](artifacts/dirty-tree-sha256.json) are refreshed. Binary
hashes cover the four binaries rebuilt/exercised by these gates; the historical
release executable is excluded because this correction did not rerun release.

All requested gates exit 0 with no compiler/linter warnings or sanitizer
findings. The full unit log retains intentional invalid-settings/MCP-import
negative-test diagnostics. Darwin quality explicitly skips the Linux-only GCC
analyzer. Previously recorded coordinator-owned platform, leak, integration and
performance gates remain unchanged; full-issue completion is still INCOMPLETE.
Finalized ADRs are unchanged, and all correction changes remain uncommitted.

| Command | Exit | Evidence |
| --- | ---: | --- |
| `make -j8 debug` | 0 | [review2-debug](artifacts/logs/review2-debug.log) |
| `build/tny-test` | 0 | [review2-unit](artifacts/logs/review2-unit.log) |
| `make test-runner-ownership` | 0 | [review2-ownership](artifacts/logs/review2-ownership.log) |
| `make test-libtny-fault` | 0 | [review2-fault](artifacts/logs/review2-fault.log) |
| `make test-libtny-fault-sanitize` | 0 | [review2-fault-sanitize](artifacts/logs/review2-fault-sanitize.log) |
| `python3 tests/mutation/runner_critical.py` | 0 | [review2-mutations](artifacts/logs/review2-mutations.log) |
| `make quality` | 0 | [review2-quality](artifacts/logs/review2-quality.log) |
| `python3 build/review-regression-baseline.py` | 0 | [review2-regression-baseline](artifacts/logs/review2-regression-baseline.log) |

## Unmet gates and coordinator handoff

- `make test`, `make leaks`, `make test-abi`, `make test-sdks`, full job race,
  cleanup-hold and artifact integration suites, and benchmarks are assigned to
  the coordinator outside this sandbox. No macOS leaks task-port proof is claimed.
- Selected new `test_jobs.py` and `test_jobs_cleanup_hold.py` cases and the rebuilt
  real producer/artifact case return **1 at teardown**, with `PermissionError:
  Operation not permitted: ps`. Their assertions passing before teardown does
  not make these gates pass. The full suites were not repeated against this
  known sandbox denial. Test-owned accepted jobs had reached terminal state.
- Actual Linux/glibc/musl, native Windows/MSYS x64, wasm node/browser and Nix
  execution remain unmet. The MSYS source check is only source evidence, not
  runtime handle accounting, admission, cancellation or sole-reaper proof.
- No before/after startup/TTFT, peak-memory, wasm-size or cumulative pre-series
  benchmark claim is made. Native descriptor counts are measured above; actual
  Windows handle counts and full durable-job-cycle performance remain unmet.
- Full fault-matrix signoff still requires the coordinator's ps-dependent
  admission/cancel/reap/log-drain/snapshot races and actual platform execution.
  Focused syscall injection is not claimed as exhaustive testing of every OS
  error at every transfer on every platform.
- Independent design/first-slice/risky-boundary reviews and integrated delivery
  remain with the coordinator. All edits are uncommitted by instruction.

| Unavailable environment | Required command / gate |
| --- | --- |
| macOS outside sandbox | `make test leaks test-abi test-sdks`; full `test_jobs.py`, `test_jobs_cleanup_hold.py`, `test_job_artifacts.py` |
| Linux glibc | `make test CC=gcc CXX=g++`; `make quality ANALYZER_CC=gcc-14`; gcc/clang `make warn-strict`; `make valgrind`; all ownership/fault/ABI/SDK targets |
| Linux sanitizer/fuzzer | `make test-libtny-tsan`; `make test-libtny-fuzz`; `make test-parser-fuzz FUZZ_CC=clang FUZZ_CXX=clang++ FUZZ_RUNS=20000 FUZZ_SECONDS=60` |
| Linux musl | `make release STATIC=1`; `make test-unit test-parser-smoke test-runtime-ownership test-runner-ownership SANITIZE=0 STATIC=1` |
| Native Windows MSYS x64 | `make release`; `make test-unit test-parser-smoke test-runner-ownership SANITIZE=0`; `python3 tests/integration/test_jobs_msys.py`; full job/interrupt suites |
| wasm node/browser | `make wasm wasm-web`; existing OpenAI/ACP/Codex wasm fixtures; `test_background.py` unsupported path; `test_jobs.py` unsupported path; `test_site_wasm.py`; wasm size gate |
| Nix | `nix flake check` |
| Same-host phase and pre-series performance | ADR0115 `bench_startup.py` protocol; `bench_ttft.py --bench tui --iters 20` and `ask-stdin`; stripped native/wasm sizes, dependencies and peak memory |

## Final reconciliation

P3-I1–I3 have implemented ownership and current local resource/lock/restart proof.
P3-I4 has local cancellation/sentinel/hold coverage, with full job race teardown
and platform proof outstanding. P3-I5 retains the existing platform seams but
requires actual unavailable-platform execution. P3-I6 has local bounded shutdown,
allocation-free descriptor/client cleanup and resource counts; performance and
platform resource proof remain unmet. C0 timing is recorded, C1 reviews remain
coordinator-owned, and C2–C6 are incomplete wherever their full external proof or
integration step is outstanding. No invariant or missing gate is silently dropped.
