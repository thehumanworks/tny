# Verification contract — open issues 2026-09-11

Status: ACTIVE, first #123 implementation slice IN PROGRESS. Overall gate: INCOMPLETE.

Current checkpoint: fresh amended-contract review `a0cbdcbc-29b1-493e-b793-95e607ba0b3d` approved the first #123 slice; primary preimplementation guard passed and execution.jsonl records implementation_start before source writes. Read artifacts/preimplementation-gate.json for the exact native goal and input assertions. Historical NOT STARTED/PENDING entries below describe earlier stages, not the current authorization. Other patterns may not reuse this slice until the required DIFFERENT fresh code reviewer passes. No final-gate guarantee has been waived.
Created: 2026-09-11T19:20:54.559308+00:00
Source repository: /Users/tomas/projects/tny (thehumanworks/tny)
Task-owned detached worktree: /Users/tomas/projects/tny-open-issues-2026-09-11
Baseline revision: `b80c04b9df740c8388da03991cf4808c07e9cb50`; all tracked inputs match artifacts/baseline.json.
Canonical contract: `/Users/tomas/projects/tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/contract.md`
Evidence: `/Users/tomas/projects/tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/evidence.md`

## Outcome, immutable scope and authority

Implement and verify all six snapshotted open issues (#122–#127) for tny: truthful image dimensions, actionable durable-child creation, durable ask/image jobs and bounded batches, explicit original-preserving exports/contact sheets, capability-aware visual-review attachments, and private versioned image manifests with lineage.

The snapshot started at 2026-09-11T19:11:10.192526+00:00 and completed at 2026-09-11T19:11:14.199270+00:00. GitHub REST pages contain 6 and 0 records; all are issues, not pull requests; each issue's complete comments query returned zero comments. Full descriptions, acceptance criteria, URLs, timestamps and related closed issue descriptions are preserved in artifacts/issues.snapshot.json. Newly opened issues do not expand this scope. Closed #66, #98 and #50 are integration dependencies to inspect, not substitutes for verifying present source. No issue is dismissed, administratively closed or assumed resolved.

Only this worktree and task-owned scratch/check environments may be written. Preserve the original checkout, every pre-existing worktree, ongoing processes and unrelated unfinished goals. No commit, push, merge, release, deployment or GitHub issue mutation is authorized. No requirement or guarantee is excluded or reduced. Runtime support is C11 across existing supported native targets and shared wasm seams; external-process-only behavior may return an explicit documented unsupported result in wasm, but this result must itself be tested. No performance improvement is claimed without baseline/final measurements.

## Baseline and discovery findings

The original checkout was clean on main; all nine pre-existing auxiliary worktrees were also clean. artifacts/baseline.json contains 1,068 tracked inputs, symlink targets, finalized ADR hashes, and worktree state. Root AGENTS.md and applicable product, architecture, implementation-plan, image/tool docs and ADRs were read before this contract. Repository historical statements are resolved by accepted superseding ADRs, never by silently changing finalized records.

Darwin arm64 27.0, Apple Clang 21.0.0; installed project tools: clang-format 23.1.0, clang-tidy 22.1.8, Ruff 0.16.6, ShellCheck 0.11.0, shfmt 3.14.0, actionlint 1.7.12, Python 3.14, Node 26. C11, normal -Werror and make warn-strict remain enforced. Effective exclusions are in .clang-tidy, ruff.toml and Makefile; no blanket suppressions or reduced gates are proposed.

The baseline `make -j4 test` is running in the isolated worktree with inherited TNY_TOOLS=terminal; failures have already been observed in interception, SSH and subagent fixtures. They remain failures. The repository documents `env -u TNY_TOOLS make test` / leaks for default-profile fixtures; a separately recorded normalized rerun is required before attributing those failures to code. Baseline quality and leak runs are queued in the same owned runner. Results will be appended, not retroactively declared passing.

Native goal capability has been exercised via Codex 0.155.0-alpha.3's real app-server `thread/goal/get`. No matching unfinished goal was found among repository threads; unrelated goals remain untouched. A new dedicated coordinator thread must be read for its current goal before setting a goal after this snapshot. Goal identity is the native threadId, not a fabricated identifier. No token budget will be set.

Fresh Codex review probe could not run its tool host; this is a failed capability check, not a completed review. A separate fresh Claude session successfully read AGENTS.md using Read/Glob/Grep only, with write/command/child-agent tools unavailable. The actual contract/design checkpoint is still NOT RUN. Future reviewer inputs must use stdin=DEVNULL or an explicitly supplied prompt, never ambient shell stdin.

### Known conflicts and environment blockers (retained gates)

1. Finalized ADR prefixes already collide: 0030-public-event-schema.md / 0030-settings-schema-and-acp-map.md and 0045-monorepo-and-tnytty.md / 0045-system-prompt-flag.md. The user requires both unchanged finalized names/bytes and globally unique numeric prefixes. These cannot both pass in this baseline. Preserve both requirements, mark I-G6/C-G6 blocked, and do not rename, grandfather, waive, or claim uniqueness. Only the user can resolve the conflict. This does not remove any issue behavior from scope.
2. Nix, emcc and ImageMagick are not currently on PATH. Docker is installed but its Colima daemon socket is unavailable. Missing tools/environments are blockers, not NOT-APPLICABLE. Developer-only isolated setup may be added after design review; no global configuration weakening is permitted.
3. No Windows runtime or Linux runtime has yet been verified. Darwin tests alone never establish those guarantees. Hosted CI observations on another revision are baseline-only, never evidence for an unpushed dirty tree.

## Requirements

ID | Issue | Requested behavior | Invariants | Acceptance checks
--- | --- | --- | --- | ---
R122.1 | #122 | Structured results include requested and actual dimensions, effective provider size when available, MIME type, and any resize/crop operation. | I122.1 | C122
R122.2 | #122 | A mismatch yields an actionable warning in human output and a machine-readable status in JSON. | I122.2 | C122
R122.3 | #122 | Document provider-specific size support and behavior for unsupported requests; an optional strict mode fails rather than silently accepting a mismatch. | I122.3 | C122
R122.4 | #122 | Tests cover exact matches, mismatches, auto size, unsupported requests, and separation of native versus exported dimensions. | I122.4 | C122
R123.1 | #123 | Reproduce child creation from an active agent session under the all-tools profile; test named and automatically assigned IDs against the documented schema. | I123.1 | C123
R123.2 | #123 | Determine whether failure comes from invocation validation, session creation, environment inheritance, provider startup, or unsupported execution mode. | I123.2 | C123
R123.3 | #123 | Supported creation returns a durable child ID and working inspect/message/lifecycle operations. | I123.3 | C123
R123.4 | #123 | Invalid or unsupported calls return a stable error code and actionable explanation, including the valid invocation or supported fallback. | I123.4 | C123
R123.5 | #123 | Add regression tests for the identified failure and negative-path diagnostics; ensure errors do not expose credentials. | I123.5 | C123
R124.1 | #124 | Submission returns a job ID and persisted metadata/log locations immediately. | I124.1 | C124
R124.2 | #124 | Status and wait expose queued/running/succeeded/failed/cancelled states, exit/error information, and artifact paths in structured output. | I124.2 | C124
R124.3 | #124 | Jobs remain inspectable after the submitting CLI exits; interrupted jobs receive an explicit state rather than appearing to run forever. | I124.3 | C124
R124.4 | #124 | Cancellation cleans up owned child processes without affecting unrelated work. | I124.4 | C124
R124.5 | #124 | Batch execution supports bounded concurrency, per-item results, and retrying selected failures without regenerating successes. | I124.5 | C124
R124.6 | #124 | Tests cover mixed outcomes, cancellation, interrupted submitters, concurrency limits, and output-path collisions. | I124.6 | C124
R125.1 | #125 | Exact output dimensions can be requested with an explicit aspect-ratio policy and crop alignment. | I125.1 | C125
R125.2 | #125 | Native generated originals are retained separately; overwrite behavior is explicit and safe. | I125.2 | C125
R125.3 | #125 | Metadata records source dimensions, target dimensions, scaling/cropping, and source artifact linkage. | I125.3 | C125
R125.4 | #125 | Exported/upscaled files are never presented as native-resolution generations. | I125.4 | C125
R125.5 | #125 | Contact sheets support multiple artifacts with optional labels and deterministic ordering. | I125.5 | C125
R125.6 | #125 | Tests verify dimensions, aspect-ratio handling, preservation of originals, missing dependencies, and partial export failures. | I125.6 | C125
R126.1 | #126 | A successful generation can explicitly attach a preview for the agent's next visual review, using the shared attachment mechanism. | I126.1 | C126
R126.2 | #126 | Metadata-only behavior remains available for scripts and non-vision backends. | I126.2 | C126
R126.3 | #126 | Results identify the full-resolution artifact and any downsampled preview separately. | I126.3 | C126
R126.4 | #126 | Unsupported vision/transport modes give clear fallback instructions without losing a successfully generated artifact. | I126.4 | C126
R126.5 | #126 | Preview failures are distinguishable from generation failures; neither is silently interpreted as visual approval. | I126.5 | C126
R126.6 | #126 | Tests cover attachment timing, capability gating, bounded payloads, and CLI/tool parity. | I126.6 | C126
R127.1 | #127 | Record prompt, reference paths and content hashes, provider/model, quality, requested/effective/actual dimensions, timestamps, result status, and artifact paths. | I127.1 | C127
R127.2 | #127 | Record seed or provider request ID only when actually supplied by the provider; do not fabricate unavailable metadata. | I127.2 | C127
R127.3 | #127 | Edits and exports link to their source artifacts; manifests distinguish native outputs from derived copies. | I127.3 | C127
R127.4 | #127 | Manifest writes are atomic, schema-versioned, and safe under parallel runs; failed operations retain useful diagnostic state. | I127.4 | C127
R127.5 | #127 | A documented rerun/edit flow can consume recorded settings, with explicit overrides and clear warnings for missing references. | I127.5 | C127
R127.6 | #127 | Do not persist credentials; document prompt/reference privacy and provide control over manifest persistence. | I127.6 | C127
R127.7 | #127 | Tests cover round trips, lineage, moved/missing inputs, concurrent writes, and failed generation. | I127.7 | C127

## Invariants

ID | Requirement/family | Observable guarantee | Owner | Checks
--- | --- | --- | --- | ---
I122.1 | R122.1 | Structured results include requested and actual dimensions, effective provider size when available, MIME type, and any resize/crop operation. | primary implementer | C122
I122.2 | R122.2 | A mismatch yields an actionable warning in human output and a machine-readable status in JSON. | primary implementer | C122
I122.3 | R122.3 | Document provider-specific size support and behavior for unsupported requests; an optional strict mode fails rather than silently accepting a mismatch. | primary implementer | C122
I122.4 | R122.4 | Tests cover exact matches, mismatches, auto size, unsupported requests, and separation of native versus exported dimensions. | primary implementer | C122
I123.1 | R123.1 | Reproduce child creation from an active agent session under the all-tools profile; test named and automatically assigned IDs against the documented schema. | primary implementer | C123
I123.2 | R123.2 | Determine whether failure comes from invocation validation, session creation, environment inheritance, provider startup, or unsupported execution mode. | primary implementer | C123
I123.3 | R123.3 | Supported creation returns a durable child ID and working inspect/message/lifecycle operations. | primary implementer | C123
I123.4 | R123.4 | Invalid or unsupported calls return a stable error code and actionable explanation, including the valid invocation or supported fallback. | primary implementer | C123
I123.5 | R123.5 | Add regression tests for the identified failure and negative-path diagnostics; ensure errors do not expose credentials. | primary implementer | C123
I124.1 | R124.1 | Submission returns a job ID and persisted metadata/log locations immediately. | primary implementer | C124
I124.2 | R124.2 | Status and wait expose queued/running/succeeded/failed/cancelled states, exit/error information, and artifact paths in structured output. | primary implementer | C124
I124.3 | R124.3 | Jobs remain inspectable after the submitting CLI exits; interrupted jobs receive an explicit state rather than appearing to run forever. | primary implementer | C124
I124.4 | R124.4 | Cancellation cleans up owned child processes without affecting unrelated work. | primary implementer | C124
I124.5 | R124.5 | Batch execution supports bounded concurrency, per-item results, and retrying selected failures without regenerating successes. | primary implementer | C124
I124.6 | R124.6 | Tests cover mixed outcomes, cancellation, interrupted submitters, concurrency limits, and output-path collisions. | primary implementer | C124
I125.1 | R125.1 | Exact output dimensions can be requested with an explicit aspect-ratio policy and crop alignment. | primary implementer | C125
I125.2 | R125.2 | Native generated originals are retained separately; overwrite behavior is explicit and safe. | primary implementer | C125
I125.3 | R125.3 | Metadata records source dimensions, target dimensions, scaling/cropping, and source artifact linkage. | primary implementer | C125
I125.4 | R125.4 | Exported/upscaled files are never presented as native-resolution generations. | primary implementer | C125
I125.5 | R125.5 | Contact sheets support multiple artifacts with optional labels and deterministic ordering. | primary implementer | C125
I125.6 | R125.6 | Tests verify dimensions, aspect-ratio handling, preservation of originals, missing dependencies, and partial export failures. | primary implementer | C125
I126.1 | R126.1 | A successful generation can explicitly attach a preview for the agent's next visual review, using the shared attachment mechanism. | primary implementer | C126
I126.2 | R126.2 | Metadata-only behavior remains available for scripts and non-vision backends. | primary implementer | C126
I126.3 | R126.3 | Results identify the full-resolution artifact and any downsampled preview separately. | primary implementer | C126
I126.4 | R126.4 | Unsupported vision/transport modes give clear fallback instructions without losing a successfully generated artifact. | primary implementer | C126
I126.5 | R126.5 | Preview failures are distinguishable from generation failures; neither is silently interpreted as visual approval. | primary implementer | C126
I126.6 | R126.6 | Tests cover attachment timing, capability gating, bounded payloads, and CLI/tool parity. | primary implementer | C126
I127.1 | R127.1 | Record prompt, reference paths and content hashes, provider/model, quality, requested/effective/actual dimensions, timestamps, result status, and artifact paths. | primary implementer | C127
I127.2 | R127.2 | Record seed or provider request ID only when actually supplied by the provider; do not fabricate unavailable metadata. | primary implementer | C127
I127.3 | R127.3 | Edits and exports link to their source artifacts; manifests distinguish native outputs from derived copies. | primary implementer | C127
I127.4 | R127.4 | Manifest writes are atomic, schema-versioned, and safe under parallel runs; failed operations retain useful diagnostic state. | primary implementer | C127
I127.5 | R127.5 | A documented rerun/edit flow can consume recorded settings, with explicit overrides and clear warnings for missing references. | primary implementer | C127
I127.6 | R127.6 | Do not persist credentials; document prompt/reference privacy and provide control over manifest persistence. | primary implementer | C127
I127.7 | R127.7 | Tests cover round trips, lineage, moved/missing inputs, concurrent writes, and failed generation. | primary implementer | C127
I-G1 | Language quality | All introduced first-party C/Python/shell/JS/config changes pass existing effective formatting, type/static/warning gates; full-suite failures stay visible and no new violation is introduced. | primary implementer | C-G1,C-G2
I-G2 | Simplicity and navigation | Each consequential design has a documented simpler alternative and focused independent inspection of abstractions, ownership, interfaces and error paths; no duplicate provider loop or image attachment transport. | primary implementer | C-G3
I-G3 | Complete scope | Every original issue acceptance criterion maps to concrete delivered code/docs and passing observable checks, including callers, configuration, SDK compatibility and requested integrations. | primary implementer | C-G4
I-G4 | Behavior and platform boundaries | Integrated final-state unit, integration and relevant runtime checks pass across every claimed platform; fixtures and live provider checks are labeled separately, no mock replaces a claimed live boundary. | primary implementer | C-G2,C-G5
I-G5 | Mutation resistance | All planned critical faults are accounted for; original behavior passes and a compilable fault fails its mapped behavioral oracle; meaningful survivors are resolved and final state restored/rechecked. | primary implementer | C-G7
I-G6 | Decision history | Every finalized baseline ADR retains its exact filename and bytes, every substantive new decision is recorded, and every numeric ADR prefix is globally unique. Baseline conflict is BLOCKED, not waived. | primary implementer | C-G6
I-G7 | Independent reviews | A fresh read-only contract review precedes all implementation; a different fresh reviewer inspects the first meaningful slice before its pattern is repeated; further consequential boundaries are independently reviewed with dispositions. | primary implementer | C-G3,C-G8
I-G8 | Contract timing and native goal | Immutable initial contract and separate hash, baseline manifests and sequenced execution proof precede code; real native goal read/set/get links every active invariant plus exact contract/evidence paths before implementation. | primary implementer | C-G9
I-G9 | Evidence and preservation | Every run is bound to exact source/test/config/dependency inputs and environment, failed/superseded runs remain visible, evidence is invalidated on relevant change, unrelated state and secrets remain preserved. | primary implementer | C-G10
I-G10 | User understanding and completion | Issue-by-issue handoff distinguishes local checks, live integrations, CI, deployment and administration; only all-scope current proof can yield PASS and native-goal completion. | primary implementer | C-G4,C-G11

## Checks

All commands below use `/Users/tomas/projects/tny-open-issues-2026-09-11` unless a different cwd is explicitly stated. Planned new tests are acceptance entry points, not assertions that those files already exist. Any necessary command correction is appended as an amendment without dropping its behavioral criteria. A successful build alone is not runtime evidence.

ID | Invariants | Command/procedure and cwd | Pass criterion | Required evidence
--- | --- | --- | --- | ---
C122 | I122.* | `env -u TNY_TOOLS python3 tests/integration/test_image_workflow.py -k Dimensions`; `env -u TNY_TOOLS python3 tests/integration/test_image_service.py`; affected C unit tests | Exact/mismatch/auto/unsupported cases, strict mismatch preserves destination, MIME/header dimensions independently checked, CLI/tool/interception parity and native/export distinction | Full logs, captured fixture requests, byte hashes and parsed output assertions
C123 | I123.* | `env -u TNY_TOOLS python3 tests/integration/test_subagent.py build/tny`; `env -u TNY_TOOLS python3 tests/integration/test_subagent_diagnostics.py` | Reproduce named/unnamed creation in active all profile; localized failure, persistent ID/inspect/message/lifecycle or explicitly unsupported schema cases; stable actionable errors with no secret disclosure | Before/final transcripts against fixture, child persisted state, negative cases and redaction assertions
C124 | I124.* | `env -u TNY_TOOLS python3 tests/integration/test_jobs.py`; existing `test_background.py` and canonical event checks | Immediate durable IDs; queued/running/succeeded/failed/cancelled, submitter/crash/cancel states and owned descendants; bounded batches, selective retry without successful repeats, output collision protection | Process/state/log/artifact observations, concurrency oracle, canonical event comparison
C125 | I125.* | `env -u TNY_TOOLS python3 tests/integration/test_image_workflow.py -k Export` | Actual optional external tool produces independently inspected fit/crop/pad/contact-sheet dimensions and ordering; explicit overwrite, originals byte-identical; absent tool and partial failure diagnostics | Before/after original hashes, image pixel/dimension oracle, real subprocess command results
C126 | I126.* | `env -u TNY_TOOLS python3 tests/integration/test_image_workflow.py -k Preview`; existing runner/attachment tests | Next request sees shared pending attachment only after success; capability/payload limits and CLI/tool timing; metadata-only unchanged; preview failure retains artifact and cannot imply visual approval | Captured next provider request and attachment acknowledgements, negative/no-socket/no-vision cases
C127 | I127.* | `env -u TNY_TOOLS python3 tests/integration/test_image_workflow.py -k Manifest` | Versioned atomic manifests, exact reference hashes, actual metadata only, source links, failures retained, parallel safety, rerun/edit settings overrides, moved/missing references and persistence opt-out; no credentials | Schema/round-trip assertions, concurrent filesystem observations, failing-operation metadata and privacy checks
C-G1 | I-G1 | `make quality`; `make warn-strict`; inspect effective .clang-tidy/.clang-format/Ruff/shell/JS scopes | Full host quality passes or explicitly recorded baseline-only failures with no introduced violations; no disabled/reduced rules | Exact versions/logs/config manifests; Linux analyzer requires Linux evidence
C-G2 | I-G1,I-G4 | `env -u TNY_TOOLS make -j4 test`; `env -u TNY_TOOLS make -j4 leaks`; Linux `make valgrind` | Full tests pass, sanitizer errors absent, leaks pass with platform skips exposed and Linux fork coverage verified | Full logs, test counts, skips, environment and binary hashes
C-G3 | I-G2,I-G7 | Fresh read-only review of contract and original issue snapshot; different fresh review after first slice; new focused reviews at jobs ownership, manifest/export commit semantics and preview queue boundary | Identity and inspected input manifest recorded; no inherited verdicts or implementer conclusions; no children/writes; each finding fixed or resolved by evidence | Original review prompts, session IDs, input manifests, findings/dispositions, reruns
C-G4 | I-G3,I-G10 | Reconcile every R/I/C row and ledger entry against final diff, tests, mutations, docs, ADRs and reviewed findings | No requested row omitted; each has current PASS or explicit unmet verdict; overall cannot PASS with any blocker | Final requirement/invariant/check ledger and diff manifest
C-G5 | I-G4 | Native macOS/Linux/MSYS2 runtime suites, `make wasm wasm-web`, `TNY=build/wasm/tny python3 tests/integration/test_image_service.py`, new wasm cases, browser fixture; `nix flake check` where affected | Each claimed runtime actually executes its acceptance cases; external-process-only wasm modes fail cleanly; Nix declares fixtures/tools; live provider smoke separately exercises real generation/edit and preview when claimed | Platform/version/run logs, artifacts, explicit unrun/blockers, provider smoke metadata without secrets
C-G6 | I-G6 | Compare all baseline finalized ADR filenames/SHA256 against artifacts/baseline.json; enumerate every numeric prefix; exclusive allocate max+1 before each new ADR | Baseline files unchanged, zero global duplicate prefixes, substantive decisions covered. Existing collisions make this BLOCKED until user resolution | Before/final ADR manifests, collision list and allocation record
C-G7 | I-G5 | In an isolated disposable copy run `python3 tests/mutation/mutate.py --focus open-issues-2026-09-11`, plus documented controlled faults for boundary cases | Every planned M row has passing original and intended behavioral failure on compilable mutant; survivors/equivalents/timeouts/unrun visible; restored integrated suite passes | Fault IDs, exact patches/source manifests, build/test logs, failure assertion, review of exclusions and restoration proof
C-G8 | I-G7 | Inspect execution sequence at each checkpoint before permitting next implementation write | Design review precedes code; first-slice reviewer differs from design reviewer; further risky patterns not replicated before reviews | Sequenced hash-linked events and reviewer manifests
C-G9 | I-G8 | Rehash contract.initial.md against separate .sha256; inspect baseline -> initial snapshot -> native get/set/get -> design review -> implementation_start chain | Immutable initial content, ordering backed by source-state assertions and native API responses; objective contains active IDs and exact paths, no token budget | artifacts/execution.jsonl, initial hash, native goal API record and reviewed state
C-G10 | I-G9 | For every run save revision plus relevant dirty-file/content manifest; recheck original checkout/worktrees/owned processes; scan delivered logs/manifests for credentials | Results match delivered inputs; no secrets or unrelated changes; failed runs retained; evidence-only files excluded from self-hash recursion | Per-run records/manifests and preservation/redaction report
C-G11 | I-G10 | Only after every active gate passes, native `thread/goal/set` complete then `thread/goal/get`; otherwise blocked/active and INCOMPLETE handoff | Completion status never replaces behavioral proof, failed finalization disclosed, issue administration distinct | Final gate verdict and separately verified native goal status

## Ordered implementation and design boundaries

First prove and repair #123 in one meaningful slice, then obtain a different fresh review before applying its error/lifecycle patterns elsewhere. Image dimensions (#122) form the foundation for #127 manifests and #125 derived exports. Jobs (#124) own execution, not provider wire code or image semantics; coordinate the existing canonical event contract (#66). Preview (#126) reuses the #98/#50 attachment/capability boundary, not a second transport. Integrate source-of-truth result metadata through CLI, typed tools, terminal interception and existing SDK callers. Shared files have one integration writer; delegated workers get bounded file ownership and invariant IDs and cannot create goals, contracts, children or commits.

Proposed simplest design: extend existing C service/result structs and session/process primitives, keep image post-processing explicit and optional via an external tool, and record manifests as versioned adjacent artifacts. A new general daemon, linked image framework, alternate provider loop or plugin framework is not justified. Review must compare this against the simpler alternative of composing current ask/session/image primitives; add only missing lifecycle/state/metadata guarantees. Named subagent `id` semantics must be validated against present CLI/schema before choosing support versus a clear invalid-invocation result; do not claim the historical incident's exact cause without reproducing it.

Tooling changes proposed only where needed: focused Python/C acceptance fixtures; register mutation targets; update integration runner, help/schema checks and Nix fixture/tool allowlists alongside new surfaces. Optional ImageMagick and isolated Linux/emsdk/Nix development tooling may be provisioned after the contract/design gate with exact versions recorded. Do not change global policies or lower repository lint thresholds. All affected third-party dependencies remain explicit and documented.

## Critical edges and planned mutations

ID | Critical controlled fault | Expected behavioral oracle
--- | --- | ---
M122.1 | Force mismatch to exact/matched | Dimensions mismatch JSON and human warning assertions fail
M122.2 | Bypass strict mismatch rejection | Strict-mode existing-output hash preservation/exit assertions fail
M122.3 | Trust requested/provider dimensions instead of bytes | Independent header-dimension fixture fails
M123.1 | Restore named-create-to-resume confusion or bypass invalid-create diagnostic | Named/automatic invocation regression fails for the documented schema
M123.2 | Drop resolved provider/credential-safe inheritance | Child provider fixture fails without leaking credentials
M123.3 | Report failed child process as success | Negative exit/status/diagnostic assertion fails
M124.1 | Remove concurrency bound | Independent peak-active counter exceeds configured limit
M124.2 | Retry already-successful item | Successful-item execution counter changes on selective retry
M124.3 | Treat abandoned lock/worker as running forever | Killed-worker state/wait boundedness assertion fails
M124.4 | Broaden cancellation beyond owned process identity | Unrelated-process sentinel survival assertion fails; fault confined to disposable process sandbox
M124.5 | Allow colliding output reservations | Two-item collision/no-overwrite assertion fails
M125.1 | Permit export destination to alias original | Original byte/hash preservation assertion fails
M125.2 | Change fit/crop/pad or contact ordering | Pixel/dimension/order independent oracle fails
M126.1 | Queue preview after the next provider request | Immediate next-request attachment assertion fails
M126.2 | Bypass vision or payload capability bound | No-vision/no-oversize POST assertion fails
M126.3 | Convert preview failure into generation loss/successful visual approval | Artifact-preservation and separate-status assertions fail
M127.1 | Persist credentials or omit persistence opt-out | Sentinel-secret/opt-out filesystem assertion fails
M127.2 | Use non-atomic/shared temporary manifest | Concurrent-reader/writer completeness and collision assertions fail
M127.3 | Lose source hashes/lineage or silently accept missing references | Round-trip/source-hash/missing-reference assertions fail

The fault descriptions freeze critical intent, not exact source line offsets. Implement each as a compilable controlled change and preserve every planned row even if unrun. Compile failures do not count as kills. Equivalent/unreachable exclusions require independent review. Restore all faults and rerun final relevant checks.

## Native goal linkage

PENDING: initial snapshot must exist before the native goal is created. Record actual native get/set/get responses in artifacts/native-goal.json and append the returned threadId, objective and status here and in evidence.md. No file-only goal substitutes for this gate. Matching goal discovery used real native APIs; unrelated goals are out of scope and must remain untouched.

## Amendments

Append dated amendments here. The initial file remains byte-for-byte immutable. No amendment may waive a failing criterion without explicit user authorization.


## Native goal established — 2026-09-11T19:22:46.270986+00:00

Native identity: `01a091ec-193f-7a91-b8e3-b28e757eff19` (threadId); status read back `active`; tokenBudget null (not set). Initial get returned null; set/get agree on all 44 IDs, concise guarantees and exact contract/evidence pointers. Exact objective/results: [artifacts/native-goal.json](artifacts/native-goal.json). A prior overlong objective was rejected by the native 4000-character limit; [failed attempt](artifacts/native-goal-rejected-attempt.json) remains recorded. No guarantee was dropped. No implementation has begun; unrelated goals remain untouched.


## Amendment A1 — explicit acceptance semantics and complete integration scope — 2026-09-11T19:45:33.084410+00:00

This amendment is additive. All original 34 checkbox requirements, their invariants and all 10 verification-family invariants remain active. Five additional requirements below capture original non-checkbox scope and the concrete prerequisite needed for #126. There are now 39 active requirements and 49 active invariants. No issue, platform, failed run or guarantee has been waived. Historical PENDING statements above describe initial state; the current state is stated here and in evidence.md.

### Preconditions and current baseline

B001/B002/B003 finished before any implementation. B001 and B003 failed with inherited TNY_TOOLS=terminal; B002 full Darwin quality passed. A separate pristine worktree at /Users/tomas/projects/tny-open-issues-baseline-2026-09-11 ran B004 `env -u TNY_TOOLS make -j4 test` and B005 `env -u TNY_TOOLS make -j4 leaks`: both passed, exit 0, clean before/after. Logs and exact run/revision records are artifacts/baseline-normalized-*. No product source has changed. Existing Darwin leak exclusions and missing Linux/Windows/wasm/Nix execution remain explicit, not passing coverage.

Before implementation_start, a guard must rehash every baseline tracked input, immutable initial contract and finalized ADR; confirm completed baseline records; read the same native goal with the active amendment IDs; record fresh design-review resolutions; and append a hash-linked execution event before releasing the write phase. Independent review rejection is a blocked checkpoint, not approval. Only verification artifacts may change until a fresh amended-contract review has passed with actionable preimplementation findings resolved.

### Additional scope-derived requirements

ID | Issue / source | Requested behavior | Invariants | Acceptance checks
--- | --- | --- | --- | ---
R124.7 | #124 scope prose / required integration | Jobs are usable through shell CLI and native agent tools with equivalent permission, state and cancellation semantics. | I124.7 | C124
R124.8 | #124 scope prose / required integration | Ask and image job lifecycles remain distinct, and existing ask --events=jsonl retains the #66 canonical event contract rather than being replaced by a job envelope. | I124.8 | C124
R125.7 | #125 scope prose / required integration | Export and contact sheets are explicit operations; ordinary generation/editing never silently invokes post-processing. | I125.7 | C125
R126.7 | #126 scope prose / required integration | The missing image-input capability prerequisite is implemented and tested: per-profile capability override, advertised/executable tool gating and pre-turn image refusal, without duplicating the existing attachment transport. | I126.7 | C126
R127.8 | #127 scope prose / required integration | Subsequent image edits can reference an earlier artifact or job, with explicit file paths retained, source-hash validation and actionable missing-reference behavior. | I127.8 | C127

Each added I ID has the same observable guarantee as its R row, primary implementer owner and listed C check. artifacts/requirements.active.json and invariants.active.json include originals plus additions; the original JSON inventories and initial contract remain unchanged.

### #123 authoritative child contract and first slice

The supported native schema is `create`, `message`, `inspect`, `lifecycle`. `create` requires a nonempty prompt and **must omit id**; tny allocates its existing 16-lowercase-hex durable ID. ANY id supplied to create, including a plausible hex ID, is rejected before a child is launched. It never implicitly resumes, overwrites or adds an alias namespace. The actionable example is `{"action":"create","prompt":"..."}`; subsequent message/inspect/lifecycle use the returned ID. `message` requires that ID and a nonempty prompt. `inspect` and `lifecycle` require that ID. Arbitrary names and `last` are not child IDs. Bad types, unknown fields, empty strings and malformed IDs fail at validation, without interpolating untrusted values into diagnostics.

Persistent successful creation must actually save the child and return its ID. `inspect` reports existing child identity, turns and stored metadata. `lifecycle` reports observed status, exit code when stored, and resumability: live flock means running; a stored running status without a holder means interrupted/stale (not success); old records without a result use unknown/null rather than invented success. These operations read actual state, not a fixed descriptive string. `message` appends a new turn to the same child; a busy child returns a stable busy error without changing it. The misleading relationship/configure and disk-message-queue claims are corrected in current feature documentation; those actions are explicitly unsupported, not newly implemented features. Ephemeral creation stays one-shot; message/inspect/lifecycle are unsupported because no durable child exists.

The exact stable diagnostic format is the existing tool-result string prefix `error: SUBAGENT_<CODE>: <safe guidance>`. Codes are INVALID_ARGUMENT, UNSUPPORTED_ACTION, UNSUPPORTED_CONTEXT, SESSION_NOT_FOUND, SESSION_BUSY, AUTH_UNAVAILABLE, LAUNCH_FAILED, CHILD_FAILED, INVALID_RESPONSE and CANCELLED. These are documented, exact-string tested and distinguished from the envelope's generic success/error status. Validation, runtime/profile rejection, missing/busy session, missing resolved native credentials, process launch, nonzero child turn, malformed/truncated/missing child JSON and cancellation have separate observable classes. No raw provider body, child stderr, partial stdout, arbitrary rejected value or secret-bearing URL is echoed as error detail. Successful model output remains ordinary requested result content, not a diagnostic sink.

All-tools reproduction explicitly sets TNY_TOOLS=all and asserts subagent is advertised; named/automatic invocations are separate cases. artifacts/subagent-before.json records baseline automatic creation success and named-create-as-resume failure. It does not establish why the historical unnamed attempt failed. Tests must cover resolved profile/model/wire/base URL, flag-selected API-key and ChatGPT credential sources, no global environment mutation, mode/tool-profile ceilings, TNY_ISOLATE=0, ephemeral behavior, hidden terminal-only/library/SSH/host contexts and clean wasm unsupported behavior. Host-owned loops are not silently converted into native loops. Existing correlated extension events are preserved.

The child receives resolved credentials only through a private child environment or copied context, never literal argv, shell command strings, diagnostics or persisted metadata. Secret-bearing gateway URLs must not appear on argv either. Reuse existing CLI/environment configuration where sufficient; a narrowly additive environment-valued CLI option is permitted only if needed and covered by parser/help/schema tests. No global environment mutation, new credential store or provider protocol. A new subprocess helper must have bounded I/O, actual exit-status checks, owned-process cancellation and cleanup; do not introduce a general job framework merely for this fix. Keep the existing process seam and avoid shell interpolation of secrets.

C123 additionally runs `env TNY_TOOLS=all python3 tests/integration/test_subagent_diagnostics.py` and the existing subagent fixture with TNY_TOOLS=all. Test each code above, observable persisted create/message/inspect/lifecycle, invalid named create does not start a request, missing/busy IDs do not mutate unrelated sessions, and sentinel secrets in environment/flag/URL/echoed provider failure do not occur in tool errors, session diagnostic records, extension events or child argv. Unit tests may exercise inaccessible runtime guards directly; mocks never stand in for a claimed real child process.

### Image semantics, integration and dependency order

#123 is the first meaningful implementation slice; a DIFFERENT fresh reviewer must inspect it before its subprocess/error pattern is reused. Next: image-input capability prerequisite and #122 actual metadata; then #127 core manifests; then #125 exports/contact sheets and #127 export lineage; then #124 jobs/batches and #127 artifact/job references; #126 preview integration last. Capability discovery may proceed independently after the first-slice checkpoint. Cross-feature checks stay pending until their dependency exists: in particular #122 native-versus-export, #127 export/job lineage and #126 bounded preview cannot pass from a foundation-only slice.

Current source has only TNY_CAP_FAST, not the historical #50 image-input capability. Add the minimum #126 prerequisite through existing configuration/capability surfaces: a documented per-profile image-input override, consistent schema and execution gating, and refusal before a --image request reaches an incompatible provider. Reuse existing pending-image/runner attachment channel (#98); do not claim a missing capability already exists. Unknown support must not be labeled verified support. Full output and any preview are separate artifacts; payloads above the existing attachment bounds require an explicitly derived bounded preview or an actionable attachment failure that retains generation success.

Dimension mismatch is exact width AND height inequality, not approximate aspect ratio. Result fields distinguish requested size, the effective size actually sent on the provider wire (not guessed provider internals), dimensions parsed from PNG/JPEG/WebP bytes, actual MIME, and local transform metadata. Status values: match, mismatch, auto, unverifiable, unsupported. Omitted/auto request yields auto; an opaque provider-specific size token whose dimensional meaning is unknown yields unverifiable, not an invented match. Invalid/unsupported hints yield a documented local or provider rejection; a provider rejection is never retried with another size/model. Available provider size behavior must be verified against a pinned primary source; public API behavior is not silently equated with the ChatGPT account endpoint.

Strict size mode requires a concrete positive WxH and trustworthy output dimensions. A mismatch or unverifiable dimensions returns nonzero before replacing the destination. Paid response bytes are discarded, not silently saved or resized; this cost/retention behavior is documented. Structured strict failure includes stable code and requested/actual/status metadata without claiming an output was committed. Ordinary JSON fields remain additive and backward-compatible. Auto and opaque hints are rejected as invalid strict invocations before a paid request. No library-wide pixel decoder is added solely to read bounded image headers; malformed/truncated headers must not cause overreads or fabricated dimensions.

C122/C127/C126 include all existing result callers: standalone CLI, typed tool, terminal interception and SDK toolkit. Add `make test-sdks` and `make test-abi` to C-G2/C-G5, with Python and TypeScript result/option assertions and unchanged frozen ABI inventories. SDK input allowlists must permit any newly exposed option deliberately, not silently discard it. Wasm executes shared #122/#127 service cases; external-process-only export/jobs explicitly reject in wasm and the rejection itself is tested. Include new wasm fixtures in CI commands and Nix fixture/tool declarations. A build or old hosted-CI pass does not establish runtime acceptance for this dirty tree.

### Jobs, exports, manifests and permission boundaries

Jobs reuse relevant semantics from ADRs 0031, 0041, 0047 and 0081 rather than duplicating the provider loop. A new decision explicitly supersedes ADR 0047's retry deferral. Test concurrent submit IDs; cancel-before-start prevents provider invocation; submitter death does not own the job lifetime; worker loss becomes terminal interrupted/failed; wait timeout exits 124 without cancelling; selective retry leaves successful item execution counters and artifact hashes unchanged. Cancellation requires current ownership beyond a persisted numeric PID, retains unrelated-process sentinels and reports incomplete cleanup. Canonical ask JSONL remains the same normalized event stream; job lifecycle envelopes have a separate documented kind/version. Job output collision checks include normalized aliases and concurrent reservations. CLI and agent-tool surfaces have explicit sensitive permission identities; reading status/logs does not grant permission to submit/cancel or overwrite artifacts.

Exports support an explicitly declared ImageMagick 7 magick executable; IM6-only or missing installations fail actionably unless separately implemented and tested. Do not discover an arbitrary convert executable and assume compatibility. Stage image bytes under generated private names, use fixed argv and explicit output formats, and treat coder prefixes, @file, brackets, percent expansion and label text as untrusted data, not ImageMagick expressions. Optional labels may be constrained to deterministic item numbers rather than model-supplied expression text; document the supported form. Inputs and outputs must not alias by normalized path, symlink or hard link even under overwrite. Fit preserves full content/aspect, crop fills target and crops according to explicit alignment, pad fills target with explicit padding; every successful output has the requested exact canvas. Originals are byte-identical after both success and partial failure. C125 uses the real optional executable and independently inspects output dimensions/pixels/order; a fake subprocess alone does not verify transforms. Nix tests must declare the executable and any fixture font required. Generation/editing does not implicitly invoke it.

Manifests use private permissions (0600 files, private temporary staging), explicit versioning, atomic replacement, unique temp names, bounded inputs and a documented opt-out that writes no manifest/prompt record. Only provider-supplied seed/request IDs are stored; absence is null/omission. Newer unsupported schema versions fail rather than being guessed. Source paths/hashes and versioned lineage allow edit/rerun with explicit overrides; missing/moved or changed references produce actionable failures/warnings rather than uploading unintended bytes. Failure records never claim a committed output; collision and manifest-write failures preserve original artifacts and remain visible. Artifact/job references resolve through validated recorded outputs, not arbitrary traversal. Permission checks cover reference uploads, exports, sheets, job operations and preview across direct tools and interception.

### Additional planned mutations (all original M rows retained)

ID | Critical fault | Required kill oracle
--- | --- | ---
M123.4 | Rename/merge a stable error code | exact documented error-class assertions fail
M123.5 | Relay raw child stderr/provider echo or secret-bearing URL | diagnostic/session/event/argv sentinel assertions fail
M122.4 | Report auto as mismatch or invent effective dimensions | auto/opaque metadata oracle fails
M127.4 | Fabricate seed/request ID not supplied by provider | missing-provider-field oracle fails
M127.5 | Persist manifest with public permissions or ignore privacy opt-out | file-mode/prompt-persistence assertions fail
M127.6 | Accept unsupported future manifest schema | version rejection oracle fails
M-G1 | SDK drops new metadata/options | Python/TypeScript shared-service parity assertion fails
M124.6 | Trust reused PID or run cancelled queued work | ownership/no-provider-invocation assertions fail in disposable process fixtures
M125.3 | Interpret unsafe label/coder/path syntax | sentinel local-file non-read and explicit validation oracle fails
M125.4 | Permit hard-link/symlink alias overwrite | original inode/hash preservation assertions fail
M-G2 | Bypass an added operation permission identity | deny-mode no-side-effect assertions fail

### ADR preservation and final blockers

All baseline numbered ADRs AND docs/adr/README.md remain byte-identical. New numbered ADRs are discoverable files in docs/adr and linked from current feature documentation and evidence; an unchanged historical index does not erase a newly recorded decision. Root AGENTS.md requires decisions to be recorded in docs/adr; it does not require mutating the index. Therefore no index exception or user permission is assumed. This resolves the proposed index-edit conflict by choosing not to edit it. Existing numeric-prefix collisions 0030/0045 remain an unwaived final blocker pending the user's answer. Allocate new numbers exclusively under the primary writer, rechecking the delivered tree and original checkout before creation; do not claim uniqueness across arbitrary unpublished branches.

Native goal 01a091ec-193f-7a91-b8e3-b28e757eff19 remains the only canonical goal. Read/set/get must add the five new invariant IDs while preserving all original IDs and exact pointers; no token budget is set. No implementation or process/dependency/configuration changes are authorized across the still-blocked design-review checkpoint. A fresh review of this amended contract is required; review approval remains separate from behavioral evidence.


### A1 native linkage verified — 2026-09-11T19:46:23.279955+00:00

The same native goal now includes all 49 active invariant IDs and guarantees; no token budget was set. Native read-before/set/read-after evidence: [artifacts/native-goal-amendment-1.json](artifacts/native-goal-amendment-1.json). Implementation has not started.


### Current-stage annotation — 2026-09-11T20:02:02.038990+00:00

This status-only annotation records the already-verified first-slice design approval and execution guard; no requirement or check changed. Bounded implementation worker: `41eaddaa-3242-4e37-b3de-4e09353d4e4d`. Canonical native goal remains active.


## Amendment A2 — image foundation commit, manifest and replay boundaries — 2026-09-11T20:13:04.304400+00:00

This amendment resolves the image foundation's remaining concrete design choices before its implementation. It adds no new requirement IDs and removes or weakens none of the 49 active invariants. The first #123 implementation and its different-reviewer checkpoint remain independent; image product code may begin only after that checkpoint and the image design prerequisites are satisfied. A new proposed ADR will record these choices before image implementation; no existing ADR or index will change.

### Observable result and provider metadata

Use shared `tny_image_result` metadata and a bounded image-header helper next to the existing image MIME helpers. PNG requires signature, complete IHDR header with legal dimensions/type/length (not merely offset reads); JPEG uses a bounded marker/segment walk to a supported SOF with positive dimensions; WebP validates RIFF/chunk boundaries and VP8/VP8L/VP8X dimension encodings. Truncation, zero/oversized/impossible values and inconsistent headers are unverifiable, never guessed. Metadata parsing is not full pixel decoding and must not be documented as such. Strict behavior is unchanged from A1.

Output JSON stays additive: `requested_size`, `effective_size` (literal value sent), `width`, `height`, `size_status`, `mime_type`, `native` and `transform` distinguish original provider bytes from derived artifacts. Unknown dimensions are null rather than zero. A strict failure returns a stable error object including available metadata but no falsely committed output path. Preserve old successful fields and ordinary human stdout output path; put mismatch guidance and the optional manifest path on stderr. New flags/options are `--strict-size` / `strict_size` and `--no-manifest` / `persist_manifest:false`. Omission retains non-strict generation and enables the manifest. All four shared service callers deliberately map the same options/results, including SDK allowlists and type declarations.

The provider adapter passes the exact literal effective size and any actually returned scalar seed or request identifier through a bounded metadata result. Omit/null those fields when absent; do not hardcode them null if a recognized field is present, and do not infer them from timestamps, local operation IDs, requested seeds or public-API behavior. Tests supply both absent and present seed/request-ID metadata and verify preservation without fabricating unknown values. Unknown untrusted provider keys never get copied wholesale into manifests or error records.

### Immutable operation identity and safe concurrent output

Each requested generation/edit has a generated operation ID and its own private manifest, automatically named adjacent to the destination as `<output>.tny-image-<operation-id>.json`. The exact path is returned in metadata; it is not a guessed global registry or a mutable shared latest-manifest pointer. The manifest file is created with exclusive identity and private permissions and is updated atomically only by that operation while running. Once terminal, it is immutable. Later generations at the same destination create new manifests rather than overwriting earlier lineage. Reference records carry the earlier manifest path, source hash and operation ID; image bytes are not silently copied into a new content-addressed store.

Before a paid request, acquire a nonblocking per-output writer guard through the existing host OS seam and write a private atomic `running` intent manifest when persistence is enabled. Concurrent attempts for the same normalized destination return a stable busy error before a provider call. Guard identity includes the active operation ID, not only a PID; normal exit, cancellation and process death release native ownership. Existing output, references, output aliases and reserved manifest paths are validated before mutation. The guard does not authorize following an unsafe symlink or overwriting a reference as an export. Native inode/path alias cases receive explicit checks. Wasm uses the existing host seam and its actual filesystem/execution model; it must either enforce the same per-instance concurrent guard or explicitly reject unsupported concurrent execution, not claim an OS flock exists.

Under the held guard, validate actual image bytes and strict size first, stage/atomically replace output, then atomically finalize that operation's manifest with output SHA-256, byte count, actual dimensions, MIME, status and path. A two-file rename is NOT claimed to be a transactional pair. If final manifest persistence fails after output commit, return a distinct error that says the artifact was retained but provenance finalization failed; never delete a successful paid artifact to hide a metadata failure. If a process dies between writes, the intent remains nonterminal; a reader/replay detects no matching live operation ownership, classifies it interrupted, and refuses to represent it as verified success. Output hash verification is mandatory before a manifest's output is reused or represented as current. A later overwrite or missing output yields an actionable changed/missing-artifact result, not stale metadata presented as the current file. Failed operations never claim another operation's existing destination as their successful output.

Manifest privacy opt-out writes no manifest or prompt/reference record and does not create a replayable operation record. A nonsecret transient writer guard may still be needed; it must not contain prompts, credentials or reference contents. On initial manifest write failure, fail before contacting the provider. On provider/validation/strict failure, atomically record the safe terminal failure when possible, preserving any previous destination. No raw provider error body or credentials are stored. File mode, atomicity, cancellation and the interrupted-intent read path are direct assertions in C127, not inferred from a successful request.

### Version 1 manifest schema and reference checks

The JSON object has `version:1`, `kind:"image_manifest"`, `operation_id`, operation (`generate` or `edit`), status (`running`, `succeeded`, `failed`, `cancelled`, `interrupted` as observed), started/finished UTC timestamps, prompt, references (path, SHA-256 and optional source manifest/operation identity), requested provider/model/quality/size, effective provider/model/size, result dimensions/MIME/bytes/size-status, actual optional provider seed/request ID, safe error code/message on failure, and an `artifacts` array. Each successful artifact has role `native` or `derived`, path, SHA-256, dimensions/MIME and explicit transform/source-lineage metadata. Omit unavailable values or use null; never make up a provider value. Version, bounds, JSON types, reference count and each field needed for replay are checked. Unknown future versions and invalid/malicious manifests fail closed. Additional unknown fields may be ignored only if they do not alter execution or bypass validation.

Resolve relative references relative to the manifest's recorded workspace/base directory rather than the caller's unrelated cwd. Store paths and hashes of the exact bytes actually sent to the provider, not hashes from an earlier preflight that can race with a changed file. Before replay/edit reads, load each reference once into bounded memory, verify its hash and use those same verified bytes. Changed/missing references require an explicit newly supplied reference/override rather than silently trusting a new file at the old path. Symlink and moved-file behavior is documented and tested. Merely parsing a manifest never reads arbitrary source paths or contacts a provider; only an explicitly requested replay/edit operation does so under the existing permission boundary.

### Replay and earlier-artifact surfaces

`tny image replay --manifest <path> --output-file <new-path> [explicit options]` reruns the recorded generate/edit operation using stored prompt/references/settings. A new output path is mandatory so replay cannot silently overwrite the source; explicit provider/model/quality/size and stdin prompt replace their recorded counterparts, while omitted options reuse them. Reject an empty/invalid replay source, failed/interrupted source manifest, unsupported version or missing/changed required reference. Pure replay does not require stdin on a TTY; nonempty piped stdin is an explicit prompt override. The new result/manifest links the source operation and records all actual overrides.

`tny image edit --artifact <manifest-path> --output-file <new-path>` uses the source manifest's verified successful output as a reference; ordinary `--image <path>` remains supported. Multiple references obey the existing maximum and declared ordering. The tools/SDK expose equivalent `from_manifest` and `artifact` inputs via the shared service parser/validation where those operations are supported; no second replay implementation is created. A future #124 integration adds `--job <id>` / job reference by resolving the selected successful job artifact into this same reference representation. Job/export lineage and their cross-feature acceptance rows remain PENDING until those dependent slices are implemented; this foundation cannot by itself mark #127 or #122 native-versus-export checks complete.

### Focused additional checks, unchanged acceptance strength

C122: valid/truncated/malformed PNG, baseline/progressive JPEG and WebP lossless/lossy/extended headers; independent dimensions; strict no provider for invalid size; strict mismatch leaves old destination bytes unchanged; provider metadata present/absent; all caller options and JSON fields.
C127: unique manifest identities for repeated output; nonblocking same-output concurrency produces at most one paid request; independent output-hash consistency; killed writer/abandoned intent; metadata failure after successful artifact commit stays visible without deleting the artifact; 0600/opt-out; references changed between preflight and send cannot bypass hash validation; replay from unrelated cwd and explicit overrides; future version rejected; old overwritten artifacts never represented as current. Controlled faults are added under existing M122/M127 families and remain unrun until actually executed. A meaningful survivor cannot be dismissed as merely a parser detail.

The simpler alternative (one last-writer-wins mutable sidecar) was rejected because independently renamed image/sidecar writes can silently disagree under concurrency and destroy prior lineage. Unique per-operation records plus one existing-seam writer guard avoid a daemon, database, content-addressed blob store and a fictional multi-file atomic transaction while exposing the unavoidable crash window honestly.


## Amendment A3 — resumed #123 review corrections — 2026-09-11T22:28:32.491591+00:00

The original six-issue snapshot, all 39 requirements and all 49 invariants remain active. A fresh paginated reconciliation again returned only #122–#127 and zero comments for each; it does not replace or backdate the immutable original snapshot. The same real native goal was read active with all 49 IDs and no token budget. Resume input manifests and native response: artifacts/resume-20260911/discovery.json. All baseline ADR filenames/bytes and the original contract hash were reverified; the existing 0030/0045 collisions still block I-G6. Main and unrelated worktrees/processes remain untouched.

The earlier first-slice reviewer d15820da-1861-4b3f-af32-a01b979dfea4 approved the inspected pattern, but identified an overly short launcher cancellation grace and a readlink truncation guard. The current worker state differs from that earlier frozen snapshot (host-path lookup moved into util/process.c and tests expanded). A fresh review of the current corrected files is required before pattern reuse. Prior passes remain bound to their recorded snapshots, not this resumed tree.

C123 adds a real-process regression `subagent_child_wind_down_completes_before_forced_kill` under the existing `make test-unit` entry point: after interrupt, a child takes four seconds to record its wind-down; the launcher must allow that completion instead of forcing its previous three-second kill. The child's own five-second session cancellation deadline and the launcher's later fallback share an explicit constant. This does not promise clean finalization after every enclosing caller's independent hard-stop deadline; interrupted/stale classification and owned-process cleanup remain mandatory. Existing actual nested-tny cancellation integration still runs separately.

Additional planned fault M123.6: restore the launcher's three-second deadline; the unmodified implementation must preserve the wind-down marker, and the compilable fault must fail that observable assertion. This is additive to all original and A1/A2 mutation rows. No canceled, failed or unrun mutation is counted as killed.


## Amendment A3.1 — controlled #123 faults — 2026-09-11T22:44:13.575516+00:00

The original C-G7 and every mutation family remain active. Its repository runner must target the new shared subagent functions rather than removed shell-command helpers. In addition, run `python3 tests/mutation/open_issues_20260911.py --source <frozen reviewed source copy> --artifacts <versioned evidence directory>` from the task checkout. This documented controlled-fault procedure is additive, not a substitute for final integrated checks or unimplemented issue families. It clones that supplied frozen source into a disposable directory, records all inputs and the complete planned set before faulting, proves each unmodified focused test passes, separately checks compilation, requires a named behavioral assertion failure (timeouts/compile errors/zero tests are not kills), restores bytes after each fault, and rebuilds/reruns original tests.

Planned variants: M123.1 removes the create-id rejection; M123.2a substitutes a different provider, M123.2b drops the resolved credential carrier, M123.2c widens the inherited permission ceiling; M123.3 treats the process exit status as success; M123.4 renames INVALID_ARGUMENT; M123.5 returns raw child failure text; M123.6 restores the premature three-second cancellation. Exact byte replacements and named test oracles are recorded in the runner's immutable per-run plan before execution. The three M123.2 variants strengthen the existing configuration-inheritance family; none removes an original planned case. Final-state reruns remain required after relevant source changes.


## Amendment A4 — image-input capability prerequisite — 2026-09-11T23:08:09.402966+00:00

R126.7/I126.7 remain the existing capability requirement, not a new issue or a scope reduction. The approved design is artifacts/image-capability-design-v2.md, with fresh independent read-only reviewer 77a85d3c-ac1c-4317-bed4-adefab0c9d73 and raw input hashes/response in artifacts/resume-20260911/review-capability-design-v2.*. The earlier review e574e4c7-c4bd-4740-9ccb-0e8da9230b69 did not approve the original provider-object proposal; it remains preserved and is superseded, not counted as approval.

Use the separate top-level per-provider boolean map `image_input`, never a same-named builtin provider object that could replace OAuth wiring. Effective enum: unknown / configured-supported / configured-unsupported; the public wording for true is **configured, unverified**, never simply supported. Unknown preserves the existing explicit manual image paths but cannot authorize future automatic preview; false is a hard refusal. Actual ACP/native-queue/host restrictions still apply even when configured true. Image generation and its independent credentials are never gated by conversation image input. Map keys are canonical provider selectors, 1–256 ASCII bytes with existing provider-name grammar; ACP map keys use `acp@NAME`, while CLI aliases are normalized through existing `acp_provider_name`. Duplicate keys, NULs, invalid root/value types and noncanonical/invalid selectors fail actual runtime parsing; editor schema validation is separate. The private enum resets/recomputes at every provider resolution.

Four explicit gates: (1) engine start before prompt/event/session-state mutation; (2) engine queue and direct tools queue before file read/append, and pending flush before any mutation; **a refused flush preserves pending entries and count**; (3) read_image schema advertisement and tools_call_prepare agree, with existing permission/profile/SSH restrictions retained; (4) CLI rejects configured-false --image before creating/opening a session or provider connect. Source check for reviewer qualification: src/main.c invokes cli_make_ctx before cmd_ask (lines 96–103), and src/cli/args.c resolves the provider inside cli_make_ctx (line 311), before cmd_ask's session creation. No provider/auth ordering refactor is necessary.

C126 prerequisite check: `env -u TNY_TOOLS python3 tests/integration/test_image_input.py <absolute build/tny>` plus the corresponding unit and existing image_service fixtures. Pass requires actual parser rejection (not schema-only), unchanged builtin auth configuration under fake credentials, provider-switch/ACP-alias reset, hidden schema/direct-call agreement, zero provider calls/no new CLI session under false, preserved engine/pending state, true/unknown explicit actual request image parts, generation still usable with chat false, and truthful labels. Tests run native macOS/Linux and wasm with exact snapshots; a missing environment stays blocked. The later Preview acceptance remains pending; this prerequisite is not #126 completion.

Planned additional faults within M126 capability family: bypass false engine/queue/flush/tool gates; mislabel unknown as configured-supported; omit reset on provider change; accept invalid runtime boolean. Each is mapped to its corresponding no-call/no-mutation, label/reset or actual-parser assertion; all remain required and unrun until recorded.

Implementation is isolated in /Users/tomas/projects/tny-open-issues-capabilities-2026-09-11 to avoid concurrent image-worker ownership. It references this same contract and native goal, creates no second contract/goal, and owns only the named capability files in artifacts/worker-image-capabilities/run.json. The primary integrates and re-verifies the combined tree and owns every invariant. New capability source requires a fresh consequential-boundary review before use by later preview integration.


## Amendment A5 — known launch-path failures across spawn implementations — 2026-09-11T23:09:09.523959+00:00

R123.2/R123.4 and C123 retain their original stable actionable failure guarantees. L103/L104/L105 now pass on the corrected stronger-regression input. L106 remains failed: its missing-executable case exposed a real portability assumption in the launcher. POSIX allows some child-side exec failures to appear as a successful spawn followed by process exit 127; Valgrind 3.22 exhibits this for missing executables (upstream record https://bugs.kde.org/show_bug.cgi?id=481679; specification https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html). The test's LAUNCH_FAILED expectation is retained, not weakened or disabled.

The host process seam will reject an already missing, non-regular or entirely non-executable launch path before posix_spawn, returning its errno/safe EACCES. This is a diagnostic preflight, not authorization, an atomic exec guarantee or a promise the path cannot change afterward. Real process exit 127 remains CHILD_FAILED unless the parent actually observed a pre-spawn failure; never relabel an arbitrary child's 127 as a known launch failure. No check suppression, Valgrind exclusion, dependency downgrade or new global tool setting is permitted.

C123 adds a directory-path rejection to the existing absent/relative-path cases. Existing actual exit 7 with complete success JSON continues to prove real exit status cannot be overwritten by reported success. Linux make valgrind, normal unit/integration and fresh macOS focused runs must pass on the changed process seam; all earlier failures remain visible. A fresh independent code review of the preflight/outcome boundary is required before any later jobs reuse.

Additional fault M123.7 removes this known-path preflight in an isolated copy; the Linux Valgrind focused `subagent_process_outcomes_are_classified` case must fail its LAUNCH_FAILED behavioral assertion after a successful mutant build. It is not a memory-report or compile-error kill. The entire earlier M123.1–M123.6 set remains active.


## Amendment A6 — header trust counterexamples and safe caller parity — 2026-09-11T23:40:53.236659+00:00

All original R122/I122/C122 criteria remain active. The fresh independent #122 code review 09eebc60-459f-4147-8d22-90c4a67d927c identified incorrectly accepting arithmetic lossless JPEG SOF11 (0xCB) despite the documented unsupported-lossless policy. That finding must be fixed before pattern reuse. Review approval does not prove the parser against independent malformed inputs. The read-only compiled probe at artifacts/resume-20260911/header-probe-before reproduced additional unsafe answers for corrupted IHDR CRC, missing RIFF odd-chunk padding, nonzero VP8L version, and unsupported JPEG precision. The first probe's two VP8X reserved-field expectations were wrong: the official WebP reader specification explicitly requires readers to ignore those fields. Those cases remain visible as **incorrect exploratory oracles**, not product defects or excluded required mutations; corrected regression tests must preserve their accepted dimensions.

The bounded header reader will validate IHDR's checksum, require declared RIFF size/chunk padding to fit including its zero pad, reject unknown VP8L versions, distinguish supported DCT precisions (baseline 8, supported non-baseline DCT 8/12), and keep arithmetic lossless unsupported. VP8X width-times-height must not exceed its format's 32-bit pixel-product bound. Reserved VP8X fields stay ignored as mandated; trailing RIFF data may remain ignored per the specification. This remains header metadata validation, not full compressed-pixel validation or an added decoder dependency.

C122 adds an independent fixed-fixture corpus and deliberate corruptions. Positive C fixture builders must emit real IHDR CRC and required RIFF padding, not a malformed input tailored to the old parser. The test oracle for checksum uses zlib in Python/generated known fixtures rather than sharing the production CRC routine. Every new negative case must fail before the correction and pass after; all applicable prior tests remain, and a fresh corrected-state review precedes manifest reuse. Additional M122 critical faults remove checksum/padding/version/precision/pixel-product guards and re-enable SOF11; each must be killed by its named header regression, with successful compilation and restored original checks.

Original line-243 structured strict-failure metadata remains required. The initial implementation intentionally gave SDKs a coarse status only, typed tools a plain error and interception no JSON failure body; that is an unmet caller-parity gate, not an authorized reduction. Preserve existing generic provider-error secrecy, frozen public ABI, exception status and normal stdout. A minimal additive plan will carry only locally constructed, whitelisted strict-size failure metadata through existing result accessors/error channels, without provider payloads/credentials or a committed path; independently review that security/API boundary before implementation. Invalid arguments and generic provider/cancel/OOM errors must never acquire arbitrary request/config/provider dumps.

Primary format references: https://www.w3.org/TR/png-3/ (IHDR/CRC), https://developers.google.com/speed/webp/docs/riff_container (odd payload pad, even RIFF size, ignored VP8X reserved fields and canvas pixel-product bound), https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification (version must be zero). Original and failed probes/results remain immutable; current source-bound acceptance and whole-platform matrix remain pending.


## Amendment A7 — canonical foreground-event prerequisite — 2026-09-11T23:42:43.890450+00:00

I124.8/R124.8 requires canonical ask events, but A1's statement that --events=jsonl already exists was factually wrong: actual source lacks it and GitHub #66 was closed not_planned, not implemented. The dependency body and current state are preserved in artifacts/issue-66-dependency.json. No extra open issue was added to the frozen set. Its canonical behavior was already referenced by active #124 and remains required. This independent ask-event prerequisite may be implemented in an isolated increment before image manifest/export phases; durable jobs and later phase order remain unchanged.

Fresh read-only design review 6407aa95-b471-4b79-9adb-380b709dcb9d conditionally approved reusing the existing in-process engine, owned canonical envelope and frozen public-ABI enum values, with all three conditions made mandatory here: every write/flush failure cancels/cleans the owned turn and returns a distinct non-success exit (2 for stream I/O failure); drained-without-turn_end branches on the explicit seen-terminal boolean before stop-reason/default value and fails without inventing a turn_end; envelope keys remain present even for empty ephemeral session_id/turn_id values. Existing engine envelope values are serialized verbatim, not reconstructed.

The machine mode is additive and foreground: --events=jsonl conflicts explicitly with -B/--background and final --json stdout. It preserves --ephemeral as an in-memory engine turn, legacy Markdown/final-json behavior, real permission flow and existing provider restrictions. --progress=none suppresses successful human status/tool stderr independently but does not suppress actual errors. Startup/configuration failures use a documented stable machine diagnostic on stderr before any accepted turn; no fabricated engine event/terminal may be emitted before successful engine start.

Bounded backpressure is an acceptance guarantee, not an assumption that checked blocking stdio alone is sufficient. The writer must preserve event order/completeness while its consumer is available, bound retained memory, respond to SIGINT/closed pipes, stop reporting success after a stream I/O failure, clean owned engine/tool work and avoid leaving inherited file-status flags changed. Use the existing cancellation probe/deadline and smallest OS write/poll seam; do not add a second agent loop, durable event broker, unbounded queue or changes to frozen ABI records. Persisted execution failure must remain honest; completed real provider output is retained rather than fabricated/discarded to hide a stdout error.

C124-events prerequisite: `python3 tests/integration/test_ask_events.py <absolute build/tny>` from repository root (temporary HOME/fake credentials/real loopback only), canonical public-reader conformance fixtures, and existing ask/cancel/permission/SDK tests. Assert per-line strict JSON/envelope/schema_version/monotonic sequence, exact event order and payload fields versus libtny readers on the same deterministic fixture; one actual turn_end only after an accepted start; tool/permission/usage/error/cancel/empty text values; startup failure/ephemeral/option-conflict/legacy stdout behavior; slow-reader bounded RSS/no event loss, closed reader nonzero status, SIGINT while pipe is full terminates owned work and spares an unrelated sentinel; backend drained with no terminal does not pass. Build alone and schema-only checks do not establish these claims. macOS/Linux and applicable wasm runtime/conformance remain required.

M124-events adds controlled faults: change canonical numeric enum/field mapping; omit/duplicate turn_end; let missing-terminal default to DONE; ignore failed stdout write; bypass cancellation while blocked; add human text to JSONL stdout or progress=none success stderr. Every applicable fault must have a named behavioral/conformance oracle, build successfully, fail for that reason, and restore/retest. Existing M124 lifecycle/batch faults remain pending, not replaced.

Implementation ownership is isolated in /Users/tomas/projects/tny-open-issues-events-2026-09-11 and references this canonical contract and goal. A fresh actual-code review is mandatory before the event path is reused for jobs. This increment does not implement or complete durable ask/image jobs, image manifests, exports or previews.


## Amendment A8 — reviewed schema combination and container-boundary cases — 2026-09-11T23:56:38.304057+00:00

I126.7's schema/execution parity remains mandatory for fully configured as well as minimal profiles. Fresh reviewer bc94819b-d683-4ba8-8c69-a73dfc2ead01 found the all-features raw-schema fast path bypassed the new read_image filter when web search, speech playback and image generation were all available. C126 adds `image_input_false_gates_fully_configured_schema`, with a private temporary executable only to establish audio-player availability (no playback), fake direct ChatGPT credentials, actual configured web-search/image capability and unchanged failure status. It must fail before correction and pass afterward. The smallest correction adds the missing capability predicate to the existing filter guard, avoiding an unrelated schema rewrite. M126.C16 removes that predicate and must be killed by this combination regression on a runtime where the fast path exists. The prior C1–C15 set stays active.

Fresh header reviewer 4200e86a-761f-4020-b861-3c407960780d found no trust leak but two incorrect container-boundary cases: native 32-bit intermediate addition and a dangling partial RIFF chunk header classified as unsupported instead of unverifiable. C122 adds a two-byte partial-chunk corpus case; it must fail before correction. RIFF bounds/advances must use size_t arithmetic with subtraction-based availability checks, retain the format's maximum even RIFF size and return unverifiable for an incomplete final chunk. This preserves strict failure status accuracy without claiming full pixel validation. M122.H7 removes the incomplete-tail distinction and must be killed by that regression.

The actual frozen integrated Nix check fails the unchanged size gate at 1,051,608 bytes; the pristine same-revision/same-lock/same-architecture Nix package passes at 986,072 bytes. This introduced size excess remains a blocker; no budget increase or check suppression is authorized. Linux full-test timeout and missing quality tools are also failures/unmet gates, not passing scoped verification. Exact logs remain in artifacts/resume-20260911/integrated-platform-a6a4 and nix-baseline-size. Git-dependent quality commands must run with a real inventory, not an untracked archive that silently yields an empty file list.


## Amendment A9 — strictly local failure detail across existing result channels — 2026-09-11T23:58:33.726985+00:00

Original #122 strict-failure metadata parity remains required. Fresh read-only design review returned session 1dde9053-32bb-4b8d-b236-b5504b013e1c (requested launch identifier is preserved separately in review-image-failure-parity-design.run.json; the returned session ID is authoritative). Its conditional approval and all six corrections are resolved as this implementation contract, not treated as behavioral proof. The proposal is artifacts/image-failure-parity-design.md.

Use exactly the four locally assigned strict-size codes, not a provider-text prefix, to populate a shared safe failure object. It contains locally constructed stable code/diagnostic, explicit requested size/exact actual wire size only when sent, native dimensions/MIME/status and path:null/committed:false. It never contains credentials, endpoint/header/provider body, prompt, or reference/output paths. Generic provider, input-allocation, cancellation and OOM failures keep empty result detail; no arbitrary error-string serialization. Result/request structs must be initialized before any fallible work.

Preserve the existing failure signal at every boundary. Typed tool returns its usual `error: ` prefix followed by the safe structured object only for local strict-size failures. Interception returns that object on stdout only for explicitly requested --json; non-JSON error stdout remains empty and rc remains failure. Native toolkit run retains its nonzero status and generic error message, while the existing completed-job result accessor can expose safe strict-failure detail only; result is empty again if final OOM/cancel overrides. This deliberately revises the new slice's assertion that every error has no result: tests/abi/test_toolkit.py and docs/sdk-toolkit.md must explicitly test/document the narrow exception; generic-failure emptiness remains required. Public ABI layout, symbols, version and successful results are unchanged.

Python reads/copies the completed result before job destruction and before/while handling the existing raise_status exception; inspection of _binding.py:327–336 confirms raise_status frees the native error then raises error_from_status, so catching that typed exception after copying detail is sufficient without changing global raise semantics. Attach an optional read-only **image_detail** with a distinct failure type, not the success ImageResult. Ordinary str/repr/traceback remain generic. TypeScript complete() likewise reads result before destroy on the failure branch; its own distinct validated failure-detail type is attached as non-enumerable read-only **imageDetail** (a non-enumerable private JSON carrier is permitted). Never reuse the success ImageResult parser for a differently shaped error. Ordinary message/stack/JSON.stringify/default inspection must not print the detail. OOM while formatting degrades to a generic failure, never success or a partially trusted detail.

C122 adds strict preflight/mismatch results through native ABI, Python, TypeScript, typed tools and --json interception; exact nonzero/error marker, dimensions/status, unchanged existing output, no fake commit, provider-call count zero/one and no retries are asserted. Explicit non-JSON errors remain compatible. Fake provider/body/config sentinels, generic errors, cancellation and available allocator fault injection must show empty error detail and no sentinel in default error formatting. Normal result/ABI/memory/cancel tests stay active. Planned M122 failure-parity faults drop individual caller details, falsely label generic failure as safe, retain detail after cancellation, report success after mismatch, or expose details through normal formatting. Every applicable fault requires successful mutant compilation, the mapped behavioral failure, restoration and current checks.

Bounded worker ownership is recorded in artifacts/worker-image-failure-parity/run.json. It references the same contract/native goal and must stop after this slice for a fresh actual-code/security review before #127 manifest reuse. No image manifest/export/job/preview implementation is authorized in this worker, and no goal/issue/overall completion can be claimed.


## A8 experiment note — release size, no project rule change yet

The introduced Nix size failure will be investigated against the same frozen source/toolchain using an isolated compiler invocation with link-time optimization (`CC=cc -flto=auto`). This changes neither repository files nor the1MiB gate and is not final acceptance. The default unchanged build's failure and pristine baseline's pass remain the comparison. Any permanent release-flag change requires a substantive proposed ADR, independent design review, evidence of existing compatible compile/link toolchains, full runtime/ABI/platform checks and a new final-source measurement. No debug, warning, sanitizer, quality, fixture or size check may be disabled to obtain a passing measurement. Results may support a binary-size statement only, not an unmeasured runtime-performance claim.


### A8.1 — bounded-map and controlled-fault inventory

The new image_input map has a1024-entry bound to cap duplicate validation cost; this does not restrict any preexisting setting. Runtime and schema agree. C126 explicitly tests1024 accepted and1025 rejected, with M126.C17 removing the bound. The earlier malformed-map regression tested only rejection; the positive boundary is additive, not a weakened expectation. M126.C1–C15 retain the worker's original exact intents and oracle mappings, C16 the reviewed fully configured schema combination, and C17 this size bound. M122 original1–4 plus H1–H7 header guards remain. The controlled runner preserves its complete plan before any fault, separately compiles every mutant, refuses zero-test/timeout/compiler/unrelated-error kills, restores before the next fault and reruns every original oracle at completion. Any survivor remains visible for independent equivalence/criticality review, not silently filtered out.


## Amendment A10 — resumed reviews and cancellation correctness — 2026-09-12T07:42:34.606196+00:00

All original39requirements and49invariants remain active. New resume N001 full macOS test, N002 leaks and N003 quality each pass with unchanged recorded inputs; historical failures stay visible. The previous worker's leak-failure report is not adopted as a permanent baseline exception: the current normalized full leak gate actually passes.

Read-only reviewer798913bc-ab5f-4666-b788-b2e5cd05012c inspected the completed strict-failure surface. Fix its two concrete low findings before manifest reuse: derive toolkit strict status from an exact locally assigned code/private status, not diagnostic substring or prefix; wipe a partially constructed detail using its live byte length before clearing it on OOM. Existing test_image_workflow.py already contains actual typed-tool and interception failure parsing/zero-side-effect cases, so the review's missing-test concern is resolved by those raw tests and N001 execution, not by deleting a criterion. Deterministic post-serialization concurrent cancellation remains an acceptance case to implement with a test-only interposition copy of the real serializer, no production hook or fake provider boundary. Native ABI/status/layout/ordinary error formatting remain unchanged.

Read-only reviewer4b77fb2f-040b-41a8-a63c-ed4683e6d69d found two event-mode defects. Native stdout must NEVER infer unobservability from a timeout and fall back to a blocking write. Remove the heuristic; represent wasm stdout readiness explicitly at the existing net/poll seam, with actual Node/browser checks rather than a fourth platform branch in shared event code. Add a real already-full-pipe test whose cancellation occurs after the old500ms threshold. Preserve inherited descriptor flags and bounded memory.

In-process synchronous terminal tools must consult the existing cancellation probe without re-entering the active backend. Handle cancellation both while draining output and after a child closes stdout but remains alive; reap actual owned children/descendants and spare unrelated sentinels. Use the existing process seam and a bounded wait, not a frontend callback that can recurse into the backend. Explicit background-terminal lifecycle is not silently redefined. C124-events adds real shell and MCP cancellation and initial-full-pipe oracles; relevant mutants remove each corrected guard. A fresh actual-code review of these changes precedes jobs reuse. This expands event-worker ownership only to tools_shell.c, the existing net_wasm.c readiness seam and needed bounded tests/docs; no new goal/contract or arbitrary unrelated writes.

Native release size remains a blocker in the default build. Independent review e18bd550-33d9-40a2-bd3d-a04de10b2c2c approved native CLI-only LTO with an explicit correction: all four compile/link sites using REL_OBJS include a dedicated generic -flto flag, including the separate dictation-fixture object and link. REL_CFLAGS itself, PIC/shared-library/debug/sanitizer/wasm/static-analysis flags remain unchanged. Exact production -flto must be remeasured; the prior =auto experiment is not final evidence. No budget, warning, test or platform guarantee is reduced. A new proposed ADR precedes the Makefile change.


### A10 deterministic late-cancel oracle — 2026-09-12T07:54:53.681207+00:00

`python3 -m unittest discover -s tests/abi -p test_toolkit_late_cancel.py -v` compiles a disposable copy of the actual toolkit and serializer. A test-only link wrapper calls the real serializer, then blocks at a condition-variable barrier; another thread calls the public cancellation API before the job continues. Control produces the local strict error detail; cancelled case must return CANCELLED with zero result bytes. The compiled mutant preserves detail despite final cancellation and must fail that exact state assertion; restoration must pass. No production hook, provider mock or public ABI change is added. Raw build/link/run commands and input hashes are retained by the fixture and the canonical run artifact. This covers finalization timing not reached by ordinary in-flight provider cancellation.


### A10 native-LTO compiler finding — 2026-09-12T08:16:21.594877+00:00

The exact new generic -flto build under Ubuntu's GCC reports maybe-uninitialized CPR row/column in decode_one; its former non-LTO build passed and Clang LTO passes. Do not suppress -Werror or the diagnostic. Initialize that local decoded-event aggregate to zero before the existing parser, preserving its valid-key/report behavior. This one-line initialization is within the introduced compiler-gate repair, not a TUI redesign. Existing real CPR/split-boundary TUI regressions and exact GCC release/dictation-fixture links must pass afterward. Original failing build logs remain preserved; new snapshot/retry rather than treating Clang as GCC proof.


## Amendment A11 — reviewed durable-job design and independent dependency order — 2026-09-12T08:42:20.974699+00:00

All original issue requirements and49invariants remain unchanged. The corrected canonical event implementation is independently approved by f2f228a4-ab71-4268-a5e0-960ac898b84c, its inspected source hashes still match, and primary runs N011/N012/N013 pass current unit, canonical event flow and actual terminal cancellation. The complete14-case event fault set compiled, failed its intended behavioral oracles and restored original build/unit/event/terminal/conformance checks. Generic mutation, full platforms and final integrated checks remain separate gates. No event approval substitutes for final runtime evidence.

Adopt the concrete durable-job design in artifacts/resume-20260912/jobs-design-v4.md with the two final precision corrections from independent reviewer263228c1-cea1-4d6d-a847-c62cbfe1eae3: EVERY dead-owner probe uses LOCK_EX|LOCK_NB (EWOULDBLOCK means active/uncertain, deny reclaim without waiting), while the reservation-record lock remains held through probe/state/read/rewrite; ownerless queued AND running records project interrupted/cleanupunknown. All state/reservation lock attempts are nonblocking or bounded explicitly; never wait on a different job's owner while holding an output reservation. Retry resets job/item cancel_requested and pending timing/errors for the selected new attempt. These close the reviewed potential lock-order deadlock without removing an outcome requirement.

Refine A1's incidental serial implementation order based on the actual dependency boundaries: after corrected event/process review, native jobs core may develop in the isolated jobs worktree concurrently with image manifest/export work. Canonical ask events and owned process launch are its execution prerequisites; derived exports are an artifact-kind integration, not a prerequisite to scheduling native ask/image work. No pattern crosses an unresolved review checkpoint: manifest-dependent artifact/replay wiring waits for its own corrected reviewed interface, and final I124/I127 cross-artifact checks remain pending until exports/manifests are integrated. #126 preview integration remains last. This is dependency sequencing, not scope reduction; none of the final original acceptance/mutation/platform guarantees is waived.

C124 additionally uses `python3 tests/integration/test_jobs.py <absolute tny>` and a Jobs adapter under the original image-workflow check if needed. Required outcomes: durable immediate ID/metadata/log paths; survivor of submitter loss; queued-vs-running cancellation linearization and verified owned cleanup; worker-loss interrupted+cleanupunknown; bounded actual concurrency; canonical ask JSONL preserved separately from lifecycle; collision/permission failure before provider spend; selective retry verifies successful ask sessions/results/logs and image manifests/output hashes before carrying them forward; stale success fails without spending; private prompt opt-out and no credential/provider-diagnostic persistence; CLI/tool/interception parity; explicit wasm unsupported execution with shared image behavior unaffected. All original M124 rows and the design's parent-loss/handshake/reservation/old-attempt/counter/secret/output-limit critical faults remain required.

Implementation remains local/uncommitted in /Users/tomas/projects/tny-open-issues-jobs-2026-09-12, references this exact contract/evidence/native goal, and has bounded ownership recorded in the worker run manifest. It creates no competing goal/contract and stops for an independent actual-code process/concurrency/permission review before final integrated reuse. No commit, push, CI publication, ADR waiver or issue administration is authorized by this amendment.


### A11 Linux fixture reaping environment — 2026-09-12T08:45:19.566359+00:00

Identical compiled subagent outcome tests fail without an adopting reaper and pass under `tini -s --`, with identical source and assertion; see nix-reaper-diagnosis/without-reaper.json and with-reaper.json. The failure is an orphaned killed grandchild remaining a zombie under the container's non-reaping init, followed by fixture early-return allocations; it is not permission to weaken the kill(pid,0) cleanup oracle or ignore sanitizer output. The Linux Nix test derivation will declare a test-only tini dependency and run its existing make test and shell-workflow commands under an adopting subreaper. Production binary/dependencies/behavior and Darwin test invocation stay unchanged. Verify this in a strict disposable Nix override before merging the declaration into the worker-owned Nix file, then rerun the default final flake. Missing reaping support remains a failure, never a skip.


## Amendment A12 — explicit export mechanisms and independent component boundary — 2026-09-12T08:48:41.280166+00:00

Adopt the export/contact-sheet design at artifacts/resume-20260912/export-design-v2.md, with the precise remaining condition from reviewer96e797ee-dc33-4008-84d8-be0f6fd523ff: create commit-stage files by openat(retained_parent_fd, randomly generated basename, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600), not by resolving the original parent pathname again. Keep the existing canonical output flock as an additive cooperating-writer guard. No-overwrite commits complete validated bytes via linkat+unlink staging (atomic EEXIST); explicit overwrite uses renameat without following the target. Recheck expected target identity and reject observed changes. No impossible global inode-CAS against arbitrary external namespace mutations is claimed; original source inode bytes must never be written.

ImageMagick never receives a user-controlled path as expression syntax. Read/hash approved source bytes once, write them under fixed generated ASCII names in a private directory rooted at a trusted literal temporary path, force a supported known coder for every input/output, and pass only finite enum/numeric options. Final encoded bytes are copied to the retained-parentfd stage only after the producing operation, a second full decoder invocation and exact dimensions/MIME checks succeed. Resource limits are the declared tool/cache limits plus a parent wall deadline and input/output/canvas bounds, not an OS memory-sandbox claim. Numeric labels are fixed bitmap glyphs, no user expression or external font.

Preserve original I125 behavior and all cross-feature gates. As an independent component refinement of A1, the native export/commit implementation may proceed in an isolated worktree after its two fresh design reviews and the read-only manifest/IO boundary review that found no guard, file-commit, immutable-record or hash-lineage defect. That review found two unresolved existing generation caller issues (retained-artifact failure visibility and post-grant artifact recheck); those remain active blockers in #127 and MUST NOT be copied into the new export callers. Export's own retained-result and exact grant revalidation have separate acceptance/review checks. No final manifest/export/job/preview row can pass until those original findings are corrected in the integrated source. Independent component development is not completion or a waiver of a blocked caller checkpoint.

C125: real optional magick7 produces exact fit/crop/pad canvases and deterministic ordered numbered contact sheets, while all original inputs are byte-identical; unavailable dependency, wrong decoder/format, partial conversion, cancellation, target/source aliases, concurrent target creation and unsafe expression-like filenames fail without corruption. All flags shared by CLI, typed tools and interception with operation-specific sensitive permission identities. A derived manifest records native:false and source hashes/IDs, transform settings and actual canvas; returned output is decoded and independently checked, not accepted on subprocess exit alone. Ordinary generate/edit never invokes local conversion. Wasm external transforms reject before input/output side effects while image generation/manifests remain usable. Existing SDK artifact/reference consumers must retain derived lineage; no separately unrequested export SDK surface is invented.

Add stable C125-export and C125-sheet checks using `python3 tests/integration/test_image_exports.py <absolute tny>` and preserve the original workflow acceptance mapping. Original M125 faults and explicit child-failure/alias/permission/cell-order/native-label/injection/output-commit guards remain planned before faulting. The worker references the same canonical contract/evidence/native goal; primary performs final conflict-aware integration, independent actual-code review and full integrated checks. No production original checkout, commit, push, release, issue administration or ADR exception is authorized.


### A12 real WASM output-backpressure case — 2026-09-12T08:54:42.183723+00:00

The existing net_wasm output readiness callback has a meaningful false branch. C124-events now also runs `TNY_TEST_WASM_BACKPRESSURE_REQUIRED=1 python3 tests/integration/test_wasm_event_backpressure.py` with actual Chromium and the actual web module: a host sink reports unavailable for1250ms while the real engine receives a local fixture response. There must be no output before readiness, at least8false polls, a complete canonical ordered stream and one terminal, exactly one provider request, no diagnostic noise and exit0. This is not a mocked C poll or fabricated JSONL. The ordinary native fixture sweep explicitly reports NOT RUN; only the browser-required invocation counts for this gate. Browser/tool absence under required invocation is a failure.


### A12 browser readiness mutation — 2026-09-12T08:56:49.748824+00:00

M124-WASM-output-readiness removes the actual host-sink callback decision in a disposable net_wasm translation unit, rebuilds the real web module and runs the same real Chromium fixture. Original/restored stream must wait at least1200ms with actualfalsepolls; mutant must compile and run the engine to exit0 but fail the readiness assertion (zero false polls / early output), not syntax/network/timeout. This supplements, not replaces, the native initially-full-pipe and existing14event faults. The production/frozen source must remain hash-identical after the isolated mutation.


## Amendment A13 — prepared image-plan ownership and retained failure across callers — 2026-09-12T10:04:18.664611+00:00

R127.4/R127.6/R127.8 and R122 caller guarantees remain active. Independent source reviews 5447d8ee-f452-43d3-bbe4-af41a364a412, b1175c1a-7f3e-4925-8ec7-ff8c53a4b832, 95c24fee-045d-415f-b6e4-df028ad59117 and f8262745-c272-4b43-9420-7dbf4fac2d4b exposed the artifact-only recheck gap, a third manifest resolution after permission, broken one-time grants, borrowed-string ownership, and retained-artifact error loss. The final design is artifacts/finish-20260912/manifest-plan-final-design.md with the following binding corrections; prior unapproved drafts remain preserved, not reported approved.

**Prepared ownership.** Prepare ONE owned tny_image_plan before permission; private tools_call owns the native plan and private tny_intercept owns the terminal plan, never both. Deep-copy prompt/provider/model/quality/size into the plan (including replay defaults and explicit overrides) and release every owned string/reference/source record in plan_free. This eliminates borrowing the interception parser's short-lived yyjson document. Both prepare and permission detail use one existing describe serializer; retain its ordinary field shape/order and default codex when plan provider is null. Using a replay's actual inherited provider is a necessary fail-closed correction: old exact rules/grants for a different described provider do not authorize that request. Document this, do not claim those erroneous grant keys remain compatible.

Execute precisely the retained plan using one shared service body and a new PRIVATE run_prepared wrapper taking mutable nonnull tny_image_plan*. Normal CLI/SDK run keeps normal resolution through the same body. The prepared path never reopens a source manifest or runs a second permission check; it honors an unchanged ALLOW_ONCE without creating a remembered grant. Reference actual hashes may be populated while expected hashes/settings remain pinned. All literal/option/count/strict/roots/guard/load/hash/commit/cancel checks remain. Permission-gated execution with a missing prepared plan must refuse; no null-plan fallback or lower-level route may silently re-resolve after permission. A source record edited after preparation cannot substitute its new settings/paths; either the originally approved bytes/settings run, or their exact loaded-byte hash fails before HTTP. This is the original no-permission-widening guarantee, not mandatory rejection of an unrelated metadata pathname change.

A const intercepted-handle pointer may hold a mutable owned plan pointer without any cast: standalone C11 compile/runtime proof is artifacts/finish-20260912/const_plan_probe.json. Thus the review's const-handle concern does not require widening every const executor signature. Call and intercept free paths, pending permission moves, cancellation and failed prepare must dispose the plan once.

**Retained failures.** Exactly result.committed + local IMAGE_MANIFEST_FINALIZE_FAILED selects retained JSON in typed tools, interception and native toolkit. Keep failure prefix/exit and explicit-JSON-only stdout. Native toolkit assigns IO(-7) explicitly and records private committed state. OOM remains first; committed local IO failure is returned before checking late cancellation; noncommitted cancellation/strict/OOM detail clearing remains unchanged. Existing result accessor and readonly SDK detail become a discriminated union: strict committed:false/path:null versus distinctly typed retained committed:true/path/bytes/operation/manifest/dimensions/status. No public ABI symbols/layout/version or generic error formatting change, no provider diagnostics/credentials/prompt in detail, no repeated paid request, no deletion of a committed artifact.

C127/C122 additions: actual prepare→ALLOW_ONCE→execute without a remembered grant; changed artifact-record mapping/provider after grant cannot substitute data; changed originally referenced bytes refuse before HTTP; both typed and intercepted variants; missing-plan refusal; real finalization-fault retained artifacts through CLI/tools/interception/native ABI/Python/TS; late cancel after commit retains IO detail while precommit cancellation clears. Controlled faults: bypass retained plan, reread source, drop caller detail, restore late-cancel precedence, bypass expected-byte hash. Exact original/mutant/restored checks and current integrated source manifests remain required. One additional discovered reserved-name boundary checks actual generated record names when the user's output basename already contains '.tny-image-'; a suffix detector must not miss the final real manifest suffix. This remains protection of existing immutable manifests, not a broader filename restriction.

A focused actual-code review is required after implementation and before reuse for preview/job/export integration. Implementation is isolated in tny-open-issues-manifest-fixes-2026-09-12; canonical contract/native goal and invariant ownership remain primary-owned.


## Amendment A14 — jobs review checkpoint: atomic retry and credential carriers — 2026-09-12T10:09:48.017585+00:00

All original I124 invariants remain active. The first concrete job slice was independently reviewed by b3784906-e36c-4a0d-b2a1-724583a1a6ee while changes were still isolated. Two material findings remain: retry can reset a newly running attempt from a stale pre-lock document, and image credentials are populated into ask-item child environments. The task-owned implementer was interrupted at this review checkpoint after preserving its full source manifest; this is not a completed implementation or discarded work. A fresh implementer resumes the same bounded component and original integration before-images, with no competing goal/contract.

Retry must acquire nonblocking job ownership BEFORE any state mutation, then re-read and validate terminal/attempt/revision/selection under the existing state transaction. No failed contender may reset metadata, write a misleading attempt snapshot, or leave an ownerless queued attempt. Use the same ownership-before-write discipline as submit; lock release/handoff and global output-reservation lock order must remain deadlock-free. All owner probes under reservation lock remain strictly nonblocking. Test simultaneous retries of one finished job against real process barriers: exactly one new attempt/supervisor, losing contender leaves the winner's state untouched, provider request count equals selected failed items, carried-success hashes unchanged. Failure before owner acquisition leaves prior record byte-identical; failure after an accepted transition records an honest failed/interrupted outcome.

Credential separation must cover both explicit private carriers AND inherited environment: image-only token/account/endpoint values cannot be reintroduced into an ask child from its parent environment, and image children cannot inherit chat-only carriers. Preserve the actually selected chat provider's required resolved authentication, including legitimate Codex chat account/token when that is the selected conversation provider, via a distinct chat credential mapping. Do not fix the leak by breaking Codex ask jobs or provider selection. No secrets enter argv, metadata, logs or diagnostics; sentinels in tests are fake only. Test different chat/image accounts, inherited carriers, actual resolved request headers, and absence of irrelevant credentials in the item child; never echo real process environment.

C124 adds these deterministic concurrent retry/failure and credential-isolation cases; compiled controlled faults must reintroduce stale retry state, owner-after-write ordering, and wrong-kind/inherited carrier leakage and be caught by the matching assertions. Original lifecycle/crash/cancel/batch/reservation/verified-retry requirements are not replaced. A fresh actual-code review of the corrected integrated job component is required before finalization.


## Amendment A15 — first preview slice: captured bytes and explicit queue lifecycle — 2026-09-12T10:12:39.379665+00:00

All original #126 behavior remains required. Design reviews b2f646ac-4a4e-4e3d-8c41-f3ae704b0b9e and 28f21ad3-ff47-4042-9717-6d1e52a323f9 verified the actual one-queue bridge and identified missing batch-readiness, terminal disposition and manual-compatibility semantics. This amendment resolves those findings before a bounded first implementation slice; earlier drafts remain unapproved as written. Full generation/export/job preview integration remains a later reviewed phase.

The existing tools queue becomes one owned array of entries (path, exact loaded bytes/length/MIME/hash, manual/preview origin), not a second uploader/queue. Capture bytes once when accepted; the same immutable captured bytes feed the existing user image-parts builder on flush. Ordinary initial --image uses its path-loading wrapper around that same builder. Existing count/per-image bounds, roots and false-policy guards remain; preview additionally requires configured-true support at enqueue and flush. No output-file reread can substitute a later generation. Clear/free ownership is explicit and covered on every exit; private struct changes do not alter libtny ABI.

**D1, ready for actual continuation.** New preview queue admission requires a real native OpenAI-compatible owning backend, a currently active tool batch, no terminal/cancellation/denial state, and an available next request within the turn's step budget. Mere runner turn_active while streaming is insufficient. The new engine/backend preview entry point checks this in the owner loop and returns unavailable_session/turn_not_ready instead of queued when it cannot participate in that batch's next request. Keep manual image_attach/initial --image semantics unchanged. No queued success from an idle/completed/no-more-steps session.

**D2, atomic failure then terminal cleanup.** Flush checks all entries before mutation. On incompatible preview in a mixed batch, it preserves every entry/byte/count and returns an explicit preview-fatal outcome; nothing is partially delivered or silently dropped. The owning backend surfaces IMAGE_PREVIEW_NOT_DELIVERED, prevents the next POST and ends the turn with the appropriate failure. After recording that terminal non-delivery, the owner deliberately frees the pending batch and resets count, so it cannot poison later turns or retain64MiB indefinitely. Cancellation/termination/step exhaustion with accepted previews similarly has an explicit not-delivered cleanup disposition. Direct refusal tests still observe an unchanged queue before owner finalization, as A4 requires. A later allowed turn starts without stale preview bytes. Queue acceptance is time-local, not a claim that the model saw pixels.

**D3, preserve manual-only behavior.** Use an additional internal flush outcome/out-param or compatibility wrapper to distinguish preview-fatal from ordinary/manual failure; the existing manual-only warning/continuation behavior is not silently changed. Only a batch containing preview entries takes the new explicit fatal/non-delivery lifecycle. Unknown policy still permits all-manual capture/flush and refuses automatic preview; configured-true compatible mixed batches send all entries exactly once in queue order, with truthful manual/preview/mixed origin text.

**Control transport.** Refactor existing control exchange into a private status/error-code/message-returning helper with NO stdio, also in its wasm branch. Existing command wrappers restore their current printing/exit behavior. Add exact tool-role image_preview op, id/path/expected64hex validation and OPTIONAL response status/error_code fields; do not change existing ok/answer/error fields. Use explicit switches for each op, never a binary ternary fallback to manual attach. Receiver verifies roots, owning batch readiness, configured-true capability and captured-byte hash before queued reply. No acknowledgment retry; uncertain socket loss is failed/unknown delivery, not success or a second enqueue.

This first slice implements/tests only captured queue, loaded builder, receiver and reusable control primitive. Full --preview generation UI/derived sizing/lineage are still pending until reviewed manifest/export/job interfaces integrate; no foundation-only issue completion. C126 adds same-output-path twice before one batch flush, manual unknown vs preview true-only, source mutation after queue, real next-request exact byte oracle, roots/hash/capacity refusal, streaming-without-tool-batch/no-more-step refusal, mixed fatal preserves-then-owner-clears/no-post, subsequent turn recovery, legacy manual behavior and control-role/status compatibility. Planned critical faults reread filenames, bypass preview origin policy/readiness/hash, continue POST after fatal, omit terminal cleanup, or fallback to manual control. Each must compile and fail its intended behavioral assertion with original/restored passes. A fresh actual-code review is required before full preview pattern reuse.

## Amendment A16 — delivery authorization — 2026-09-12

The user's current request authorizes completing HANDOFF.md through commit,
push of a feature branch to origin, and PR creation for review. This supersedes
only earlier no-commit/no-push/no-PR language. It does not authorize merging,
direct-main pushes, releases, deployments, issue closure, weaker acceptance
gates or discarding existing work. Initial contract bytes remain immutable.

Use `feat/durable-image-workflows` at the preserved baseline and dirty tree.
All 49 invariants remain active. Preserve the ADR collision blocker I-G6/C-G6
until the user resolves the incompatible baseline preservation/uniqueness
requirements. If unresolved gates prevent completed delivery, publish an
explicitly draft/incomplete review PR and list every unmet gate; do not mark
the native goal or six issues complete. Fresh source-bound evidence lives in
`artifacts/delivery-20260912/`; historical worker passes are not combined proof.

## Amendment A17 — explicit generated-artifact preview design — 2026-09-12

Fresh design reviewer `6ac36b843180179d` conditionally approved the smaller
full-resolution-first design, with no automatic downsampling. Its source-based
review and required cases are retained in
`artifacts/delivery-20260912/preview-design-review.md`; ADR 0097 records the
decision. This preserves A1's explicit bounded-failure alternative and does not
remove any #126 behavior, platform, mutation or prerequisite review gate.

Generation/edit/replay/export/contact-sheet preview is explicit only. The
producer supplies the exact validated output digest before irreversible
commit, including no-manifest mode. A selected artifact/job reference is owned
and resolved once before permission; a job selection pins attempt and item.
Preview uses the existing queue/control mechanism with real owning-batch
readiness, configured-true input policy, unchanged bounds and exact loaded-byte
hash checking. No implicit conversion, regeneration, retry or manual fallback.

One shared orchestration boundary preserves successful artifacts and generation
exit 0 on preview failure, returns a separate safe nested status and actionable
fallback, and never reports visual approval. Generation failures and retained
manifest-finalization failures do not queue previews. Permission details bind
the requested conversation upload without changing ordinary non-preview grants
or re-resolving the approved plan. SDK toolkit operations remain metadata-only;
preview misuse must fail explicitly rather than be ignored. Terminal cleanup
accounts for unsent transcript messages as well as pending queue memory.

C126 adds the named Preview integration/mutation matrix in that design record:
actual next-request CLI/tool/intercept parity; explicit-only defaults;
capability/readiness; producer identity and captured bytes; selected derived/job
lineage; bounds/roots; retained failures; actual ALLOW_ONCE; terminal recovery;
control correlation/split boundaries; SDK and browser-WASM behavior. Original
C126 and all original mutation rows remain active. Implementations may not reuse
the rejected A13/A15/export/job patterns before corrected-source review passes.

Delivery quality evidence may additionally run the unchanged project commands
in a task-owned candidate worktree containing every intended delivered source,
test, configuration and dependency, with a real Git inventory. Frozen historical
probe/before-image artifacts remain untouched in canonical evidence and are not
product sources. D003's canonical format failure on archival probes remains a
failed run; no format rule is suppressed or weakened. Candidate checks must bind
to exact file hashes and publication must verify those hashes against staged
inputs. This is not permission to omit any delivered source from quality gates.

## Amendment A18 — parallel independent preview implementation — 2026-09-12

A13 owned plans, A15 captured queue, A12 exports, and the image-service
producer-identity/no-replace boundary now have corrected-source approvals.
The coordinator may implement and test generation/export/manifest-selected
preview against those approved boundaries while jobs cancellation/retry
corrections finish separately. This is independent component scheduling, as
in A12, not permission to reuse rejected job code. Job-selected preview and
I127.8 remain blocked on the corrected real jobs resolver and integrated
lineage tests. No stub, fabricated successful selection or missing-case skip
may count as delivery. The full C126/ADR0097 design, all 49 invariants and all
original platform/mutation/review gates remain active. A fresh actual-code
review is required after this preview slice and after job selection integration.

## Amendment A19 — full verification and merge authorization — 2026-09-12

The current user goal explicitly requests review of the complete intended
delivery, completion of its scope, passing quality/tests, and merge to remote
main once verified. This supersedes A16 and historical handoff prohibitions on
merging; it does not waive acceptance requirements, authorize issue closure or
release/deployment, or permit discarding preserved work. PR #130 remains draft
and incomplete until the full current-state gate passes.

The active delivery thread goal is `01a09677-b72d-7e51-9c8d-1c917d0a6286`, read
through `get_goal` on this continuation with status `active`. It incorporates
the full original six-issue scope and all 49 active invariants by this backlink
to this canonical contract and `evidence.md`. Available goal tools cannot edit
an active objective to add the paths; the existing goal is retained. The
historical implementation-goal records remain preserved and are not rewritten.

The historical ADR conflict and host-package incident have been presented for
explicit user disposition. No answer is inferred from time elapsed. Independent
review, implementation integration and verification continue while those
decisions remain pending.


## Amendment A20 — owned job artifact integration — 2026-09-12T17:03Z

R127.8 and all I126/I124 invariants remain required. Source design by
/root/preview_review and independent challenge by /root/ci_triage conditionally
approve one owned metadata-only selection into the existing image plan/preview
selection. The proposal and its six mandatory precision corrections are frozen
here before implementation; they do not approve the rejected jobs process code.

Selection reads one bounded, strictly typed internal job snapshot without
jobs_project, retry, provider initialization or artifact hashing. Require image
job kind and succeeded selected item, but not whole-job completion. Validate
positive integer attempts: ordinary item_attempt == projection_attempt with
carried_from_attempt ==0; carried item_attempt == carried_from_attempt <
projection_attempt. Copy job ID/index, both attempts, carried origin, canonical
absolute path, producer SHA256/bytes and optional manifest/operation identity.
Validate job location and confine record-derived paths before opening a referenced
manifest. A declared manifest must pass the shared strict loader and agree on
successful committed path, hash, operation and applicable bytes. Missing declared
manifest fails; only explicitly absent persistence is optional.

Job success must adopt correctly typed producer result SHA256/bytes and verify
actual disk bytes against them. Missing producer identity or replacement cannot
be converted to success by computing a new digest. No-manifest output keeps a
null manifest; do not fabricate one. Existing stricter paid-retry checks remain.

One prepared selection binds permission details to all identities and the edit
provider or preview conversation upload target. Execute that same owned plan
under ALLOW_ONCE; never reload mutable job/manifest records after approval or
create a remembered grant. Existing loaded-reference expected hashes and captured
preview bytes remain authoritative. Add a bounded optional job provenance object
to reference serialization, strict parsing, replay copying and derived-source
records. Absence preserves old records and ordinary permission identities;
malformed present provenance fails. Replay uses stored paths/hashes/provenance
without job lookup. Preview results expose pinned job identity even without a
manifest. These are private structs/additive metadata, not public ABI changes.

Preview grammar chooses exactly --manifest RECORD or --job ID --item N. Edit
accepts the complete job/item pair as a reference within the existing total limit,
alongside explicit file/artifact references with preserved documented order.
Reject incomplete pairs and selectors on generate/replay. CLI, typed tools and
interception use the same context-aware preparation and validation. CLI initializes
only paths/allowed roots needed for metadata lookup, never an unrelated chat
provider or credentials. Wasm job selection explicitly refuses through the
existing platform seam; ordinary replay of stored provenance remains shared.
SDK direct job/preview selectors remain rejected while existing manifest
operations keep working without ambient session authority.

C127/C126 adds actual next-request selected bytes on both wires with zero image
requests, actual edit upload and manifest/replay provenance, one-time permission
and later re-prompt, post-grant job/manifest mutation without substitution,
post-grant artifact change refusing before upload, capture stability, carried
attempt1 under projection3, malformed attempt tuples/types/IDs/indices, foreign
roots and mismatched manifest path/hash/bytes, no-manifest producer mismatch,
optional-provenance round trips, CLI/tool/intercept parity and wasm/SDK refusal.
Controlled faults must challenge producer-digest adoption, ownership reload,
expected-hash retention, attempt/provenance preservation and selector rejection.
Earlier fault families and all final platform requirements remain active.

Corrected native jobs ownership must receive source review before integration.
MSYS2 is a mandatory pending native implementation, not an allowed unsupported
fallback. Once Linux/macOS ownership is corrected and reviewed, shared job/image
integration may proceed independently of isolated MSYS2 host-seam work; the final
all-platform gate remains incomplete until both are integrated and verified.
This is dependency scheduling, not reduced scope. A new immutable ADR will record
this decision before the integration code is written; baseline ADRs are preserved.


## Amendment A21 — native MSYS2 ownership and admission — 2026-09-12T17:30Z

All native Windows/MSYS2 R124 and global platform guarantees remain mandatory.
Independent /root/jobs_process_review approved DESIGN-v2.md, SHA256
 df8061b1a6e94b9b6bc28f8399c88c2d1bb7fabca45e3b049724ad65fc8070ce,
after v1's stopped-bootstrap and transaction gaps were corrected. Source design
and real guest probes remain in artifacts/review-merge-20260912/msys-jobs-design.
The guest runs real x64 MSYS under Windows ARM emulation; it is not native x64 CI.

Implement one noninheritable supervisor-owned lifetime Windows Job, containing
bootstraps from creation, and nested per-item Jobs retained by opaque scopes.
Keep lifetime handle until process teardown, so supervisor death also contains
pre-admission bootstraps holding temporary inner handles. Item self-admission
happens before config/provider work and releases only after exact private ACK
and explicit GO. Await admission outside job state locks. Count pending admission
against concurrency; re-read attempt/cancel under the state transaction before
nonblocking GO, then permit normal prompt writes. Failure after GO is started
execution with explicit cleanup, not a fictitious never-started item.

Only an explicitly unreaped direct child under SIGCHLD default/single reaper may
be signalled by POSIX PID before GO. After GO use retained Job authority, never
persisted or enumerated PID authority. Preserve root exit, forced-stop reason,
log drain and scope cleanup independently. Normal root exit must clean residual
descendants before terminal publication, reservation release or slot reuse;
Windows force-termination status0 never independently establishes success.
Strip/validate all private admission fields (including ask_env); stage CLOEXEC
pipe mappings and close child fd3 before ordinary execution. Bounded membership
queries, retained observational handles, direct-child reaping, ActiveProcesses0
and strict ESRCH for captured POSIX identities establish cleanup; query/mapping
uncertainty remains unknown and never grants signalling authority.

C124 adds actual integrated admission pauses before open/after open/after assign/
before close, individual cancellation and real supervisor loss, bootstrap exec,
normal-root-exit descendants, nested outer Jobs, unrelated siblings/sentinels,
fd collisions/closed endpoints, cancellation/commit failure around GO, environment
stripping and native x64 hosted CI. Existing macOS/Linux tests and all strict
cleanup/fault oracles remain. The Windows implementation is isolated; corrected
shared job/image work may proceed independently under A20. No platform is waived.


## Amendment A22 — native release JSON inlining — 2026-09-12T17:54Z

I-G1/I-G4 and the unchanged Linux1MiB gate require a smaller final native
executable. Frozen current GCC13.3 aarch64 build is1117160B (D067). The pinned
yyjson0.12 header explicitly permits overriding yyjson_inline; removing its
always_inline annotation while retaining static inline and existing -Os/LTO
yields986088B (D074), with unwind metadata preserved. Independent reviewer
/root/jobs_process_review approved this source-backed design before changes.

Use a dedicated REL_INLINE=-Dyyjson_inline=inline only at the same four native
CLI/fixture compile/link recipe sites as REL_LTO. Do not change vendored files,
DEFS, common release flags, PIC/library, debug/sanitizer, strict-analysis or wasm
flags. Header and implementation helpers remain static inline; optimizer choices
may affect latency, so record paired release behavior/benchmarks, compiler/target
checks, actual final size and effective flag separation. The override is no
permission to drop features, diagnostics, platform coverage or the size limit.
A small recipe-flag regression must fail if the override leaks into library/debug/
wasm builds or is absent from real native release/fixture commands. Actual final
GCC/Clang/MSYS/musl release tests and full combined acceptance remain required.


## Amendment A23 — observed cleanup uncertainty retains output claims — 2026-09-12T18:10Z

Clarify A11/A21 without removing either guarantee: owner-loss projection can be
reclaimed after actual owner freedom and a nonblocking held state transaction;
a supervisor's observed cleanup failure cannot be treated as that projection.
Root wait failure itself can yield interrupted, so state+cleanup alone is not
a sufficient discriminator. Independent /root/jobs_process_review approved an
explicit cleanup_hold latch with conservative legacy fallback before writes.

A supervisor finalization atomically sets cleanup_hold=(cleanup != complete)
with terminal state/cleanup. A true or malformed latch denies reclaim, retry
and rm; projections preserve a true latch. A false/missing latch alone never
authorizes unknown cleanup: only exact canonical owner-loss projection
(state interrupted, cleanup unknown, error_code JOB_INTERRUPTED), actual free
owner and held nonblocking state locks retain A11 reclaim. Other unknown
cleanup, including supervisor-published interrupted/IO, remains held. Read/lock
uncertainty denies. Retry rechecks under its final state transaction; rm checks
before tombstoning. Complete cleanup retains ordinary release behavior.

C124 adds actual post-exit contender denial, same-job retry/rm denial and
unchanged claim/record assertions for observed unknown cleanup, including root
wait loss; canonical abandoned-owner projection remains reclaimable and its
existing tests must pass. This adds no automatic recovery or new authority to
kill a process. Existing process cleanup and original scope remain required.


## Check routing clarification — 2026-09-12T18:32Z

The final reconciliation confirms C125/C126 implementation checks moved to
dedicated maintained modules. The original test_image_workflow.py -k Export
and -k Preview commands currently discover zero tests and do not count as
acceptance. C125 uses test_image_exports.py (including Export/ContactSheet and
real ImageMagick positives). C126 uses test_image_preview_workflow.py,
test_image_preview_queue.py and test_job_artifacts.py plus the actual browser
checks. Existing dimensions/manifests workflow filters remain as mapped. This
changes command routing only, preserving every original outcome and fault row.
Each final filtered check must discover its intended nonzero cases; a zero-case
exit0 or required positive skipped for a missing tool is not a pass.


## Amendment A24 — Linux Clang native release size — 2026-09-12T19:06Z

The final Linux Clang -Os release is1117912B and fails the unchanged1MiB gate.
The same frozen1141-input Clang release with trailing -Oz is986840B. Independent
/root/jobs_process_review approved the narrow policy before implementation:
compute the actual full CC command's --version once; only Linux Clang native
CLI/fixture compilation and linking receive a dedicated REL_SIZE_OPT=-Oz after
common -Os. GCC, macOS, MSYS, PIC/library, debug, analysis and wasm remain as
before. All four native recipe sites and native fixture flag captures agree.

Keep compiler detection explicit (including multiword wrappers), no toolchain
switch/fallback/vendor edit/size waiver. Final effective-flag tests cover Linux
Clang versus GCC and Darwin/MSYS, real release behavior and paired latency.
The previous Clang failure remains visible; exact final-policy size is required.


## Amendment A25 — Installed Nix payload size and runtime path — 2026-09-12T19:22Z

ADR0103 records the independently reviewed package correction before implementation.
For Linux CLI packaging only, set the lazy OpenSSL RUNPATH at link time and disable
the pinned shrink-only ELF hook; preserve alignment, unwind, hardening and no
SSL/crypto DT_NEEDED. Keep wrappers and pre-fixup size; also check actual installed
payload against the unchanged Makefile-owned limit and run installed TLS fixtures
with test-only declared dependencies. Darwin and libtny retain their current policy.
The instrumented original installed1052896B failure remains evidence; measured
candidates986088B with real HTTPS pass are design proof only. Final integrated
sandboxed packages/flakes and platform matrix remain required. No budget waiver.


## Amendment A26 — Runner ownership through quiescence — 2026-09-12T20:47Z

Hosted Nix x86 at f968265 exposes an early-release runner race. The independent
static review in artifacts/review-merge-20260912/runner-quiescence-design/review.md
approves ADR0104 before product writes: ownership before bind and all child
storage work; retain through final save, engine/MCP shutdown, log flush and
socket unlink; release before bye. Preserve caller-owned parent descriptor
handoff. Failed acquisition cannot mutate another owner's state. Reload resumed
state under already-acquired ownership and reconcile again before provider work.

Existing isolation/background/steer/task/runner gates remain required. Add
behavioral assertions for startup refusal without mutation, serve-error retained
ownership, refreshed resume state, and writer freedom at bye. The orphan resume
fixture must observe flock freedom in addition to durable terminal status; no
sleep-only workaround or lost scope. Native/WASM and final integrated quality,
size and hosted platform gates remain required; earlier failures remain evidence.


## Amendment A27 — Native MSYS private filesystem — 2026-09-12T20:54Z

ADR0105 and the independently approved Windows DESIGN.v2 (SHA256
2e10a81a4eb4edad11517f1e81a97cca853940f76abbc9795eed0d5f6db69233)
precede this correction's product writes. The six caller privacy, confinement,
retained-parent and atomic publication guarantees remain unchanged. Implement
native atomic private ACL creation and retained object operations, exact alias
and name handling, explicit committed cleanup state and an owned transform stage.
Pathname-consuming stages require proven pinning or refusal before spawn.

First primitive slice receives independent review before caller rollout. Existing
POSIX mode/behavior and WASM boundaries remain; native MSYS tests verify effective
access rights independently instead of trusting noacl stat modes. All original
feature checks, failure/alias/parent replacement cases, actual Windows guest flows,
hosted native x64 unit/jobs and unchanged size/quality gates remain mandatory.
No global remount, automatic ACL migration, test skip or scope waiver is authorized.


## Continuation A28 — External merge observed — 2026-09-12T21:19Z

GitHub records thehumanworks merged PR130 at21:02:59Z, headf968265, merge1bf59d1.
This agent did not perform that merge. Main4b859f8 adds only generated Pages
WASM artifacts afterward. The original39R/49I and quality requirements remain;
no waiver is inferred from the external repository action. The reviewed runner
correction and Windows review/implementation gaps remain outstanding. Preserve
the user's merge and proceed on fix/durable-workflow-verification based on current
main. A follow-up review PR is within the already-authorized correction/delivery
work; its eventual merge remains conditional on the full active gates.
