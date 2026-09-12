# Verification contract — open issues 2026-09-11

Status: ACTIVE, implementation NOT STARTED. Overall gate: INCOMPLETE.
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
