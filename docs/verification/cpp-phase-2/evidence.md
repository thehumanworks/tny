# Evidence: C++ phase 2

Contract: [contract.md](contract.md). State: **INCOMPLETE** pending coordinator
reviews, outside-sandbox/platform gates and performance proof. Local
ownership conversion covers plan items 1–6, but the whole-provider allocation-free
settlement obligation in item 3 remains incomplete; passing subsets are not
issue completion.

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

Pending.

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

- **P2-I3 implementation gap:** the runtime's event construction/queue/reserve
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
P2-I3 has the explicit provider-cancellation allocation gap above; required external
leak/platform/TSan/aggregate ABI-SDK proof is outstanding. P2-I6 has only the
allocation/size observations above. C0 timing is recorded; C1 reviews are
coordinator-owned; C2–C6 remain incomplete wherever external proof or delivery
is required. No requirement or unavailable gate is silently treated as passed.
