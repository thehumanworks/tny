# Evidence: C++ phase 1

Contract: [contract.md](contract.md). **INCOMPLETE**: parser implementation and
focused macOS checks pass; full host/platform, review, performance and local
commit gates do not all pass. No scope reduction is claimed.

Assigned worktree: `/Users/tomas/projects/tny-cpp-parsers`, branch
`migration/cpp-parsers`. Initial and current Git HEAD:
`c6d938a0846e0cabd7235b56468abc5cf65cfaf5` (contracts on main baseline
`1d8ad71d66c06c726b3c5b35e367fec678031e85`). Initially clean. Implementation is
uncommitted because the sandbox denies writes to this linked worktree's Git
index under the protected main checkout. `git add ... && git commit -m
'docs: authorize private C++20 parser ownership'` failed 128 creating
`index.lock`; the commit command was not reached. No push, PR, other worktree
change, phase-2/3 edit, or subagent was made.

The initial contract preceded implementation and compared byte-for-byte with
`contract.initial.md`. That snapshot and the live contract remain unchanged.
The series contract owns goal orchestration; the assignment reuses its recorded
native-goal limitation and leaves independent reviews to the coordinator.
ADR 0114 was allocated by the user and written before production conversion.
All 116 pre-existing ADR files match [the initial hashes](artifacts/adr-before.json).
ADR 0115, `tests/bench/`, and `docs/size-and-speed.md` were not edited.

## Delivered ownership and caller inventory

| Surface | Owner and lifetime | C consumers / result |
| --- | --- | --- |
| SSE | `src/net/sse.cpp`: noncopyable state, allocator-backed line/event strings; no allocation for empty feed; sticky OOM | OpenAI/Codex `openai.c`, search `search_codex.c`, unit/fuzz tests; callback bytes expire on return; free/reset destroys the owner |
| Connect | `src/net/connectrpc.cpp`: noncopyable header/payload state; checks declared length before payload growth | `cursor/rpc.c`, `cursor/sdk_client.c`, network/Cursor tests; explicit pending-frame accessor preserves truncated-final-frame handling |
| Event JSON | `src/backends/openai/events.cpp`, `src/cpp/owners.hpp`: move-only yyjson document with correct deleter | `openai.c` synchronous decoded-event sink; retained reasoning/hosted values are deep-copied by existing C capture helpers |
| Tool fragments | `toolcalls.cpp`: noncopyable records in allocator-owned state; owned ID/name/arguments, C read-only views | Chat/Responses decoding, C tool execution and checkpoint restore/readback; restore now goes through `oa_calls_set` |
| Allocation / exceptions | `tny::allocator`, object factory and no-throw deleters; explicit `tny_alloc_malloc`, no global new override | All C++ allocation failures return -2 at private C boundaries; event parsing returns 1 for malformed JSON, distinct from OOM |

The initial caller search covered named functions in openai/search/rpc and
unit tests. Compilation additionally exposed `sdk_client.c`'s direct access
to the old Connect accumulator; it was replaced with `connect_decoder_pending`.
`responses.c` is request translation and stays C: actual Responses event
parsing was extracted from `openai.c` into `events.cpp`. Scheduling, retry,
checkpoint policy, tool execution and the event loop remain C. Public headers
and export/layout contracts are unchanged. Transports, chunk decoding, ACP,
MCP, vendored libraries and tnytty remain C.

Private C++ objects are noncopyable, document handles are move-only, and C
facades are explicitly noncopyable by contract. Destructors do not allocate or
throw. No credential is copied into parser owners. The Cursor request adapter
also wipes its existing temporary authorization buffer after use.

Limits: Connect retains **64 MiB per decoded frame** (limit-1/limit/limit+1
exercised with real payloads). Encoding rejects lengths not representable by
the uint32 wire field before reading payload memory. Search retains its 2 MiB
wire/64 KiB text limits; non-SSE OpenAI bodies retain 1 MiB and provider error
bodies 64 KiB. SSE and tool-argument accumulation remain allocation-limited;
no new universal payload cap is imposed. Growth arithmetic is checked before
container append. Tool call capacity remains 32, with excess calls ignored as
before; fresh IDs precede reused indices, and index-only continuations select
the most recent matching call.

## Build and quality coverage

The Makefile discovers C++ sources independently, uses C11/C++20 per-language
flags and `.cpp.o` dependencies, and links mixed artifacts with CXX. This
covers release, debug/unit/mutation, PIC/shared, allocation-fault,
ASan/UBSan, TSan, both libFuzzer drivers and wasm via em++. Frozen ABI0 still
builds its unchanged C source archive. C++ uses explicit allocator calls rather
than the C force-included allocator macros. Quoted vendor include paths avoid
case-insensitive `VERSION` shadowing the C++ `<version>` header on macOS.

Format, clang-tidy and strict warnings include .cpp/.hpp; GCC -fanalyzer
explicitly stays C-only, with C++ covered by clang-tidy. The only new lint
exception is `performance-enum-size` on the private C-facing enum: its C11
layout must agree with C callers. No analyzer family was disabled. Three
pre-existing C++ test consumers newly discovered by format-check were formatted.
C++ strict warnings use `-Wmissing-declarations` instead of C-only prototype
flags, including GCC CI. Negative tests prove formatter/analyzer rejection.

CI native/musl/MSYS lanes run parser smoke; Linux Clang runs the bounded parser
libFuzzer. The wasm lanes use C++ exception catching. Nix sources include the
owners, driver and corpus; its test derivation runs parser smoke with explicit
C/C++ compiler selection. Release/SDK/Pages compiler selection and MSYS static
C++ runtime linkage are recorded in the changed workflows/Makefile.

## Execution records

Host: Darwin arm64; Apple clang/C++ 21.0.0 (clang-2100.3.27.1); GNU Make 3.81;
clang-format 23.1.0; clang-tidy 22.1.8; Python 3.14.7; Node 26.8.2.
`mise install` returned 0. No Linux, Windows, Emscripten or Nix result is claimed.

[run-records.json](artifacts/run-records.json) preserves gate commands, exit
codes, timings, tested HEAD and source deltas against
[final-source-sha256.json](artifacts/final-source-sha256.json). A dirty-tree
hash manifest, rather than the unchanged HEAD alone, identifies the tested
implementation. Raw local logs/manifests are in `build/phase1-logs/`; selected
parser and diagnostic tails are retained in `artifacts/`. Commands run in the
assigned worktree unless an explicit `make -C` is recorded.

Test environment: `TMPDIR=$PWD/build/tmp`. Apple compiler temporary files and
bare macOS mktemp otherwise select a sandbox-denied `/var/folders` path. A
local, uncommitted `build/phase1-bin/mktemp` shim supplies an explicit template
under TMPDIR only for bare/default mktemp requests; explicit templates and
product code are unchanged. Inherited provider endpoint/key variables and
TNY_TOOLS are removed for final fixtures, which supply their own synthetic
credentials and loopback endpoints. No environment workaround was added to
production or test source.

The coordinator identified inherited `TNY_TOOLS=terminal` and CURSOR_API_KEY;
earlier environment-induced fixture failures are discarded as product evidence.
A further inherited OPENCODE endpoint/key overrode the provider-setup fixture
and caused an unintended external request returning HTTP 401. No paid result
was obtained. Clearing all inherited `*_BASE_URL`, `*_API_KEY`,
`*_API_KEY_CMD`, and `*_WIRE_API` overrides makes that fixture pass. Subsequent
checks use those cleared variables. Secret values were neither printed nor
persisted. This incident is recorded rather than claiming all earlier calls
were local.

### Development checks and superseded runs

- Initial reads: AGENTS; product/architecture/implementation-plan/language/
  size/CI docs; phase-1/series contracts and evidence; ADRs 0110-0113; Makefile,
  allocator/JSON/parser sources and caller searches. No baseline worktree was
  inspected. Memory keyword lookup supplied no implementation decision.
- `make -j8 debug`: first failed 2 on a Make expression; next failed 2 on
  the VERSION/header collision; corrected build passed. The first SSE slice's
  `build/tny-test -s net_suite` passed (16 tests/1542 assertions, ASan/UBSan).
  Self-review checked synchronous views, checked growth, reset/free and sticky
  OOM before extending the pattern. This does not count as independent review.
- Subsequent debug builds exposed stale C rename dependencies (fixed with
  distinct .cpp.o objects) and the Cursor SDK direct accumulator access. Final
  OpenAI/network units passed; final full unit binary has 553 passing tests.
- First quality runs failed on newly discovered unformatted C++ consumers,
  deleted-but-unstaged tracked sources in format discovery, and the C ABI enum
  size diagnostic. These were fixed without disabling an analyzer family.
- One quality run overlapped a header rewrite and cached a partial net.h;
  its parse errors are invalid/superseded. `quality-frozen` reran with sources
  held stable. Do not use the overlapping run as final quality proof.
- Default-temp release builds failed 2 (`LLVM ERROR: IO failure on output
  stream: Bad file descriptor`). A minimal C-only LTO probe also failed temp
  creation. Setting TMPDIR in the worktree fixes release with LTO retained.
- ABI `BUILD=build/abi-check` failed 2 because existing ABI tests hardcode
  `build/pic`/`build/lib`; rerun used the default build path. The default ABI
  aggregate still fails unrelated toolkit image operations with IO (-7).
- A read-only `git archive 1d8ad71` source snapshot under
  `build/phase1-baseline` (not another worktree) built `lib-shared-active`
  successfully. Its unmodified `tests/abi/test_toolkit.py` also fails seven
  image-operation cases with IO (-7). Current ABI/SDK toolkit failures remain
  unmet. Source inspection shows image writer guards use ~/.tny/image-guards,
  outside this sandbox's writable roots, before making the provider request;
  that is a likely cause, not a traced failure attribution. No user-home state
  was changed or test failure suppressed.
- `test_nix_ci_matrix.py` and `test_toolchain_pins.py` pass. The
  `test_windows_lto_flags.py` host-only recipe test passes with TMPDIR set;
  this is not a Windows build/run. `git diff --check` and ADR hash comparison
  pass. No phase-2/3, benchmark or size-policy file changed.

### Latest local gate status

See run-records for exact command variants and chronological failures.
The candidate-specific OpenAI/Cursor commands set
`TNY=$PWD/build/parser-candidate/tny`; the release candidate is built from the
final source separately so it does not overwrite an active fixture binary.

| Command | Exit | Scope/result |
| --- | ---: | --- |
| `make -j8 release BUILD=build/parser-candidate` | 0 | Final native release; 1,087,392 stripped bytes |
| `build/tny-test` | 0 | 553 tests, ASan/UBSan |
| `make quality` | 0 | `quality-frozen`, all local quality checks; GCC analyzer explicitly unavailable on Darwin |
| `make test-cpp-gates warn-strict test-parser-smoke` | 0 | Positive discovery, both negative controls, portable smoke |
| `make -j8 test-libtny-fault test-libtny-fault-sanitize` | 0 | Exhaustive library sweeps, repeated OOM then success, sanitizer host |
| `make -j8 test-libtny-fuzz-smoke` | 0 | Existing ABI corpus and positive/negative class checks |
| `python3 tests/mutation/parser_critical.py` | 0 | All four intended kills; baseline restored/unmodified and passing |
| `python3 tests/integration/test_openai.py` | 0 | Final candidate, Chat/Responses golden mocks |
| `tests/integration/test_cursor.sh <candidate>` | 0 | Final candidate bridge stream/resume/options |
| `python3 tests/integration/test_cursor_management.py` | 0 | Final candidate RPC/stream/error/safety mocks |
| `python3 tests/integration/test_search_service.py` | 0 | 17 independent search tests |
| `python3 tests/integration/test_native_search.py` | 0 | Native hosted-search mock flow |
| `tests/integration/test_provider_setup.sh <tny>` | 0 | Cleared provider environment; local fixture only |
| `python3 scripts/check_abi_baseline.py --candidate build/abi/libtny-v1-current.json` | 0 | ABI-1 baseline comparison |
| `python3 scripts/check_abi_baseline.py --compat0 abi/compat0.json` | 0 | Frozen ABI-0.8 manifest |
| `make test-abi` | 2 | Toolkit image IO failures; aggregate not green |
| `make test-sdks` | 2 | Python toolkit image IO failures; aggregate stops there |
| `make test-sdk-typescript` | 2 | Separately run; toolkit image cases fail before provider request |
| `make -j8 test` | 2 | Full 1271-second run: sandbox ps denials, toolkit IO, inherited provider override and subagent mock ps denials; not green |
| `make leaks` | 2 | Final rerun: macOS denied leaks task ports; no leak verdict established |

Expected diagnostic text from deliberate negative tests is not a compiler
warning. No new compiler warnings remain in passing build/quality runs.
The broad suite also prints existing expected invalid-input warnings and
recursive jobserver warnings; it is not presented as warning/error-free.

## Reviews

Pending.

## Mutation results

`tests/mutation/parser_critical.py` compiles private copies below
`build/parser-mutations`, never writes production source, verifies source hashes
are unchanged and reruns the unmodified smoke. Each mutant compiles and links
successfully; none is counted as killed merely for a build failure.

| Mutation | Behavioral failure | Exit | Verdict |
| --- | --- | ---: | --- |
| Disable 64 MiB decoder guard | `64 MiB frame-size check` at limit+1 | -6 | killed |
| Let reused index override fresh ID | `fresh ID must take precedence over repeated index` | -6 | killed |
| Retain JSON document ID pointer | ASan `heap-use-after-free` after document destruction | -6 | killed |
| Return success on Connect OOM | `OOM must not be swallowed as success or malformed JSON` | -6 | killed |

Source hashes and outcomes: [mutation-results.json](artifacts/mutation-results.json).
ASan/assertion logs are retained alongside it. Smoke sweeps cover 2 Connect,
9 Chat and 13 Responses allocation indices, injecting each twice before a
successful later decode. Real libtny next_event sweep covers 171 allocation
indices; its existing harness verifies reserved OOM ERROR and exactly one
TURN_END plus repeated-turn recovery. The portable driver also covers every
SSE/Connect split, one-byte fragments, CRLF/multiline/UTF-8/empty/truncated
input, keepalives/trailers, retained call lifetimes, fresh/reused/omitted indices,
32-call boundaries, malformed JSON and Connect limit neighbors.

## Unmet platform and integration gates

These are required, **unrun** here, and must be collected on the integrated
source. Source discovery/dry recipes are not runtime proof.

| Environment | Required CI commands |
| --- | --- |
| Linux glibc x86_64/aarch64 | `make test CC=gcc CXX=g++`; `make quality ANALYZER_CC=gcc-14`; `make warn-strict CC=gcc-14 CXX=g++-14`; `make warn-strict CC=clang CXX=clang++`; `make valgrind`; ABI/SDK/fault/sanitizer/smoke gates |
| Linux Clang fuzz / TSan | `make test-parser-fuzz FUZZ_CC=clang FUZZ_CXX=clang++ FUZZ_RUNS=20000 FUZZ_SECONDS=60`; `make test-libtny-fuzz`; `make test-libtny-tsan CC=gcc CXX=g++` |
| Linux musl x86_64/aarch64 | `make release STATIC=1`; `make test-unit test-parser-smoke SANITIZE=0 STATIC=1`; `make size-check SIZE_MAX=1572864 STATIC=1`; packaging/smoke |
| Windows MSYS native x64 | `make release`; `make test-unit test-parser-smoke SANITIZE=0`; existing jobs/MSYS fixture commands; `make size-check SIZE_MAX=2097152`; packaging/smoke |
| Emscripten node/browser | `make wasm wasm-web wasm-dictation-fixture EMCXX=em++`; `TNY=$PWD/build/wasm/tny python3 tests/integration/test_openai.py`; `tests/integration/test_acp_ws.sh $PWD/build/wasm/tny`; Codex/search and other existing wasm fixtures in ci.yml; `make wasm-size-check`; `python3 tests/integration/test_site_wasm.py` |
| Nix x86_64-linux/aarch64-linux/aarch64-darwin | `nix flake check` (same C/C++ Makefile; test derivation includes parser smoke) |

The remaining local host gates need an environment that permits process
inspection and macOS leaks task ports, and resolution/reverification of the
baseline-reproduced toolkit IO failures. Commit/index access is also required.
Coordinator owns independent design/first-slice/risky-boundary reviews,
benchmark/size policy integration, final combined checks and local delivery.
The Swift module-cache failure in the broad run is resolved by test environment
`CLANG_MODULE_CACHE_PATH=$PWD/build/module-cache` and
`SWIFT_MODULECACHE_PATH=$PWD/build/module-cache`: a separate final
`python3 tests/integration/test_libtny_custom_tools.py` returns 0, including its
C/C++/Swift callback/header checks. No code workaround was added.
A cleared-environment `test_subagent.py` rerun fails inside the mock's
`child_processes()` because spawning ps is denied; the mock then closes the
connection and the child reports exit 2. This is direct fixture evidence, not
an inferred parser error. Process cleanup checks that require ps are unverified.
The source-only baseline was also built as a CLI (exit 0); no performance
comparison or additional baseline behavioral claim is made from that build.
No passing subset satisfies the complete phase-1 contract.

## Final reconciliation

| Invariant | Evidence and remaining condition |
| --- | --- |
| P1-I1 | Owners, deterministic C++ allocation sweeps, ASan/UBSan and mutations pass; leak gate unmet |
| P1-I2 | Local unit/corpus/OpenAI/search/Cursor bytes/events/IDs pass; required other-platform fixtures unmet |
| P1-I3 | Explicit arithmetic/limits, 64 MiB neighbors, 32-call boundary and parse/OOM checks pass locally |
| P1-I4 | Library fault recovery and ABI manifests pass; aggregate ABI/SDK toolkit gates remain red |
| P1-I5 | Mixed lanes, discovery/negative controls and local quality pass; native non-Mac/wasm/Nix execution remains unverified |
| P1-I6 | Assigned to ADR 0115/benchmark owner; not measured by this assignment |
| C0 | Existing preimplementation snapshot equality and ADR baseline verified |
| C1 | Reserved for coordinator; self-review does not satisfy independence |
| C2-C4 | Local results above; four critical mutants killed; full gate set incomplete |
| C5 | Benchmark owner/coordinator pending |
| C6 | Existing ADRs unchanged; final source manifest retained; commits/integration/cleanup not achieved |

For ADR 0115/bench tooling: coordinate edits to **Makefile** (compiler/link and
new parser targets) and **docs/ci.md**. Runtime dependency is libc++.1.dylib on
this Mac, plus libSystem; no startup/throughput improvement is claimed. The
candidate is `build/parser-candidate/tny`. Parser APIs are in `src/net/net.h`
and `src/backends/openai/parsers.h`; implementation in `src/net/*.cpp`,
`src/backends/openai/{events,toolcalls}.cpp`, `src/cpp/owners.hpp`.
`tests/fuzz/fuzz_parsers.cpp` and `tests/fuzz/parser-corpus/` provide deterministic
inputs; the leading corpus byte selects Connect=0, Chat=1 or Responses=2.
No benchmark, size-policy file or ADR 0115 was created or changed here.
