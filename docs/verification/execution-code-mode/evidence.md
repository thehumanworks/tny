# Execution code-mode evidence

Date: 2026-09-25. Baseline: `4e4d270`; branch:
`feat/execution-server-code-mode`; review: PR #193.
This ledger distinguishes completed observations from historical failures and
from unsupported capabilities. The PR checks identify the exact revision of
hosted results; a passing check on an earlier revision is not a later revision's
aggregate result.

## Implemented acceptance boundary

The production CLI/TUI and verified ACP bridge expose only `run_code`. Each
native cell starts the matching executable over a private inherited socket;
tool preparation and effects run in that execution process. Owner callbacks
mediate policy, hooks, events and state, not a second direct tool dispatcher.
The [contract](contract.md) maps requirements to concrete tests, and
[ADR 0174](../../adr/0174-execution-server-code-mode.md) describes the design and
compatibility limits. AIM's harness/executor and code-mode implementation were
studied before selecting tny's native C/Lua approach; its parallel direct mode
was not retained.

## Native regression inventory

A complete immutable integration inventory was enumerated from
`tests/integration/run.sh` and partitioned across eight independent native
worktrees, all at tree `55c5a8c7257fad30c27c4015fb276c5972e9ea29`
(the tree committed as `ae6640f`). Every normal `make test` prerequisite passed
in every shard. All **101 integration groups** executed: 100 passed, and the
background-agent image fixture failed because it still attempted the explicitly
removed shell-to-owner image attachment path.

The corrected background fixture invokes typed `read_image` inside the same
code cell before its parked terminal operation. Its complete script then passed,
retaining same-runner/socket/owner-lock assertions, background reattachment,
exact captured bytes after the source file changes, and no replay. No raw image
path was restored. One otherwise-passing shard detected generated site HTML
changes from its site build; those were build-output version/size substitutions,
not source/test edits. That shard's drift receipt was not relabelled as an
unchanged snapshot pass.

The older evolving-tree aggregate (100 groups, 29 failures) remains historical.
Its failure reconciliation and all subsequent checks informed repairs, but it
is not presented as a green aggregate. Likewise, the partitioned inventory plus
focused repairs is not represented as a new serial `make test` exit 0. Hosted
full-suite results on the PR provide a separately revision-bound aggregate.

## Completed CI repair gates

The following local checks completed on the CI repair source committed in
`7ca1988`, using synthetic credentials and loopback providers:

| Gate | Observed result |
| --- | --- |
| Native release / unit / quality | `make -j6 quality test-unit` exited 0 on an unchanged 1,382-file input manifest; 585 unit tests, 584 passed, one platform skip, 32,572 assertions; two extra restricted-profile regressions passed |
| Native lifecycle | 43/43, no skips; raw/code refusal, retained checkpoint/steer, same-engine recovery, private pending ownership |
| Native request allocation ownership | All 17 discovered request-construction indices passed |
| Native mutations | All 16 compiled mutants killed; NDEBUG live-lease abort guard and guardless baseline checked |
| Provider refusal allocation matrix | Eight wire/context/proposal scenarios; every discovered allocation index injected with single settlement, no callback effects and same-engine recovery; 400 injections on the recorded macOS build |
| Python SDK target | 107 tests, 106 passed and one existing artifact-dependent skip; executable conformance accepted |
| TypeScript SDK target | 60 tests, 59 passed and one existing legacy-library skip; executable conformance accepted |
| SDK conformance | Each adapter reported 8 passing scenarios and 2 explicitly unsupported permission scenarios; unsupported is not pass |
| SDK types and style | Strict mypy (11 source files), strict TypeScript checking, Ruff and all 42 JavaScript syntax checks passed |

The first hosted run exposed failures that local feature-only tests had missed:

| Hosted failure | Reconciliation |
| --- | --- |
| Valgrind: ten native fixture failures, 343 reported errors | The provider-fault executable needed the same private executor/guardian entry points as the main binary. Two remaining tests assumed removed embedded direct/pending execution. Replaced those assumptions with explicit refusal plus independent private ownership coverage; did not add suppressions |
| Parser/backend ownership on Linux/macOS | The same unsupported checkpoint/pending fixtures; revised lifecycle tests preserve the supported invariants and allocation checks |
| Wasm execution refusal test | The binary correctly returned an explicit platform-unavailable/no-fallback error; the fixture now checks that actual contract and zero effects |
| SDK model callbacks and standalone optimisation | Public model tools refuse. Standalone optimisation also refuses before I/O because the embedding ABI has no trusted executor launcher; Python/Node must not be re-executed as tny |
| Linux quality | GNU spawn declarations now come from early compiler feature flags, not a late source-level reserved-macro definition |

On `7ca1988`, hosted CI run `36183006796` observed **Valgrind success**:
regular unit/CLI leak checks, native request ownership, and all 43 provider
lifecycle cases passed. Reported summaries contain **zero errors and zero
suppressed errors**. The same revision's wasm/node and browser smoke passed.
That run also exposed additional Linux packaging/build issues, handled below;
its whole matrix was therefore not green.

## Linux portability regressions

Enabling GNU spawn declarations for native C revealed two interactions. The
existing glibc compatibility header now also precedes C and forced allocation
headers, preventing accidental `__isoc23_*` imports that raised the published
2.34 compatibility floor to 2.38. The release compatibility checks remain strict.

Musl's GNU `sched.h` redeclares `calloc`; it must be loaded before allocation
call-site aliases. Actual `src/lib/tny.c` compilation under musl-gcc failed before
the early scheduler include and passed afterward. The mixed-language build
suite now includes a negative compile fixture: removing that include makes the
same declaration fail. It also checks early compatibility flags across C/C++
release and fault-library commands, then compiles the real compatibility header.
The complete build suite passed 19 tests with one explicit Emscripten skip on
macOS. Hosted Linux/musl artifact checks remain the authoritative end-to-end gate.

## Independent review and follow-up

A read-only Claude Fable review checked the actual authority/protocol source.
Its actionable settlement, permission-event, policy-stop and stdio MCP teardown
findings received focused before/after regressions in a separate snapshot.
The session-size and host-capability findings are explicit limits in ADR 0174.

The follow-up source passed 17 production execution cases (16 native passes,
one wasm-only skip), seven runner-permission cases, two guardian cases, the
30-case MCP suite, and the ACP deadline fixture. Its complete native unit gate
passed 588 cases (587 passes, one platform skip; 32,596 assertions). Scoped
Clang-Tidy/strict warnings, style and the unchanged source-linked protocol proof
also passed. These are separately recorded snapshot checks; the combined
native gates and hosted PR checks verify integration with the portability fixes.

The delayed-ACK tests use an actual private execution child and an owner that
persists the received state before delaying acknowledgement. They prove a
bounded non-executing settlement phase, not an fsync performance guarantee.
A delayed mid-cell acknowledgement cannot authorize a second write after the
code deadline; a final acknowledgement delayed beyond its separate bound fails
without replay. Policy-stop tests preserve completed effects and stop a Lua loop.
The stubborn-MCP-child test fails before the close fix and passes afterward,
while cooperative EOF, idempotent close and nonchild protection remain checked.

## Final CI reconciliation

Linux Clang-Tidy found an uninitialized socket-address read in the private
entrypoint. The peer address is now zero-initialized and its returned length
must contain the family field before it is inspected. Production entrypoint
regressions reject pipes, Unix datagrams and connected TCP sockets for both
private modes; valid Unix stream protocol tests remain. The complete execution
suite then passed 18 cases (17 native passes and one wasm-only skip).

The C reference conformance adapter still expected embedded permission prompts
although the Python and TypeScript adapters had already migrated. It now probes
actual no-fallback refusal and zero effects under all three permission modes,
reports the two unavailable permission scenarios as unsupported, and retains
the other eight passing conformance scenarios. The release validator remains
unchanged and continues rejecting unsupported *applicable* scenarios.

The combined native gate (`quality`, unit, native mutations, runtime ownership,
allocation faults and formal checking) passed on the reviewed source. Its
subsequent extension integration exposed an old expectation that a policy-stopped
code cell was successful. The corrected test requires its completed nested
operation to remain successful, both cancelled/unstarted code cells to fail,
and exactly one provider request; the complete extension script passes.

An independent Debian Linux run of the repaired provider lifecycle binary
passed all 43 cases under Valgrind 3.22.0. The sandbox's original Valgrind 3.19
could not handle pidfd_open and produced unsupported-syscall failures; that
instrumentation result was not called a production memory defect or a pass.
The supported Valgrind run and the hosted job are separate actual observations.

## Source-linked formal evidence

`make verify-formal` checks the actual Clang AST of the pure admission predicate
with fixed-width Z3 bitvectors: seven universal obligations over all 192 input
bits, a satisfiable non-vacuity witness, and 9,600 compiled representative cases.
Ten existing abstract obligations also pass. Predicate SHA-256:
`e7a1b727207551197fe372d46dd320b4b6fd8b57021a75a82ce032dc5c2d910b`.

See [protocol proof](protocol-proof.md) for the exact trusted base and exclusions.
This is **not** proof of parsers, C memory safety, callback implementations,
filesystem policy, process cleanup, or exactly-once external effects. Split
frames, malformed input, actual execution PIDs, permissions, allocation failure,
server loss, and no replay are checked by executable tests instead.

## Explicit compatibility and evidence limits

Wasm and public embedded/custom/host-callback agent tools are unsupported;
there is no direct fallback. Standalone SDK optimisation is also unsupported
before provider, credential or workspace I/O; native CLI/TUI optimisation remains
available. SDK registration, no-tool inference, lifecycle, cancellation and
supported standalone services retain independent coverage.

ACP requires the verified strict tools-only adapter. MCP connections are
cell-scoped. The complete initial snapshot, including stored session history,
is capped at 8 MiB; compaction does not delete that history. Native execution
requires the supported Linux/macOS process-identity seam. These are operational
limits, not hidden compatibility claims.

The guardian's execution-server-death tests cover its owned shell tree. They do
not establish cleanup of arbitrary MCP peers after external executor SIGKILL,
identity/allocation failures, or deliberately escaped descendants. Completed
effects are not rolled back. No live provider inference, speed improvement,
token saving, or general operating-system sandbox guarantee is claimed.

Raw local logs, atomic statuses, input manifests and the full integration plan
are retained in `/tmp/tny-execution-code-mode` on the development host. Hosted
results and their source revisions are visible on PR #193.

## Combined local delivery check

The final main-checkout `make -j6 quality test-unit lib-shared-active` exited 0,
with 588 unit cases (587 passed, one platform skip; 32,596 assertions) and both
extra restricted-profile regressions passing. All executable, build and test
inputs in its 1,510-file manifest remained unchanged; only the conformance
README was clarified during the run. A separate final macOS `make leaks` exited
0 with zero leaks. The final C reference report passed eight applicable
scenarios and explicitly classified two embedded permission scenarios as
unsupported. The complete extension integration and all 18 execution cases
(17 native passes, one wasm-only skip) passed after the final repairs.

The earlier combined gate also passed all native mutations, runtime ownership,
allocation-fault sweeps and formal obligations on the reviewed execution source.
The final repairs add socket metadata initialization, conformance assertions and
policy-stop fixture corrections; no allocation failure injection, negative
validator scenario or memory-check diagnostic was suppressed.
