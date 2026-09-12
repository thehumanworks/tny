# Handoff: complete issues #122–#127 and deliver a review PR

> Delivery is in progress on `feat/durable-image-workflows`. The recovery plan
> below is historical. Read
> `docs/verification/open-issues-2026-09-11/DELIVERY.md` and the current issue
> ledger for integrated code, fresh reviews, evidence and unresolved gates.
> No issue or native goal is complete merely because a draft PR is published.

## Mission and authority

Finish and verify all six snapshotted issues in:

`/Users/tomas/projects/tny-open-issues-2026-09-11`

Then **commit the task changes, push a feature branch to `origin`, and create a PR for review**. The user's latest request explicitly authorizes this delivery. It supersedes the earlier contract's prohibition on commit/push for this task, but does not authorize merging, direct-main pushes, releases, deployment, issue closure, weaker acceptance gates, or discarding existing work.

All six issues remain incomplete. This file is a recovery guide, not a replacement verification contract or evidence of completion. Continue unblocked work independently. Do not manufacture an all-green verdict to meet the delivery objective.

### Required user decision: ADR conflict

Baseline ADR prefixes `0030` and `0045` each occur twice. The contract requires both unchanged finalized filenames/bytes and globally unique prefixes. These requirements conflict; **no exception is authorized**.

Recommendation: explicitly grandfather only these two baseline collisions, preserve their files, and require unique prefixes for new ADRs. Tradeoff: two legacy collisions remain. Renumbering instead requires permission to relax preservation. Keep I-G6/C-G6 blocked until the user resolves this. Continue other work meanwhile. The request to commit/push/open a PR does not waive this conflict.

## 1. Read and reconcile before writing code

Read `AGENTS.md`, `docs/product.md`, `docs/architecture.md`, `docs/implementation-plan.md`, then relevant image/tool/platform docs. Load applicable verification, mutation and push skills. If `tny skill show` is unsupported, locate installed skill files rather than assuming they are unavailable.

Canonical records live under `docs/verification/open-issues-2026-09-11/` (called **V/** below):

- `contract.initial.md`: immutable initial contract. Verify its recorded hash; never rewrite it.
- `contract.md`: all requirements, invariants, acceptance commands and later amendments. Read the whole file; opening status text is historical.
- `evidence.md`, `issue-ledger.md`, `review-dispositions.md`.
- `artifacts/issues.snapshot.json`: original issue descriptions and acceptance. Newly opened issues do not expand scope.
- `artifacts/baseline.json`, later source manifests, owned-file lists and before-images.

The ledger and summaries lag behind late worker results. Reconcile raw results and exact source versions before trusting a status. Record the new delivery authorization as a dated amendment without changing the immutable initial contract or relaxing another guarantee.

### Repository and environment facts from this audit

- Remote: `origin`, `https://github.com/thehumanworks/tny`.
- HEAD was **detached** at baseline `b80c04b9df740c8388da03991cf4808c07e9cb50`. Recheck current Git state before branching.
- Create a meaningful feature branch at existing HEAD without resetting or losing the dirty tree. Check branch collisions first.
- There were 52 modified tracked files plus many untracked sources, tests, ADRs and records. `git diff --stat` omits untracked work. Do not stage indiscriminately.
- Mac shell, Git, file reads and process inspection work now. Build/test access was not exercised in this audit. The earlier blanket Mac-execution blocker is stale for these operations.
- Saved native goal/thread: `01a091ec-193f-7a91-b8e3-b28e757eff19`, last recorded active with 49 invariant IDs. The audit did not reread the live API. Existing records describe Codex app-server `thread/goal/get`; discover actual access/schema, do not guess or create a replacement goal.
- The initial-contract hash and all 88 baseline ADR-directory files matched during reconciliation. Preserve them.
- A task Windows QEMU process was visible. Ownership, guest reachability and tny runtime verification remain to be established. Do not kill it merely because it exists.

Inventory task-owned processes and worktrees without disturbing unrelated processes, work or goals. Record source hashes and ownership before integration.

## 2. Current implementation map

“Integrated” means present in this dirty worktree, not committed, accepted or released. Worker completion reports and old passes are not final combined-tree proof.

| Issue | Canonical tree | Remaining work |
| --- | --- | --- |
| #122 — dimensions/strict size | Shared PNG/JPEG/WebP inspection; requested/effective/actual metadata; strict output preservation; structured failure details through CLI, tools, interception, ABI and SDKs. Previously unconfirmed failure-parity worker returned and received review. | Current-tree late-cancel/caller checks, native-versus-derived assertions after exports, complete mutation/platform proof. |
| #123 — subagent contract | Private launch, inherited configuration, permission ceilings, redacted diagnostics, durable child state, cancellation and lifecycle. | Repeat diagnostics/nested-child/unsupported-context checks after all integration; global/platform gates. |
| #124 — durable jobs | Canonical events prerequisite is integrated, including `--events=jsonl`. | Substantial jobs implementation and later corrections exist separately. Review, merge, fix help inventory and run manifest-aware retry end to end. |
| #125 — exports/contact sheets | Design and shared manifest/IO prerequisites; export service absent. | Substantial worker implementation exists. Review dispositions, conflict-aware merge, timeout/platform and cross-feature proof. |
| #126 — capability preview | Provider policy, truthful capability wording and schema/dispatch/queue/turn admission gates. | Captured-byte queue/control worker exists separately. Generated-result preview, fallback/status and generation/export/job integration remain incomplete. |
| #127 — manifests/replay | Substantial versioned records, intent/finalization, writer guards, hashes, replay/artifact references, caller integration and tests. | Separate owned-plan/permission and retained-error corrections need review/merge; derived/job/preview lineage and final proof. |

### #122 and #123: preserve and revalidate

Relevant paths:

- `src/core/image_dimensions.{c,h}`, `src/core/image_service.c`, `src/lib/toolkit.c`.
- `src/core/subagent.{c,h}`, `src/util/process.c`.
- `tests/integration/test_image_workflow.py`, `test_subagent.py`, `test_subagent_diagnostics.py`.
- `tests/abi/test_toolkit_late_cancel.py`, `tests/fixtures/toolkit_late_cancel.c`.
- ADRs 0087, 0089 and 0091.

Preserve generic-error redaction, local-only strict-failure detail, public ABI compatibility, cancellation precedence and destination retention. N010 late-cancel proof used reconstructed pre-manifest sources after N009 raced unfinished manifest code. Repeat on combined sources.

Subagent frozen-snapshot Linux Valgrind, Mac leaks, real nested children and controlled-fault results are strong historical evidence, not final closure.

### #127: integrate the corrected shared boundary first

Worker: `/Users/tomas/projects/tny-open-issues-manifest-fixes-2026-09-12`

Records: `V/artifacts/finish-20260912/worker-manifest-corrections/`

Read `result.json`, `progress.json`, owned-file/source records and raw logs. The worker reports implementation complete **pending fresh review**.

Known canonical A13 issues include repeated plan resolution between permission and execution, inconsistent artifact-only checks, displayed provider differing from inherited replay identity, borrowed plan settings, lost retained-artifact detail at callers, and reserved-name suffix handling. Recheck current source before applying changes.

The correction worker adds owned plans and `tny_image_run_prepared()`, resolved-provider permission identity, retained-failure propagation, SDK variants and tests. Its `test_manifest_permissions.py` and `test_toolkit_retained.py` were absent from canonical.

Review the **same owned plan approved and executed** guarantee: ALLOW_ONCE, pending permission ownership, artifact-only inputs, exact-byte hashes, inherited providers, missing-plan refusal, reserved names, retained IO detail and late cancellation. Cover typed tools, interception, ABI, Python and TypeScript. Follow A13/ADR 0095; repeated permission checks are not a substitute for an owned execution plan.

### #125: substantial code, not merged

Worker: `/Users/tomas/projects/tny-open-issues-exports-2026-09-12`

Records:

- `V/artifacts/resume-20260912/worker-exports/`: `progress.json`, `results.md`, `owned-file-hashes.json`, `fault-plan.md`.
- `V/artifacts/finish-20260912/review-exports-live-boundary.result.json`.

Worker-only files include `src/core/image_export.{c,h}`, `src/util/image_transform.{c,h}`, `tests/integration/test_image_exports.py`. Shared manifests, IO, tools, interception, CLI/docs and build/test inputs also change.

**Merge hazard:** worker shared image-service/toolkit files retain older canonical code, not A13 corrections. Do not copy the whole worktree over canonical. Merge owned deltas against before-images after establishing the corrected manifest foundation.

Preserve explicit optional ImageMagick 7, full decoder verification, safe staging/finite arguments/forced coders, original preservation, exclusive destination handling, derived provenance and deterministic labels. Do not add an export SDK API, database, content-addressed store, automatic upscaling or converter-backed ordinary generation; see A12/ADR 0094.

Disposition review findings against merged code. One possible missing-macro finding was a review-scope artifact: `TNY_IMAGE_CODE_MANIFEST` exists in both inspected headers. Do not blindly implement every suggestion. Add the bounded timeout coverage that remains unrun.

### #124: corrected jobs need fresh review and integration

Worker: `/Users/tomas/projects/tny-open-issues-jobs-2026-09-12`

Records: `V/artifacts/finish-20260912/worker-jobs-corrections/`

Read `result.json`, `handoff.md`, ownership/hash records and raw tests. Earlier review found stale pre-lock retry state and mixed ask/image credential carriers. The **later** worker reports fixing both with real concurrent-process tests and 17 controlled faults. Do not present the earlier defects as necessarily unfixed or the later report as independent approval.

Remaining tasks:

- Fix missing jobs inventory in `tests/integration/test_help_flags.py` (worker handoff H1). Worker full `make test` fails there despite scoped passes.
- Apply the real manifest overlay and run the currently skipped lineage/retry end-to-end row (H2).
- Review ownership-before-mutation, reread/revision/attempt checks, immutable history, kind-separated payload **and inherited environment**, own-provider credentials, cancellation, reservations, bounded batches and selective retry.
- Security: worker reports accidentally logging the child's whole environment once, then deleting the log. Inspect retained evidence safely for exposure and record a disposition. Do not reproduce the dump or print credentials.

Canonical events include `src/core/event_jsonl.{c,h}`, `tests/integration/test_ask_events.py`, `test_ask_events_conformance.py`, `test_wasm_event_backpressure.py`. Repeat slow-reader, full-pipe SIGINT, memory-bound and actual WASM backpressure checks. Events alone do not deliver jobs.

### #126: queue prerequisite is not the complete preview feature

Worker: `/Users/tomas/projects/tny-open-issues-preview-2026-09-12`

Records: `V/artifacts/finish-20260912/worker-preview-queue/`

Latest result reports captured-byte queue/control work and ten controlled faults, but inherited-base leak/integration failures, pending WASM and an SSH pathname-loading exception. Reproduce/disposition these; “inherited” is not proof of a pristine-baseline failure.

Follow A15/ADR 0096: one shared queue, captured bytes, batch readiness, fatal-status cleanup and truthful admission. Implement generated-result preview with explicit fallback/status. Preview unavailability/failure must not destroy a successful artifact. Generation success or configured image-input support does not imply visual inspection. Connect full-resolution, derived and job identities without silently regenerating/replacing them.

## 3. Ordered completion plan

1. **Recover and freeze:** reconcile goal, workers, hashes, before-images, authorization, raw evidence and security. Establish a feature branch without disturbing dirty work.
2. **Review A13 independently and integrate:** use the owned-plan/retained-artifact boundary as the shared foundation. Run focused permission, replay, persistence, ABI/SDK, dimension and late-cancel regressions.
3. **Review exports, corrected jobs and preview queue in parallel where safe:** follow the contract's fresh-review independence requirements. Give implementation workers isolated ownership/worktrees. Use `tny ask -B --json` and collect with `tny session ID --wait --json`; do not use Grok CLI/tmux delegation. The previous audit's two Grok/two Astra, effort light arrangement is provenance, not a requirement to repeat it.
4. **Merge exports and jobs deliberately:** reconcile shared-file deltas; do not overwrite newer fixes. Update docs, Makefile, Nix source/test inventories and affected SDK consumers. Fix help inventory and supply real prerequisites for the skipped lineage test.
5. **Finish preview and cross-feature behavior:** generation/export/job attachment, captured bytes, capability fallback, selected artifacts, native/derived metadata, replay lineage and selective retry. Add full acceptance tests, not only helper tests.
6. **Freeze and verify the complete tree:** stop concurrent writes during gates. Bind results to exact inputs; repeat invalidated checks.
7. **Finalize records and deliver:** reconcile every invariant, review, mutation, failure, platform claim and authorization; then commit, push and create the PR below.

## 4. Verification contract and known gaps

This handoff comes from a read-only audit. No new builds, tests or mutations were run. Read C122–C127 and C-G1–C-G11 in the canonical contract for the full commands and matrices.

Historical evidence worth retaining:

- Subagent native checks, real nested-child fixtures, Valgrind/leaks and behavioral faults on frozen snapshots. Earlier invalid mutation attempts are not kills.
- `V/artifacts/finish-20260912/F008*`: 26 manifest cases passed on that snapshot.
- `F009*`, `F011*`, `F012*`, `F013*`: recorded quality, ABI, leaks and format passes.
- `F010*`: full Mac tests exit 0 but `source_unchanged:false`; `F010-input-change-disposition.json` records only an ADR change and explicitly requires a frozen rerun.
- Export worker reports 492 unit tests, 22 export cases, 8 sheet cases, quality/leaks on its Mac snapshot. Fault record has **seven kills and one survivor**, not eight kills. Preserve the specific limitation.
- Manifest correction worker reports 498 unit tests, ABI/SDK/leaks and ten faults, pending fresh review/platforms.
- Jobs correction worker reports 47 job tests and 17 faults; full tests fail on help inventory and lineage end-to-end skips.
- Preview queue reports ten faults but unresolved failures and platform gaps.
- Windows worker provisioned a guest and passed a C11 substrate probe, **not tny tests**. Windows ARM x64 emulation does not establish required native x86_64 CI coverage.

Required final work includes:

- Full `make test`, `make quality`, strict warnings, Linux analyzer, leaks and Linux Valgrind; ABI and Python/TypeScript SDK checks.
- Dimensions/strict-detail/late-cancel, subagents, events, manifests/permissions, exports/sheets, jobs and preview acceptance, including cross-feature paths.
- Planned behavioral mutations: original pass, compiled intended failure, restored pass and restoration hashes. Compiler errors, unrelated failures and skips are not kills.
- Required Linux, macOS, Windows/MSYS2 and actual browser-WASM execution, including documented unsupported paths and backpressure. Node/WASM is not browser-runtime proof.
- Affected Nix checks and final stripped release size on the required target.
- Resolve the historical full Linux dictation timeout rather than excluding its fixture.
- Required live-provider checks using existing authorization safely; request genuinely missing access. Do not substitute mocks/platforms without authorization. Hosted CI on an older revision does not verify this tree.

**Size correction to the earlier handoff:** historical Linux/Nix was **1,051,608 B > 1,048,576 B**, baseline **986,072 B**; isolated LTO measured **917,216 B**. CLI-only `REL_LTO = -flto` is **now in Makefile**, documented by ADR 0092. The delivered final Linux package still needs measurement. Do not raise the limit or claim unmeasured performance gains.

Tool availability, worker completion and old logs do not prove feature completion. If final sources change, repeat affected checks and invalidated whole-tree gates.

## 5. Preserve the strong work

Retain truthful image metadata, strict destination preservation, local-only error details, private/redacted launch and permission ceilings. Retain private immutable manifests, intent before paid work, exact reference hashes and paid-artifact retention on finalization failure. Use owned permission/execution plans, explicit optional conversion, real decoding and honest derived provenance.

Retain the real concurrency/process/cancellation tests and source-bound fault records, including failures, survivors and skips. Preserve immutable scope and baseline ADRs.

**Main risk: overlapping worker versions and evidence drift. Review and integrate existing work; do not restart it or mistake code volume for combined correctness.**

## 6. Commit, push and PR endpoint

The user explicitly requests this endpoint. Complete required work and verification first, subject to actual authority/access blockers.

1. Check remote/default branch, authentication, feature branch and worktree changes. Preserve baseline ADRs and the immutable initial contract.
2. Review the exact staged diff. Include task-owned new sources/tests/docs/build inputs and this handoff as appropriate. Check artifacts for secrets, oversized logs, generated binaries and unrelated work before staging. Preserve canonical evidence safely; do not blindly commit scratch artifacts.
3. Run required pre-push gates on final inputs and record exact source/commit association. Never label a failing/partial gate green.
4. Make a conventional commit or small logical series. Push the feature branch normally with upstream tracking; no force push or direct-main push.
5. Create a PR against the actual default branch with available GitHub tooling. Summarize each issue, architecture, tests, mutations/platforms and limitations. Reference #122–#127 without automatic closure keywords, because issue closure is not authorized.
6. Observe required CI on the pushed commit, fix attributable failures, rerun affected gates and push corrections. Do not claim CI success before observing it. Never merge the PR.
7. Return branch, commit SHA(s), remote, PR URL, checks/results and blockers. Mark the live goal complete only if every applicable invariant holds. PR creation alone is not completion.

If the ADR decision or another required gate stays blocked, complete unblocked work and ask for the precise decision/access needed. If delivery proceeds with unresolved gates, use an explicitly **draft/incomplete PR**, list every blocker and keep the goal/issues incomplete. That fallback is not the requested completed outcome.

## Audit provenance

Four `tny` background audits completed with `--effort light`:

| Session | Provider/model | Responsibility |
| --- | --- | --- |
| `ba235a162ff04ee4` | grok / grok-4.6 | #122 and #126 canonical implementation |
| `229e89dd4e0813ea` | grok / grok-4.6 | #123, events and jobs |
| `c7f4886ef0d12041` | codex / gpt-6-astra | #125/#127 and worker comparisons |
| `e1688fd851e422da` | codex / gpt-6-astra | Contract, late records, access and global gates |

Read with `tny session ID --wait --json` from this workspace if available. Findings were reconciled with newer worker records: jobs defects have reported corrections, captured preview queue work exists separately and permanent CLI LTO rules are present. This file does not depend on temporary audit copies.
