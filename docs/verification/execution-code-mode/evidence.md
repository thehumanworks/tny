# Execution code-mode evidence

Working-tree verification on `feat/execution-server-code-mode`, based on
`4e4d270`. This page records observations, not inferred completion.

## Added verification

- `tests/integration/test_execution_code_mode.py`: production binary with
  loopback Chat/Responses providers, exact singleton schema, file read/write,
  direct-call refusal, Lua ambient-API restriction, JSON codec, profile ceiling,
  runtime bounds and observed execution-child crash without replay.
- `tests/integration/code_mode_fixture.py`: explicit code wrappers for legacy
  fixture scenarios. Streaming conversion retains fragmented arguments and
  Responses added/done metadata cases.
- `tests/integration/test_execution_permissions.py`: owner permission,
  cancellation, reattachment and active image-control admission cases, including
  an owner claiming a vacant background session; all seven cases pass.
- `tests/integration/test_execution_library.py`: real public C ABI native and
  verified ACP refusal, with a registered host callback invoked zero times.
- `tests/test_main.c`: registers runtime, protocol, transport and state suites.

## Current observations

Python syntax compilation of all integration files and Ruff checks passed.
`test_nix_ci_matrix.py` passed. `test_make_contract.py` passed 5 tests;
`test_cpp_build.py` passed 16 tests with its Emscripten check explicitly skipped
on this native host. These checks cover build/test wiring and syntax.
The first production release run of `test_execution_code_mode.py` passed 11
native tests with one explicit wasm skip (6.633 seconds). It exercised both
HTTP wires, actual file effects, the real execution child and crash/no-replay,
limits, hooks, profile separation and malformed private protocol input.

Additional observed native checks: prompt-cache 17/17; MCP CLI 11/11; terminal
background all profiles and cross-session collection; terminal cancellation
with shell, grandchild and setsid descendants and an unrelated sentinel. The
migrated native-search suite passed all seven cases (four nested service modes,
two unsolicited hosted-search refusals, and fetch compatibility).

These are working-tree observations, not a clean aggregate gate. The final
measurements below distinguish completed gates from the original failing
aggregate; focused passes do not turn that earlier aggregate into a pass.
No live provider, hosted CI or performance improvement is claimed.

## Explicit acceptance risks

The migration changes every agent tool call. Tests now exercise native images,
terminal collection, managed-agent authority and hooks through code. Embedded
model calls explicitly refuse execution; their fixtures assert zero effects and
preserved ownership rather than claiming callback execution support. Fresh
executable reconstruction cannot copy pointers or live in-memory resources.

Wasm has no native execution process and must report a clean unsupported error.
No native/wasm tool parity is claimed for this migration.

## Current integration status

The previously identified native optimisation, oversized ACP image handling,
permission-stop audit and native nested-event projection defects are repaired
and have passing focused reruns:

- Native optimisation: 19/19, including local and SSH reads and denied writes.
- ACP client: 31/31; oversized image state returns the explicit 4 MiB error.
- ACP managed: 19/19, including readonly policy, independent admission,
  cancellation, teams and real subagent inheritance.
- Owner permissions: 7/7 on the final release, including cross-cell grants,
  six-second human wait, matched reply IDs, cancellation and reattachment.
- Extensions: complete integration script passed after matching nested audit
  entries separately from wrapper entries.
- Collective mailbox contention: both targeted regression tests passed.
- Subagents, ephemeral propagation, background lifecycle and benchmark fixture
  migration passed focused checks. Image input passed 8/8 and speech 10/10.

The full aggregate gate ran **100 integration groups and failed 29** while
fixture migrations were in progress. Focused reruns cover the corrected behavior
in **all 29 groups**, including the subsequently migrated SDK and repaired
allocation-fault groups below. There has been no subsequent green full aggregate.
Earlier failures in its log refer to inputs since repaired; that log is not a
clean final aggregate result. Image queue checks now pass 16 native cases (one explicit wasm skip), including
active shell-control refusal and typed-preview cancellation/resume without old
bytes. Job-artifact consumers pass 14 cases, including real pending selection
ownership across approval, job/manifest mutation and one-time grants. Typed
nested image tools retain their execution and queue tests. Final aggregate
acceptance remains pending.


## Latest focused repairs and remaining gaps

Public C ABI refusal passed 2/2 through actual native Responses and verified ACP
providers. Both received the explicit embedded/custom callback error, with zero
host callback invocations. The complete `test_libtny`, `test_libtny_acp` and
`test_libtny_custom_tools` scripts now pass with explicit embedded-refusal
assertions. Registration, text/streaming, repeated sessions, active cancellation,
cleanup and retained-event ownership checks remain. Model-driven asynchronous
callback completion cannot occur under this contract; private ownership tests
retain coverage of that machinery. This release has no embedded execution
backend or direct fallback.

The Chat allocation failure at index 60 exposed a real bug: failed allocation
during tool-argument replacement deleted the existing arguments member. The
replacement now stages both nodes before mutation. Its deterministic regression
failed before the fix and passes afterward (14 assertions).

The complete `make test-runtime-ownership test-libtny-fault` target now exits 0:
all 278 Responses and 191 Chat active-turn allocation indices and recovery checks,
21 other sweep scopes, and four reserved-settlement scenarios passed. Fault-only
mock responses coalesce headers and body into one write to stabilize allocation
counts; 100 discovery runs per wire were stable. Default fragmented-stream tests
are unchanged. No discovered allocation index or injected failure was dropped.
A stale nonexistent native test selector was removed; both actual regression
selectors now require at least one executed test and pass.

Focused repairs after the aggregate's earlier failures passed: MCP HTTP,
jobs permission cases 4/4 and catalog cases 2/2, purposeful swarms 14/14,
swarm factory 11/11, captured swarm parents 2/2, SSH,
complete TUI approval/steering/image checks, exact subagent diagnostics and owned
cancellation, and pending manifest permissions
7/7 (including the approval mutation matrix and allocation failures in the real
fresh execution child). Image queue passed 16 native cases with one wasm skip;
job-artifact consumers passed 14/14. The aggregate log spans fixture edits and
therefore cannot establish final source acceptance.


## Aggregate failure reconciliation

Every entry below failed in the recorded aggregate and subsequently passed the
stated focused check. A focused subset does not reclassify the full aggregate.

| Corrected aggregate groups | Subsequent evidence |
| --- | --- |
| `test_acp_bridge_deadline`, `test_acp_client` | Deadline fixture passed; ACP client 31/31 |
| `test_ask_events`, `test_background`, `test_bench_tools` | Complete focused scripts passed |
| `test_collective_swarm` | Both failed contention cases passed |
| `test_ephemeral`, `test_extensions` | Complete focused scripts passed |
| `test_execution_command` | 2/2 with the `build/tny` runner argument, exit 0 |
| `test_image_preview_queue` | 16 passed, one explicit wasm skip |
| `test_image_preview_workflow` | 21 cases, one optional skip |
| `test_image_service` | 13/13 |
| `test_image_workflow` | 96 cases, one optional skip |
| `test_intercept`, `test_isolation` | Complete focused scripts passed |
| `test_job_artifacts` | 14/14 |
| `test_jobs` | Permission 4/4 and catalog 2/2, covering all three aggregate failures |
| `test_manifest_permissions` | 7/7 with real fresh-child allocation instrumentation |
| `test_mcp_http` | Complete script, exit 0; modern/legacy discovery and calls retained |
| `test_purposeful_swarm` | 14/14 |
| `test_ssh`, `test_subagent_diagnostics` | Complete focused scripts passed |
| `test_swarm_factory`, `test_swarm_parent` | 11/11 and 2/2 |
| `test_tui` | Complete PTY script, including nested approval and steering |
| `test_libtny`, `test_libtny_acp`, `test_libtny_custom_tools` | Complete scripts pass; code-only embedded refusal, zero callbacks, retained lifecycle checks |
| `test_libtny_mutation_fault` | Full runtime-ownership and allocation-fault target exits 0; all discovered injections and recovery checks retained |

## Stable implementation gates

The coordinator recorded these results after the final production fixes:

- Unit gate: exit 0; 585 tests, 32,572 assertions, 584 passed and one skip.
- Final postfix quality gate: exit 0, including the final session fix and runner
  guard. GCC analyzer is explicitly skipped on Darwin.
- macOS leak gate: exit 0, zero leaks. Four nonfork transport cases are included;
  process suites retain the exclusions printed by the leak runner.
- Formal gate: exit 0; 9,600 compiled admission cases, seven universal
  source-linked predicate obligations plus nonvacuity, and ten existing abstract
  obligations. This does not prove the parser, process lifecycle or tool effects.
- Stripped native arm64 release: 1,486,336 bytes; runtime linkage to system
  libc++ and libSystem only. No speed or token-saving claim is made.
- After the last fixture edits, `make format-check lint-py` exited 0. This was
  read-only verification; no production source was reformatted.

Native Linux/musl runtime, browser backpressure, hosted CI and live providers
were not verified by this macOS run. The original aggregate returned nonzero, and no subsequent whole-inventory
result is claimed here. All its failed groups have passing focused reconciliation;
this does not replace a final aggregate observation.


The independent review reopened the production freeze to broaden the active
image-control guard to all roles. The new background owner regression found
that the existing role allowlist already rejects owner image controls before
the active-cell guard; no owner-role image-read bypass was reproduced. The
broader guard is defense in depth. The seven permission cases, native unit
gate, quality and leak gates subsequently passed on the changed revision.
