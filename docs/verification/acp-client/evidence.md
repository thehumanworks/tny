# ACP restoration evidence

Implementation base: `ee4aa6c9a81395c8260f7b9c1536aeba16027fd9`.
The feature is delivered on `feat/restore-acp-client`. Raw logs, atomic exit files,
commands and input SHA-256 manifests are in
`/Users/tomas/.cache/tny-acp-20260921/`. Each supervised gate has matching
`.log`, `.exit`, `.status.json` and `.manifest.json` files. Missing exit files
are incomplete observations, never passes.

The final full-suite run (`final-test3`) completed with exit 2. Earlier full
runs exited 2 in the background-agent and TUI modules. Trace review found that
the TUI stop was an outdated provider-list oracle, not yet the baseline defect.
The final full run also stopped at that obsolete palette expectation. After
adding the restored ACP entry to that exact expected list, the complete
TUI module rerun (`final-tui`, exit 1) reaches the same menu-overlay failure
reproduced on the baseline. The background-agent failure already matches it. The ABI 1.4 correction and its
artifact checks pass. Current native SHA-256 is
`3e118dff52b10cd75d9882dd3577076544ba822305f10579eb9cb856af753fdd`;
stripped size is 1,269,824 bytes versus baseline 1,203,504 bytes. Runtime
dependencies are system libc++ and libSystem on arm64 macOS. No speed claim is
made. The final delivery and live observations are recorded in `delivery-validation.json`
and `live-observation.json`.

## Delivery follow-up (2026-09-21)

The final live managed run exposed a remaining job-summary accounting bug:
ACP usage events correctly marked `tokens_reported: false`, but the job log
reader still treated their zero storage placeholders as measured usage. The
reader now respects that marker while retaining compatibility with older native
logs that omit it. The new managed regression fails against the prior binary
and passes after the correction (`delivery-usage-red`: expected exit 1;
`delivery-usage-green`: exit 0). Job token fields remain null and the unknown-item
count remains accurate. Five native usage/budget regressions preserve existing
accounting semantics.

This was the sole production-source change after the completed full-suite run.
The complete ACP protocol (28), managed execution (19), C embedding, unit, SDK/ABI,
quality and macOS leak checks are rerun on the delivery state; the follow-up
status summary is recorded in `delivery-validation.json`. The complete repository
integration inventory is not rerun after this isolated accounting correction;
its earlier full-run and exact baseline-failure evidence are retained below.

Final follow-up gates exited 0: quality, leaks, units (534 passed; one existing
skip), ACP protocol 28/28, managed 19/19, five native usage/budget regressions,
C embedding and SDK/ABI. The latest SDK run discovered 107 Python tests (one
optional wheel skip), 60 TypeScript tests (59 passed; one optional old-library
fixture skip) and 43 ABI tests. The old-library fixture had separately passed
against actual baseline ABI 1.3; a skipped optional rerun is not counted as a pass.

The final binary passed all live checks using the existing Claude account and
explicit Sonnet/low selection: seven real SSH/tool-authority checks; ten checks
for a writer followed by a read-only reviewer with one admission slot and
complete cleanup; and five checks for session/load across separate tny processes
over SSH. Both managed agents resolved to `claude-sonnet-5`, and the resumed turn
retained the prior conversation nonce. The observer requests raw SDK metadata
only; it does not inject strict tool configuration. Sanitized facts are in
[live-observation.json](live-observation.json). No account credentials or raw
connector metadata are committed.

The initial managed live answer check was an observer defect: it searched raw
JSONL instead of assembling split text_delta frames. The corrected checker tests
exact reconstructed answers. An initial resume fixture used a HOME path too long
for OpenSSH ControlPath; it failed before inference, and a shorter isolated HOME
allowed the final real resume test to pass. Neither is presented as product
success or hidden as a passing first attempt.

## Execution evidence

| Boundary | Completed evidence | Result |
| --- | --- | --- |
| Release/shared libraries and ownership | `final-build4`: release, shared/fault/sanitized libraries, subagent/checkpoint ownership, debug | 0; ABI 1.4, ASan/UBSan ownership |
| SDK/conformance/ABI | `final-build4`: test-sdks, event-schema, conformance-contract, test-abi | 0; Python 107 (one optional wheel skip), TypeScript 60 (including actual old ABI), ABI 43 |
| ACP transport/session/model/tools | `final-protocol-managed2`: test_acp_client.py | 0; 28 tests, including grouped unknown-model refusal |
| Managed execution | `final-managed18`: test_acp_managed.py | 0; 18 tests, including ordinary non-DAG jobs |
| C embedding/custom tools | `final-acp-cabi2`: test_libtny_acp.py | 0; exact custom schema, owned argv, sync/async/cancel/repeated turns |
| OOM settlement | `final-acp-oom`, `final-acp-sanitize3`: pending custom-tool failure | Both 0; plain and guarded, ASan/UBSan included, no settlement allocation or late completion |
| Permission deadlines | `final-build3`: test-acp-bridge-deadline | 0; six controllable clock scenarios, no five-minute sleep |
| Unsupported platform | `final-build3`: test-acp-wasm-seam | 0; three host-linked tests of actual wasm process seam and capability mask |
| CLI event stream | `final-ask-events2`: test_ask_events.py | 0; explicit new usage fields, envelope/order/backpressure/cancel |
| Mutation | `final-mutation2`: mutate.py --focus acp-client --test task_name_grammar_is_strict | 0; seven valid mutants killed by integration, zero survivors, invalids or timeouts |
| Full suite and TUI follow-up | `final-test3`: make test; `final-tui`: test_tui.py | 2 and 1; exact failure disposition below; 535 unit tests (534 pass, one existing skip), 29,614 assertions |
| Quality | `final-quality4`: make quality BUILD=build/acp-quality-final4 | 0; Darwin explicitly skips GCC analyzer |
| Leaks | `final-leaks3`: make leaks | 0; selected macOS suites and CLI probes report zero leaks |

The latest isolated native hash is
`3ffd9356d759fc053c77bd6c8f093877693077cde481ef1f02d5aa964a25ce14`,
with the same stripped size; it additionally requires authority for ordinary
ACP jobs. It passed 18 managed and 28 protocol tests. The delivery path now contains the identical binary. The plain-job test
fails in all four scenarios against the prior binary, establishing the
regression before the successful 18-case run. Its first isolated
protocol run lacked the Python extension host layout; staging the supported
archive layout fixed that fixture environment without changing assertions.

The managed tests execute real detached tny supervisors, session runners, MCP
relays and fake external agents. They cover dependency order, wait/result and
durable answer hashes, command/model/effort freezing, admission cap/FIFO/retry,
mixed-scope and changed-config rejection, native lead selecting ACP, credential
isolation, read-only write/shell denial under yolo, mismatched initialized child
identity before session creation, maxTurns/new/load, collective and manifest
swarms, context replay after failed prompts, caller detachment, descendant
cancellation and uncertain-cleanup holds. Counted command snapshots preserve
literal delimiter and empty arguments; equivalent profile aliases inherit the
same frozen configuration.

Protocol tests compare the complete current tool descriptions and parameter
schemas, including all/terminal profiles, imported MCP and custom SDK tools.
They exercise split frames, malformed/oversized data, EOF/crash, missing methods,
authentication errors, model/capability refusal, permissions, timeouts,
cancellation, images, hooks, file/shell execution and SSH path isolation.

## Live account evidence

The earlier production SSH observation passed all seven checks
against native SHA-256
`4210318cedd38d7847db28faae5919a879e4f50b00d71eb96e7f9343a4a8a534`.
The ordinary observer did not inject strict MCP configuration. Actual primary
model was `claude-sonnet-5`; the SDK also used an internal Haiku sidechain.
All 26 advertised tools belonged to tny, and three true MCP executions produced
exactly three authoritative tny tool calls. Real SSH output matched exactly and
the local decoy was unchanged. Production strictMcpConfig and typed cumulative
USD cost were observed. This is earlier-source production proof. The completed delivery follow-up above
now records final managed, SSH and resume validation on the current binary.

Earlier local/SSH calls proved routing and Sonnet selection but did not prove
exclusive tools. The first parent remote attempt found extra account connectors;
the second prototype injected strict configuration and is not production proof.
Parent `parent-live-permission.report.json` also passed all six checks on that
earlier binary: safe read, denied write without side effects, blocked status,
actual Sonnet, sole tny policy and exclusive tny tools. The separate remote
default-permission probe blocked writes but did not complete its read; it is
not a fully passing read test. Raw account metadata and connector names remain private. The interrupted first
independent review is not a successful result; its later completed review and
dispositions are recorded separately.

## Failures, corrections and limits

- Parent's untouched base `ee4aa6c` completed full make test with exit 2:
  `test_background_agents` failed `locked_saved_inspection_retry`, and
  `test_tui` failed `test_menu_overlay_transient`. Baseline quality passed.
  These exact failures are compared individually with final results; module
  names alone are not sufficient evidence.
- The first full feature gate found an outdated hardcoded CLI usage payload
  oracle. It now names and checks the three additive fields explicitly;
  unknown fields still fail. The full module rerun passed.
- The first final mutation run killed six of seven mutants. Its survivor exposed
  missing grouped unknown-model negative coverage; adding that case killed all
  seven on rerun. Timeouts are never counted as kills.
- SDK Python/TypeScript tests and conformance passed before ABI review, but the
  artifact gate caught four new exports incorrectly registered in the frozen
  node. They now belong to additive ABI 1.4; active manifests are updated and
  old struct layouts/signatures/nodes remain unchanged. Final artifact checks passed (43 ABI tests and both active/frozen comparisons). The rebuilt Node addon passed a
  real native usage turn against the untouched baseline ABI 1.3 library
  (`final-ts-legacy`, exit 0), including explicit ACP rejection. Python
  separately tests that ABI 1.3 never resolves the new symbol names.
- An initial sanitizer command pointed to an unlinked library path and failed.
  After linking the actual sanitized library, final-acp-sanitize3 passed.
- The full suite exposed a wasm fixture object in the native product object
  directory, causing duplicate process symbols in the workspace-control driver.
  The fixture now has a separate object directory; the unchanged 13-case
  workspace-control module passes.
- A native-profile oracle expected the word profile although the correct error
  names an undefined ACP provider and its settings path. It now asserts those
  specific diagnostics; the whole native-profile module passes without vendor
  execution. These corrections are included in the final full-suite rerun.
- The TUI palette oracle omitted the restored ACP provider. Only its exact
  expected list changed. The full TUI rerun then reproduced the untouched
  baseline menu-overlay assertion; no production TUI code was repaired and
  no failing assertion was weakened or skipped.
- Actual emcc/browser wasm, Linux GCC analyzer/Valgrind and hermetic Nix checks
  are not established by macOS host-seam tests. emcc is unavailable locally.
  macOS leaks intentionally omits process-spawning suites per the existing
  repository leak contract. Wheel-install conformance skips unless its artifact
  is supplied; source SDK conformance is separate.
- pi adapter live compatibility remains unverified. Standard ACP lacks native
  mid-turn checkpoint/steer/compaction and provider-wire hooks. Generic agents
  may retain external tools, and guarded managed/SSH authority fails closed
  unless the installed adapter identifies as the verified Claude version.

The [contract matrix](README.md), [backend guide](../../backends/acp.md) and
[ADR 0164](../../adr/0164-optional-acp-clients.md) define supported scope. Adapter
identity is a compatibility check, not attestation of executable bytes or account
login. Admission counts launch attempts, not monetary or account-wide quotas.
