# Evidence: C++ phase 2

Contract: [contract.md](contract.md). State: **INCOMPLETE** pending coordinator
reviews, outside-sandbox/platform gates and performance proof. Local
ownership conversion covers plan items 1–6. The P2-I3 follow-up below closes
the provider-cancellation allocation gap on this host. Passing host subsets
are not whole-issue completion.

Assignment baseline: `a33deda56d6b1388832d98bc3a52572bd15b4a70`, clean branch
`migration/cpp-series`, after coordinator integration of phase 1. All work was
performed in `/Users/tomas/projects/tny-cpp-series`, without agents, commits,
pushes, PRs or live provider calls. No other checkout was inspected or modified.
The existing contract and `contract.initial.md` compared byte-for-byte equal
before production changes. The preimplementation manifest is
[baseline-sha256.json](artifacts/baseline-sha256.json). Existing ADRs, phase-1/3
verification records and `tests/bench/` remain unchanged.

The existing delegated verification contract is authoritative. No competing
native goal or contract was created. The user explicitly assigns independent
reviews and final integration to the coordinator.

## Implementation and ownership

[ADR 0116](../../adr/0116-runtime-event-and-async-ownership.md) records the new
private C++ ownership area, immutable payload views, scoped mutex and distinct
provider/host async leases. [Ownership inventory](ownership.md) covers runtime,
session, engine, queue, reserves, C errors, registration metadata, every async
reference, OpenAI/Cursor adapters, callback reentrancy and teardown.

- `owned_event.cpp` replaces the runtime field-copy/free pair with a stable
  record and one byte vector. The local field descriptors describe copies;
  there is no matching per-field destruction table. All storage is allocated
  before pointers are published. Only unique handles move through the queue;
  neither the record nor its storage moves. Payload/metadata views are const.
- The C scheduler retains event/count/byte limits, suppression and terminal
  policy. Reserves use the same owner, preserve logical accounting and retain
  transactional pre-turn replacement. OOM settlement publishes terminal before
  backend cancellation, suppressing callback allocation during reserved drain.
- `custom_tools.cpp` replaces manual references, field frees and lock/unlock
  branches with unique metadata/handle owners, allocator-backed shared call
  and registry state, and a scoped pthread mutex. Generation, epoch,
  active/completed/closing checks remain explicit. A non-owning active list
  cannot retain unrelated calls. Provider pending handles are consumed/cleared
  by take/invalidate; worker handles have the original exactly-once release.
- Public headers, layouts, exported names, sized initialization, capability
  flags and release functions are unchanged. Existing C adapters retain
  owner-thread/process checks, callback guards, copied-input semantics and
  thread-safe cancellation. No new concurrent session destruction is allowed.
- Make discovery covers native release/debug/PIC/fault/sanitizer/TSan/fuzz/wasm;
  the runtime ownership target uses the real unit suite with injectable C++
  allocations. CI and Nix run the target and critical mutations. Public async
  worker stress is added to the sanitizer gate as well as existing Linux TSan.

## First-boundary self-review

Before converting the registry, the owned-event slice passed 33 runtime tests
and 4,785 assertions under ASan/UBSan. The review checked every payload field,
embedded length, present-empty strings, fixed reserve capacity, partial
construction cleanup, queue transfer, destruction after parent teardown and
byte-limit neighbors. It found the old OOM cancellation callback could copy
another event; settlement now suppresses that path and an allocation counter
proves zero allocations with that fake backend. Real provider cancellation
allocations remain a separate gap, detailed below. This is a self-review, not an independent review.

## Execution environment and development failures

Commands run through the recorded local runner with
`TMPDIR=$PWD/build/tmp`, `CLANG_MODULE_CACHE_PATH=$PWD/build/module-cache` and
`SWIFT_MODULECACHE_PATH=$PWD/build/module-cache`. It removes `TNY_TOOLS` and all
inherited `*_BASE_URL`, `*_API_KEY`, `*_API_KEY_CMD`, `*_WIRE_API` overrides.
Loopback fixtures provide synthetic credentials. Temporary build/log files
stay below ignored `build/`; durable selected evidence is under `artifacts/`.

Observed development failures, subsequently repaired:

- Event extraction initially removed `dup_cstr`, still used by unrelated C
  extension paths (build exit 2); restored that helper.
- C++ registry initially used an invalid pointer `static_cast` (build exit 2);
  corrected the byte reinterpretation.
- The first full unit run returned 1 because `build/tny` did not yet exist for
  the runner shell fixture. Building release resolved it; all 560 tests passed.
- C++ callback test initially expected a repeated mock conversation to request
  the tool again (exit 1). A fresh session now exercises the second callback.
- Node retained-event comparison initially compared null-prototype SDK records
  against normal-prototype structured clones (exit 1). It now compares clones
  on both sides; every field remains checked.
- Quality initially returned 2 for C enum width suggestions at the new C++
  boundary, then 2 for import ordering. Narrow enum annotations preserve the
  existing C layouts, matching phase-1 convention; Ruff fixed the import.
  No analyzer family was disabled.
- The isolated allocation harness first used `-Ithird_party/yyjson`, colliding
  with libc++ `<version>` on the case-insensitive filesystem (exit 1). It now
  uses the project's `-iquote` convention.

Early gate runs preceded final const-view/test additions and are superseded
by final runs. The final source manifest and per-run records distinguish them.
Expected diagnostics in negative tests do not count as compiler warnings.

## Local gate results

[Run records](artifacts/run-records.json) preserve all 48 gate executions,
including failed development attempts, commands, exit codes, elapsed time and
revision. [Final source manifest](artifacts/final-source-sha256.json) has SHA256
`e5cc18731cd1c12b641ae8ac63184872ecf0bdf08dc1f335c1681127353d006e`.
[Dirty-tree manifest](artifacts/dirty-tree-sha256.json) identifies all changed
artifacts; [binary hashes](artifacts/binary-sha256.json) identify tested outputs.
[Toolchain](artifacts/toolchain.json) records actual compiler/tool versions.

The final build targets were combined in one `make -j8` invocation; each was
reached and the aggregate returned 0. The later error-construction scenario
changed only `test_libtny_faults.py`; both fault gates and `make format-check
lint-py` were rerun. Production source, compiled unit tests and SDK clients
match the full quality/unit/mutation runs. `changes_from_final` in run records
shows the sole final Python delta for those earlier runs; null marks older
superseded development records without a per-run manifest.

| Command | Exit | Evidence / limitation |
| --- | ---: | --- |
| `make -j8 debug release lib-shared-active` | 0 | Final mixed native builds, no compiler warnings |
| `build/tny-test` | 0 | 560 tests, 19,548 assertions, ASan/UBSan |
| `make quality` | 0 | Complete local checks; explicit Darwin GCC analyzer skip |
| `make format-check lint-py` | 0 | Final Python fault-test addition |
| `make test-runtime-ownership` | 0 | 37 tests, 5,002 assertions, injectable C++ owners |
| `make test-libtny-fault` | 0 | Final exhaustive public allocation/error sweeps |
| `make test-libtny-fault-sanitize` | 0 | Same sweeps plus native host and real async worker stress |
| `make test-libtny-fuzz-smoke` | 0 | Corpus and positive/negative class checks |
| `make test-parser-smoke` | 0 | Parser faults/splits plus backend retention regression |
| `make test-cpp-gates` | 0 | All object/quality discovery lanes; both negative controls |
| `python3 tests/mutation/runtime_critical.py` | 0 | Six compiled behavioral kills and restored baseline pass |
| `make test-libtny-mutation` | 0 | Ten valid behavioral kills; seven invalid excluded |
| `python3 tests/integration/test_libtny.py` | 0 | Installed C/C++ consumers, exact export set, affinity, retained views |
| `python3 tests/integration/test_libtny_custom_tools.py` | 0 | C workers, C++ sync/async, Python callbacks, Go/Swift headers |
| `python3 tests/integration/test_libtny_faults.py build/lib-fault/libtny.1.dylib` | 0 | Executed by fault target; final tool_error indices included |
| Python `unittest discover -s sdk/python/tests -p test_sdk.py -v` with explicit library/PYTHONPATH | 0 | 25 tests, one explicit ABI0-artifact skip; not full SDK aggregate |
| `npm --prefix sdk/typescript run build` | 0 | Native addon build and verification |
| `node sdk/typescript/test/integration.mjs` | 0 | Strict mock, workflows, permissions, cancel, slow reader, retained views |
| `python3 tests/integration/test_ask_events_conformance.py` | 0 | CLI JSONL matches public libtny view/getters |
| `python3 tests/integration/test_ask_events.py` | 1 | Sandbox denies ps during process-cleanup case |
| `make test-libtny-tsan` | 2 | Linux-only target; no Darwin TSan proof |
| `python3 tests/integration/test_nix_ci_matrix.py` | 0 | Source/config inventory only, not a Nix build |
| Protected-file hashes, contract equality, `git diff --check` | 0 | Immutable records unchanged; no whitespace errors |

No outstanding compiler/linter error is reported as passing. SDK Node's early
NO_COLOR/FORCE_COLOR environment warning was absent in its final rerun with
FORCE_COLOR removed. Final logs and mutation assertions are retained here;
complete local build logs remain below `build/phase2-logs/`.

Behavior coverage includes 200 retained, fully populated events after engine
and session teardown; embedded NUL text and present-empty fields; exact
byte-cap neighbors and aggregate accounting; the existing event-count cap,
slow reader, repeated cancel, permission and transport-death cases; a
zero-allocation reserved settlement test with the fake callback backend; repeated OOM turns then success;
exhaustive owner/container/control-block failures; and retained-event reads
through actual libtny after runtime destruction.

Public worker fixtures cover success, wrong generation, duplicate completion,
invalid UTF-8, wake, cancel, late completion, unregister and runtime destruction.
Ten cycles additionally exercise completion racing owner cancel/unregister/
close, delayed completion after cancel/unregister, and release before provider
take. The C++ client runs both synchronous and immediate asynchronous release.
Python and Node SDK tests retain events across later turns and parent closure.
CLI JSONL/libtny conformance compares field values and exact ordering.

The public allocation sweep includes eight tool-registration indices (object,
strings, JSON, vector/control block), five invalid-spec/error-construction
indices, session-create reserves, 67 rearm indices
and 151 next-event indices on this fixture. The runtime suite separately
sweeps every discovered event and async-construction allocation twice, then
checks recovery and live-owner counts. Fixture-dependent counts are evidence
for this input, not universal allocation totals.

## Reviews

### Independent phase-2 review follow-up (2026-09-16)

The coordinator supplied an independent review of `4c62c66` with verdict
**APPROVE-WITH-FIXES**; reviewer identity was not included in the handoff.
This follow-up starts from clean `1b2b89a98ded270af08e4d8ae8b26ba6df2a90b2`
and remains uncommitted. It uses the existing delegated contract, with no
additional agents or changes to coordinator-owned integration/performance work.

| Finding | Invariants | Disposition |
| --- | --- | --- |
| 1: `tools_call_free()` drops the pending provider owner | P2-I4 | Fixed: invalidate/consume the async owner before clearing the call. The regression frees a pending call twice, releases the worker, destroys the registry and asserts tracked live C++ allocations return to the pre-registry baseline. Before the fix it failed that final allocation assertion; after the fix it passes. |
| 2: C++ fixture releases a handle on negative completion return | P2-I3, P2-I5 | Fixed: release only on the ASYNC return path; negative status leaves cleanup to the invoker. The real C++ fixture injects result-copy OOM twice through `tny_tool_call_complete`, verifies two OOM statuses and failed tool events, then completes a successful async retry. It is now included in `make test-libtny-fault-sanitize`. Before the fix ASan caught heap-use-after-free in handle destruction; after the fix ASan/UBSan pass. |
| 3: integrated evidence and performance | Coordinator-owned | Not acted on; remains the coordinator's gate. This follow-up does not establish whole-phase completion. |

No new ownership decision is introduced. ADR 0118 is the provider OOM
settlement decision; no remaining stale `0117-allocation` references were found.
Existing ADR files remain unchanged. The sanitizer fixture reuses the existing
C++ compiler, Python mock driver, library and fault scope; the Nix input
inventory already includes them and its comments now record this dependency.

Development checks first exposed a missing `core/tools.h` include in the new
unit test (compile failure, repaired). The first fixed C++ fixture incorrectly
expected a turn-level error: callback failures are emitted as tool errors and
the mock then finishes normally. Its oracle now checks failed tool events,
exact OOM status counts, one tool-end per turn and successful recovery. These
failed development checks are not counted as passing gates.

The pre-fix [allocation failure](artifacts/review-fixes/review-repro-owner-fixed-include.log)
and [ASan failure](artifacts/review-fixes/review-repro-cpp.log) demonstrate that
both regressions detect the original bugs. The latter also contains macOS
symbolizer warnings during the expected failing run; these do not occur in
the passing sanitizer run.

All eight final commands exited 0 on the same current source/test/configuration
manifest ([source-sha256.json](artifacts/review-fixes/source-sha256.json)).
[Run records](artifacts/review-fixes/runs.json) retain exact commands, revision,
durations, exit codes and manifest hashes; [environment](artifacts/review-fixes/environment.json)
records tool versions. Runs use the sanitized environment described above.

| Requested gate | Exit | Result |
| --- | ---: | --- |
| [`make -j8 debug && build/tny-test`](artifacts/review-fixes/review-final-debug.log) | 0 | 562/562 tests, 19,688 assertions |
| [`make test-runtime-ownership`](artifacts/review-fixes/review-final-runtime.log) | 0 | 38/38 tests, 5,008 assertions, including pending-owner reclamation |
| [`make test-libtny-fault`](artifacts/review-fixes/review-final-fault.log) | 0 | 21 exhaustive scenarios and real-provider reserved cases |
| [`make test-libtny-fault-sanitize`](artifacts/review-fixes/review-final-sanitize.log) | 0 | Fault sweeps, C async host and real C++ callback OOM/recovery under ASan/UBSan |
| [`python3 tests/mutation/runtime_critical.py`](artifacts/review-fixes/review-final-mutation.log) | 0 | 7/7 intended behavioral kills; [results](artifacts/review-fixes/mutation-results.json) |
| [`python3 tests/integration/test_libtny_custom_tools.py`](artifacts/review-fixes/review-final-custom.log) | 0 | C/C++/Python clients and available Go/Swift header checks |
| [`python3 tests/integration/test_libtny_faults.py build/lib-fault/libtny.1.dylib`](artifacts/review-fixes/review-final-faults-direct.log) | 0 | Direct exhaustive allocation sweeps and real-provider reserved cases |
| [`make quality`](artifacts/review-fixes/review-final-quality.log) | 0 | Format, C/C++ clang-tidy, strict warnings, Ruff, shell and JS checks |

No compiler, sanitizer or quality warnings occurred in these final gates.
The full unit suite prints 14 existing expected negative-fixture warnings
for invalid tool profiles and malformed/unsupported/duplicate MCP imports;
these are retained in the raw log, not suppressed or represented as absent.
Darwin retains its documented GCC analyzer skip and disables ASan leak
detection; the new pending-owner regression explicitly checks tracked live
C++ allocations. This is host proof only.

Findings 1 and 2: **PASS** for the assigned fixes and requested host gates.
Finding 3 and whole-phase completion remain coordinator-owned and unresolved
by this follow-up. `git diff --check` passes, all existing ADRs compare
byte-for-byte with `1b2b89a`, and no changes were committed.

## Mutation results

The six critical mutants compile private copies and leave production sources
unchanged. All six compiled and were killed by behavioral assertions or ASan:

| Mutation | Intended failure | Exit |
| --- | --- | ---: |
| Retain borrowed payload | Heap use after callback input destruction | -6 |
| Omit generation check | Wrong-generation completion accepted | 1 |
| Release async shared reference early | Heap use after provider lease release | -6 |
| Omit queue-byte accounting | Byte-cap/trace assertion | 1 |
| Emit second terminal | Exact terminal/drain assertion | 1 |
| Allocate in reserved settlement | Zero-allocation counter assertion | 1 |

The final private-copy rerun returned 0, with every source hash unchanged.
[Mutation results](artifacts/mutation-results.json) and
[behavior logs](artifacts/mutations/) preserve each intended assertion.
`make test-libtny-mutation` returned 0: safety 4/4 valid kills (2 invalid),
fault settlement 2/2, custom tools 4/4 (5 invalid). The seven invalid mutants
are excluded, not counted as behavioral kills. All captured source hashes
matched after restoration; active/fault/sanitized libraries were then rebuilt
before final client checks.

## Allocation overhead

[Measured allocation record](artifacts/event-allocation.json) and
[reproducible harness](artifacts/event-workload.py.txt) compare the baseline's
actual event-copy implementation with the candidate module. Both use Apple
Clang `-Os`, fixed metadata, all eight payload pointers, a 1,024-byte text
payload and 200 simultaneously retained events on Darwin arm64. The harness
locally counts allocation calls/requested bytes and sums `malloc_size` block
sizes; it does not override global product allocation.

| Metric, 200 events | Baseline | Candidate |
| --- | ---: | ---: |
| Logical payload bytes | 219,000 | 219,000 |
| Allocations | 2,400 | 400 |
| Requested bytes including records | 262,200 | 270,200 |
| Allocator block bytes at peak retention | 332,800 | 307,200 |

That is 12 -> 2 allocations per event, +40 requested bytes per record and
-7.69% actual allocator block bytes for this workload. It is not a process RSS,
queue throughput or callback-throughput measurement. The candidate stripped
CLI measured 1,088,064 bytes, loading libc++.1.dylib and libSystem.B.dylib.
No baseline CLI size/startup/TTFT comparison is claimed by this assignment.

## Unmet gates and coordinator handoff

- **Historical P2-I3 implementation gap (closed by the follow-up below):** the runtime's event construction/queue/reserve
  settlement is allocation-free, but `tny_engine_fail_oom` still calls the
  provider's ordinary cancel callback. Source audit finds allocations in
  OpenAI `oa_cancel` (`tool_err`, transcript completion/persistence), Cursor
  `cu_cancel` (`cu_send_cancel` protocol request), and ACP `ac_cancel`
  (JSON notification construction). The new zero-allocation test uses the
  private fake backend and does **not** establish a whole-provider guarantee.
  No full-provider zero-allocation claim is made. An allocation-free emergency
  cancellation policy preserving later-turn transcript/protocol state, and
  real-provider fixture counters at permission/custom-tool waits, remain
  unfinished. Ordinary cancellation behavior was kept intact; this issue
  must not be marked complete with this gap.

- `make test`, `make leaks`, `make test-abi`, `make test-sdks` and the complete
  benchmark matrix are assigned to the coordinator outside this sandbox.
  Local selected SDK and clean-prefix/export tests do not replace aggregates.
- `python3 tests/integration/test_ask_events.py` returns 1 at the deliberate
  `ps -ax -o pid=,ppid=,command=` process-inspection check: sandbox
  `PermissionError: Operation not permitted`. Process cleanup proof is unmet.
- `make test-libtny-tsan` returns 2 with its Linux-only diagnostic. No macOS
  leaks task-port, Linux sanitizer/race/leak, Windows, wasm or Nix proof is
  claimed here.
- Independent design/first-slice/risky-boundary review, integrated verification,
  commits and delivery remain with the coordinator. This writer did not run
  another agent or claim self-review as independent approval.
- P2-I6 startup/TTFT, repeated queue/callback throughput and process peak memory
  within 10% remain unmeasured; the allocation table covers only event storage.

Required unavailable-platform commands:

| Environment | Command |
| --- | --- |
| Linux glibc | `make test CC=gcc CXX=g++`; `make quality ANALYZER_CC=gcc-14`; both gcc/clang `make warn-strict`; `make valgrind`; `make test-abi test-sdks test-libtny-fault test-libtny-fault-sanitize test-runtime-ownership` |
| Linux TSan/libFuzzer | `make test-libtny-tsan CC=gcc CXX=g++`; `make test-libtny-fuzz`; `make test-parser-fuzz FUZZ_CC=clang FUZZ_CXX=clang++ FUZZ_RUNS=20000 FUZZ_SECONDS=60` |
| Linux musl | `make release STATIC=1`; `make test-unit test-parser-smoke test-runtime-ownership SANITIZE=0 STATIC=1`; `make size-check SIZE_MAX=1572864 STATIC=1` |
| Windows MSYS x64 | `make release`; `make test-unit test-parser-smoke SANITIZE=0`; `make size-check SIZE_MAX=2097152`; existing CI fixture/package commands |
| wasm node/browser | `make wasm wasm-web wasm-dictation-fixture EMCXX=em++`; `TNY=$PWD/build/wasm/tny python3 tests/integration/test_openai.py`; `tests/integration/test_acp_ws.sh $PWD/build/wasm/tny`; `TNY=$PWD/build/wasm/tny python3 tests/integration/test_codex_chatgpt.py`; `TNY_TEST_WASM_BACKPRESSURE_REQUIRED=1 python3 tests/integration/test_wasm_event_backpressure.py`; `python3 tests/integration/test_site_wasm.py`; `make wasm-size-check` |
| Nix supported matrix | `nix flake check` |

## Final reconciliation

P2-I1, I2, I4 and I5 have ownership implementation and local behavioral coverage.
P2-I3 now has the host provider-settlement proof below; required external
leak/platform/TSan/aggregate ABI-SDK proof is still outstanding. P2-I6 has only the
allocation/size observations above. C0 timing is recorded; C1 reviews are
coordinator-owned; C2–C6 remain incomplete wherever external proof or delivery
is required. No requirement or unavailable gate is silently treated as passed.

## P2-I3 follow-up contract (2026-09-16)

Baseline `4c62c66`, clean tree; `git log -3` confirms rebased phase-1
`e51b232` and parked-batch parser release `77dece0`. Initial
`make -j8 debug` exited 0 before edits. This is the existing delegated P2-I3
assignment; independent review and whole-issue delivery remain coordinator-owned.

Acceptance before implementation: measure allocation attempts over emergency
provider cancellation plus reserved ERROR/TURN_END enqueue, then prove no
allocations on their public delivery. Exercise real native Responses transport
with partial text, permission wait and retained async custom-tool wait, exactly
one OOM ERROR and one error TURN_END, followed by success on the same handle.
Audit Cursor/ACP cancellation and Codex's shared native profile; retain public
exports, layouts, event schema and quiescent callback rules. Add a provider-path
allocation mutation; run every command in the user's requested host gate list.
Source/test hashes and final gate results will be recorded below.

## P2-I3 follow-up implementation and allocation trace

[ADR 0118](../../adr/0118-allocation-free-provider-oom-settlement.md) documents
resource-only emergency cancellation and later normal-memory recovery. No
public header, frozen record, backend vtable, event kind or callback signature
changed. `nm -gU build/lib/libtny.1.dylib` exactly matched the 64 names in
`abi/libtny.exports.macos`; the new introspection functions are test-only.
All previously tracked `docs/adr/` files compared byte-for-byte against HEAD.

| Boundary / former allocating sites | Emergency behavior and evidence |
| --- | --- |
| Provider `emit` → `backend_event` → `queue_event` → owned event record/vector | Normal callback ownership still allocates through the injected allocator. Once its scope failed, queue admission records OOM instead of constructing another ERROR/TURN_END. `after_backend` enters settlement only after borrowed provider callbacks return. Exhaustive `next_event` sweeps check this route. |
| `tny_engine_fail_oom` → provider cancel | Explicit thread-local settlement scope begins before cancel and ends after reserved enqueue; the terminal guard is published first. It clears pending finalization/extension cancellation so the subsequent scheduler pass cannot allocate JSON or call cancel again. Existing callbacks and owner-thread rules are retained. |
| OpenAI pending permission/custom/remaining batch: `tool_err`, `complete_tool`, `log_toolcall`, control-hook JSON, `session_add_tool_result`, `finish_tool_batch`, image flush, `session_save` | Emergency `oa_cancel` bypasses these construction paths. It invalidates the provider custom-tool lease, frees pending calls/permissions, discards images, removes an unsent preview in place, releases SSE/tool-call/raw/text/reasoning storage, clears turn state and returns to idle. A stack TURN_END callback is suppressed by the runtime guard. |
| OpenAI partial text / `emit_turn_end`: recovery path creation, assistant JSON, usage JSON, preview persistence and session serialization | Emergency cleanup does not call ordinary `emit_turn_end` or persist partial text. Completed transcript nodes stay owned. The existing provider-view repair creates missing tool results only when the later send can allocate; the strict mock verifies every call/output pair precedes the next user message. |
| Cursor `cu_send_cancel`: JSON buffer, RPC request/response, callback pump; graceful disconnect Shutdown RPC and recursive path allocation | Emergency cancellation closes SDK streams/RPC connections, stops the owned bridge, invalidates/frees reverse callback state, retains the agent identity and ephemeral store. Later send reconnects/resumes outside settlement. Unit checks and the linked fault-object fixture exercise local callback teardown: zero allocation attempts. |
| ACP `ac_cancel`: JSON notification and pending permission-result buffers | Emergency cancellation closes transport/owned process, frees permission and framing state and retains session identity. Later send reconnects through existing load/new semantics. Unit checks and the fault-object fixture exercise pending permissions and real pipes: zero allocation attempts. |
| WebSocket/TLS close frames | Emergency close skips wslay queue/send and TLS close-notify construction; existing contexts/sockets are released. Ordinary close retains its SIGPIPE guard. The standalone host-safety fixture now links `alloc.c` for this private dependency. |
| Codex callback adapter | There is no separate Codex backend directory: ADR 0065 routes the builtin profile through the same native OpenAI/Responses code and emergency branch. No independent live ChatGPT call is claimed. |
| Reserved ERROR/TURN_END and public pull delivery | Existing owned reserves are transferred; turn IDs are formatted into their preallocated slot, queue count/bytes updated, pending terminal freed. Pop/release uses owned pointers and destruction only. The fixture separately asserts zero `tny_alloc_test_scope_count()` on every post-OOM pull through DRAINED. |

The guarantee concerns **tny allocation attempts during reserved settlement**,
not the normal allocation that triggers OOM, pre-settlement transport parsing,
future reserve replenishment, or allocations internal to an embedding host or
system TLS destructor. `TNY_ALLOC_TESTING` wraps tny C allocations and the private
C++ owners. The instrumented scope surrounds the actual provider cancel callback,
not a fake callback or only the two reserved queue operations.

### Real-library and mutation proof

`test_libtny_faults.py --reserved-only <fault-library>` runs strict HTTP/SSE
Responses fixtures for partial text, parked permission and retained async custom
tools. Each uses a persistent real libtny session, reaches the provider state,
injects allocation 2 of `session_steer`, and observes runtime-driven emergency
cancellation. Each state is exercised twice on the same handle, followed by a
successful third turn (`TNY_STOP_DONE`, no ERROR). All six OOM settlements assert:

- injection reached; exactly one settlement scope; zero allocation attempts in it;
- exactly one ERROR with OOM code and one TURN_END with error stop, in final order;
- zero allocation attempts on every remaining public event pull;
- late completion of each retained async lease returns BAD_STATE, then the host
  releases it; strict provider transcript pairing succeeds on retry.

The exhaustive fault harness also checks the settlement-allocation counter after
every `next_event`, covering provider parser/callback failures in addition to the
deterministic parked-state cases. It covers all 21 existing discovery scenarios.
The sanitizer gate repeats this real-library fixture and the exhaustive sweeps,
then runs the C lifecycle and async worker hosts under ASan/UBSan.

The new `provider-allocating-settlement` mutant adds
`free(tny_alloc_malloc(1))` inside actual `oa_cancel` emergency cleanup. The
unmodified fault library passes; the separately compiled/linked mutant exits 1
at **allocation during reserved OOM settlement**. Together with the existing six
mutants, **7/7 are behaviorally killed**, with production source hashes unchanged.
See [mutation results](artifacts/p2-i3/results.json) and
[provider failure log](artifacts/p2-i3/provider-settlement-test.log).

The supplemental [adapter counter fixture](artifacts/p2-i3/adapter-counts.md)
uses the fully instrumented library objects and measures zero attempts in Cursor
callback teardown and ACP pending-permission teardown. It is local ownership
proof; host-service reconnects and remote turn termination were not live-tested.
No live provider credentials or requests were used.

### Development failures and final state

The original debug rebuild passed before edits. During this follow-up:

- The first permission fixture used safe `echo` and did not park; it now uses
  the established `write_file` permission fixture.
- Custom-tool registration initially followed session creation; corrected to
  register first, matching the public callback contract.
- A broad Cursor reconnect condition broke an existing send-reset unit test;
  reconnect now requires the existing emergency ended/cancelled state.
- The new Cursor callback test initially omitted its registry/options and
  include; corrected, then all 562 unit tests passed.
- TLS emergency-close guards exposed the standalone network fixture's old
  source-pattern/link assumptions. Guard nesting is preserved and its source
  list now includes `alloc.c`; the real SIGPIPE/deadline checks pass.

Earlier runs are retained as development history, not final proof. Final commands
use the existing sanitized-environment runner documented above. The source/test/
configuration/dependency manifest is [source-sha256.json](artifacts/p2-i3/source-sha256.json),
SHA256 `b32fc353c19f9dd9e699b5a7bc7f690fb8cf5ab8489e39663cd9417862578b24`,
on top of `4c62c6692bace9e6365e4fa5448dfbcee5bb32b2`. All final gate records use
that manifest. Existing Nix filters already include the edited source and fixture
files; no target, fixture directory or external tool was added.

### Follow-up host gates

| Command | Exit | Result |
| --- | ---: | --- |
| [`make -j8 debug && build/tny-test`](artifacts/p2-i3/p2i3-debug-gate.log) | 0 | 562/562 tests, 19,688 assertions |
| [`make test-runtime-ownership`](artifacts/p2-i3/p2i3-runtime.log) | 0 | 37/37 tests, 5,002 assertions |
| [`make test-libtny-fault`](artifacts/p2-i3/p2i3-fault-gate.log) | 0 | 21 exhaustive scenarios plus real-provider reserved cases |
| [`make test-libtny-fault-sanitize`](artifacts/p2-i3/p2i3-sanitize-gate.log) | 0 | Same fault cases and C/async hosts under ASan/UBSan |
| [`make test-libtny-fuzz-smoke`](artifacts/p2-i3/p2i3-fuzz.log) | 0 | All required fuzz classes and corpus pass |
| [`python3 tests/mutation/runtime_critical.py`](artifacts/p2-i3/p2i3-mutation-final.log) | 0 | 7/7 behavioral kills; real-provider allocation mutant killed |
| [`python3 tests/integration/test_libtny.py`](artifacts/p2-i3/p2i3-libtny-final.log) | 0 | C/ctypes, clean-prefix, affinity, cancellation and permission clients pass |
| [`python3 tests/integration/test_libtny_custom_tools.py`](artifacts/p2-i3/p2i3-custom-final.log) | 0 | C and C++ sync/async/cancel/deny clients pass |
| [`python3 tests/integration/test_libtny_faults.py build/lib-fault/libtny.1.dylib`](artifacts/p2-i3/p2i3-faults-direct.log) | 0 | Direct exhaustive sweep and real-provider reserved cases pass |
| [`make quality`](artifacts/p2-i3/p2i3-quality-final.log) | 0 | Format, clang-tidy, strict C/C++, Ruff, shell and JS checks pass |

No compiler, clang-tidy, strict-warning or sanitizer diagnostics remain in the
final gates. The unit suite intentionally prints existing warning/error text
for negative configuration, permission and malformed-import fixtures; these
are not compiler warnings and were not suppressed. Darwin's quality target
explicitly skips GCC `-fanalyzer`; Linux proof remains coordinator-owned.

**P2-I3 is met on this macOS arm64 host for the assigned provider-settlement
closure.** Real native Responses turns and the local Cursor/ACP resource paths
are evidenced above. This does not claim live remote-service cancellation,
host-provider reconnect validation, unavailable platform gates or completion of
all of #138. The broader unmet coordinator gates recorded earlier remain open.
Changes are uncommitted at HEAD `4c62c66`; no push, merge or live-session restart
was performed.

Final source/ABI/ADR reconciliation: all 728 manifest entries match current
bytes; production exports match the 64-symbol baseline; frozen public headers
and pre-existing ADRs are unchanged; `git diff --check` passes. The final review
checked callback quiescence, pending-tool invalidation, allocation-free parser
release, provider state reset, deferred transcript repair and reserve replenishment.
It is a scoped self-review; coordinator independent reviews are not fabricated.

## Review 2 dispositions (2026-09-16)

Pre-edit baseline: clean `f7b2b70`, single writer in `tny-cpp-p1fix`. Assigned
P2-I2/P2-I3 acceptance: no allocation/persistence/recovery from detected provider
OOM through reserved settlement; bounded ACP child termination; exact OOM pair
and successful later turn. Regressions cover accumulated usage in a later
Responses step, Cursor decoder OOM, and a child ignoring SIGTERM. The supplied
independent review defines this repair assignment; no new agents, native goal,
commits, pushes or live providers are authorized. Existing contract/ADR remain
the specification. Final gate records and dispositions follow below.

| Supplied finding | Invariants | Disposition and regression |
| --- | --- | --- |
| OpenAI/Codex parser OOM runs ordinary usage finalization/persistence | P2-I2, P2-I3 | `parser_failed` enters resource-only emergency cancellation after borrowed callbacks unwind; no ordinary `emit_turn_end`, usage construction or `session_save`. Secret turn state is still wiped. The persistent Responses fixture completes a tool step with usage (123 input / 7 output tokens), faults SSE growth in the second response, asserts byte-identical persisted JSON, exactly one OOM ERROR/TURN_END and zero settlement/delivery attempts. It repeats the failed turn and then succeeds on the same handle. |
| Cursor decoder OOM enters SDK error construction / ObserveRun recovery | P2-I2, P2-I3 | Preserve OOM status through the raw transport and SDK layer before synthesized error JSON or recovery; close resources at the quiescent dispatch boundary. A real loopback stream is parked inside a partial Connect envelope, then faults C++ payload growth with a retained run identity. The runtime emits exactly the reserved pair, the fully instrumented counter stays zero, and the listener sees no ObserveRun request. |
| ACP emergency teardown blocks before escalation | P2-I2, P2-I3 | Send SIGTERM, poll `waitpid(WNOHANG)` against a 500 ms monotonic deadline, escalate to SIGKILL, then reap with EINTR retry. The real spawned ACP fixture installs SIG_IGN before emitting its ready text. Settlement finishes in less than 2 seconds, `waitpid` confirms ECHILD, the exact reserved pair drains, and the same runtime reconnects/loads and completes a successful later turn. A 5-second fixture alarm bounds the deliberately broken mutation. |

The test-only settlement allocation total now counts attempts while either the
emergency scope or the provider-failure marker is active. The marker starts at
the detected parser/transport failure and lasts through the public call, so the
interval before `after_backend` and the reserved pair is measured too. The
triggering failed allocation is outside that interval. SSE/Connect stop after
a failed callback and release their owners directly; they do not consume later
frames or manufacture another C++ exception to unwind. Ordinary recovery still
has fixture coverage. Platform-library/embedding-host allocations remain outside
the tny allocator contract described in ADR 0118.

The new provider host links the complete fault-instrumented library object set,
including C providers and C++ parsers, and runs the actual unit regressions. It
is included in both the normal and ASan/UBSan fault gates. Ordinary unit builds
also exercise the same Cursor status route; only the fully instrumented host
claims injected C++ decoder-growth proof. Nix already includes all tests and
the required C/C++/Python tools; its inventory notes now name this dependency.

Critical regressions are falsifiable: restoring OpenAI ordinary finalization
fails the zero-allocation assertion; removing Cursor's dispatch short-circuit
fails the pre-settlement allocation counter; removing ACP escalation fails the
2-second deadline assertion. These supplement the existing seven ownership and
provider-settlement mutants. Private mutant copies leave product sources intact.

Development history (not final gates): the first native host link needed its
output directory created; Python import sorting was corrected. The first
Cursor shell invocation omitted its required binary argument. Darwin's bare
`mktemp -d` ignored TMPDIR in the ACP WebSocket fixture and hit the sandbox;
a build-local wrapper supplies an explicit template below `build/tmp`, without
changing the fixture or permissions. A log-runner output-directory error was
fixed and affected gate records rerun. Final parser inspection replaced a
synthetic post-callback `bad_alloc` throw with direct status return; dependent
checks were refreshed. Earlier successful checks are retained as history only.

No fresh independent reviewer was created: the user supplied Review 2 and
explicitly required a single writer without sub-agents. Existing ADRs and frozen
public headers are unchanged; the production library retains its 64 exports.
`ps` inspection and macOS `leaks` remain sandbox-denied as specified by the user;
neither was attempted. No live provider, commit, push or PR was used. This is
scoped host repair evidence, not whole-phase, live-provider or cross-platform
completion.

### Review 2 final host gates

All final runs below use the same [729-file source manifest](artifacts/review-2/source-sha256.json), SHA256 `9d101041f4030e95ae7d58c50bd8f5b090ea2a6b8ed790bd5e8f1bb8b32cc696`, on uncommitted HEAD `f7b2b704b018cd5ab01bcbfc20d7e727546d6613`. [Machine-readable records](artifacts/review-2/gate-results.json) retain commands, elapsed time, status and source hashes; [environment](artifacts/review-2/environment.json) records tool versions.

| Command / log | Exit | Result |
| --- | ---: | --- |
| [`make -j8 debug release lib-shared-active lib-shared-fault lib-shared-fault-sanitize build/runtime-test/tny-test build/lib-fault/provider-faults build/lib-fault-san/provider-faults`](artifacts/review-2/final-build.log) | 0 | Native debug/release, active/fault/sanitized libraries and injected provider hosts |
| [`make -j8 debug && build/tny-test`](artifacts/review-2/final-debug.log) | 0 | 564/564 tests; 19,741 assertions |
| [`make test-runtime-ownership`](artifacts/review-2/final-runtime.log) | 0 | 38/38 tests; 5,008 assertions |
| [`make test-parser-smoke`](artifacts/review-2/final-parser.log) | 0 | Split/lifetime/OOM corpus and immediate backend parser release |
| [`python3 tests/mutation/runtime_critical.py`](artifacts/review-2/final-mutation.log) | 0 | 10/10 behavioral kills; original sources unchanged |
| [`make test-libtny-fault`](artifacts/review-2/final-fault.log) | 0 | 21 exhaustive scenarios, four Responses settlement cases, real Cursor/ACP hosts |
| [`python3 tests/integration/test_libtny_faults.py build/lib-fault/libtny.1.dylib`](artifacts/review-2/final-fault-direct.log) | 0 | Direct sweep of the same fault library and provider regressions |
| [`make test-libtny-fault-sanitize`](artifacts/review-2/final-sanitize.log) | 0 | ASan/UBSan fault sweeps, provider hosts, C/async and C++ completion hosts |
| [`python3 tests/integration/test_libtny.py`](artifacts/review-2/final-libtny.log) | 0 | C/ctypes, clean-prefix, affinity, cancellation and permission clients |
| [`python3 tests/integration/test_openai.py`](artifacts/review-2/final-openai.log) | 0 | OpenAI mock integration assertions |
| [`sh tests/integration/test_cursor.sh /Users/tomas/projects/tny-cpp-p1fix/build/tny`](artifacts/review-2/final-cursor.log) | 0 | Cursor bridge mock, send/resume/effort/fast |
| [`python3 tests/integration/test_cursor_management.py`](artifacts/review-2/final-cursor-management.log) | 0 | Management aliases/RPC/streams/artifacts and process cleanup |
| [`python3 tests/integration/test_cursor_sdk_contract.py`](artifacts/review-2/final-cursor-contract.log) | 0 | 12 pinned SDK contract tests |
| [`sh tests/integration/test_acp.sh`](artifacts/review-2/final-acp.log) | 0 | ACP stdio model/permission/resume/framing/death fixtures |
| [`sh tests/integration/test_acp_ws.sh /Users/tomas/projects/tny-cpp-p1fix/build/tny`](artifacts/review-2/final-acp-ws.log) | 0 | ACP WebSocket model/resume/refusal/death fixtures |
| [`python3 tests/integration/test_acp_server.py`](artifacts/review-2/final-acp-server.log) | 0 | ACP server initialize/prompt/cancel/replay/error fixtures |
| [`make quality`](artifacts/review-2/final-quality.log) | 0 | Format, C/C++ clang-tidy, strict warnings, Ruff, shell, actionlint and JS |

No compiler, clang-tidy, strict-warning or sanitizer diagnostics occurred in these final gates. The full unit log retains 14 expected pre-existing negative-fixture warnings (invalid tool profiles and malformed/unsupported/duplicate MCP imports); they are not compiler warnings and were not suppressed. The WebSocket shell fixture also prints normal termination notices for its mock wrappers. Darwin quality explicitly skips GCC `-fanalyzer`; ASan leak detection is disabled on this host. No macOS leaks, ps inspection, Linux, Windows, wasm or Nix execution proof is claimed.

**Review 2 findings 1 and 2: PASS for this assigned macOS host repair.** P2-I2/P2-I3 now cover the reviewed provider-parser gap and the owned ACP child lifecycle. Ten behavioral mutations are recorded in [mutation-results.json](artifacts/review-2/mutation-results.json), including the three regression-restoring failures. Broader phase-2 review/performance/platform obligations remain coordinator-owned.

Final reconciliation: all 729 source hashes match; all 64 production exports match `abi/libtny.exports.macos`; public headers and existing ADRs are unchanged; `git diff --check` passes. The [changed-file inventory](artifacts/review-2/files-changed.txt) lists the uncommitted deliverable. No commit, push, PR or live provider call occurred.
