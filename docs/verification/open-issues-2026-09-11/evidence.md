# Verification evidence — open issues 2026-09-11

Overall gate: **INCOMPLETE**. Delivery reconciliation on 2026-09-12 supersedes
historical status paragraphs below. The feature branch is
`feat/durable-image-workflows`; user authorization now includes commit, push
and PR creation (A16), but not merging or issue closure. Baseline ADR prefix
collisions still block I-G6/C-G6. Fresh reviews rejected all four worker
components pending corrections; none is approved for reuse merely because
older worker checks passed. Current records:
[delivery reconciliation](artifacts/delivery-20260912/README.md).
Canonical contract: [contract.md](contract.md); immutable initial: [contract.initial.md](contract.initial.md).
Issue scope: [issue-ledger.md](issue-ledger.md) and [raw snapshot](artifacts/issues.snapshot.json).
Baseline: `b80c04b9df740c8388da03991cf4808c07e9cb50`. Initial contract SHA-256: `832e97ffd3ba8b5e21a60a356e13b8759e89efae348cac5f9fa41682fb31c6b1`.

## Execution and gates

A source-state assertion verified all 1,068 tracked inputs against the read-only baseline before writing this contract. Only verification documents have been created. See [execution record](artifacts/execution.jsonl).
Current native goal: active, identity `01a091ec-193f-7a91-b8e3-b28e757eff19`. The initial two rejected designs were amended; fresh reviewer a0cbdcbc-29b1-493e-b793-95e607ba0b3d approved the first #123 slice and the primary verified the preimplementation guard. The different fresh first-slice code review is still required.
Existing ADR global uniqueness: **BLOCKED** (0030 and 0045 collisions); immutable names/bytes are preserved.
Platform checks not yet available are blockers, not passes or NOT-APPLICABLE.

## Runs

Run ID | Checks | Input state | Environment/cwd | Command / procedure | Exit / result | Artifacts
--- | --- | --- | --- | --- | --- | ---
D001 | C-G9,C-G10 | clean b80c04b9df740c8388da03991cf4808c07e9cb50 | original checkout | native read-only Git / docs / file SHA256 discovery | PASS discovery, no code writes | [baseline](artifacts/baseline.json)
D002 | C-G4 | GitHub at recorded snapshot time | GitHub connector + authenticated read-only REST | open issues page1/page2, each comments endpoint | 6 issues, terminal empty page, 0 comments each | [snapshot](artifacts/issues.snapshot.json)
D003 | C-G9 | no implementation | installed Codex 0.155.0-alpha.3 | thread/list + thread/goal/get over native app-server | capability available; no matching open-issue goal; unrelated goals left intact | [native persistence read](artifacts/native-goal-persistence.json); unrelated goal details intentionally omitted
B001 | C-G2 | clean baseline source | task worktree, inherited TNY_TOOLS=terminal | make -j4 test | FAILED exit 2; inherited TNY_TOOLS=terminal | [full log](artifacts/baseline-test.log)
B002 | C-G1 | clean baseline source | task worktree | make quality | PASS exit 0, Darwin only | [full log](artifacts/baseline-quality.log)
B003 | C-G2 | clean baseline source | task worktree | make -j4 leaks | FAILED exit 2; inherited TNY_TOOLS=terminal | [full log](artifacts/baseline-leaks.log)
P001 | C-G3 | baseline AGENTS only | fresh Codex read-only session | capability probe, no children/writes | FAILED: tool host unavailable; not a review | final probe result retained in discovery scratch
P002 | C-G3 | baseline AGENTS only | fresh restricted Claude session 558de257-ac7b-4614-9414-3c0b95162027 | Read-only capability probe | observed # AGENTS.md; no writes/children; not a design review | actual successful read-only design sessions establish capability; probe is not a review

## Reviews, mutations and final verdicts

Design reviews A/B: completed, NOT APPROVED; see preserved reviewer results and review-dispositions.md. First-slice/boundary reviews: NOT RUN. All planned M mutations: NOT RUN. Every behavior requirement and invariant: NOT YET VERIFIED. The only current PASS entries above are discovery observations, not issue completion.


## Native goal established — 2026-09-11T19:22:46.270986+00:00

Native identity: `01a091ec-193f-7a91-b8e3-b28e757eff19` (threadId); status read back `active`; tokenBudget null (not set). Initial get returned null; set/get agree on all 44 IDs, concise guarantees and exact contract/evidence pointers. Exact objective/results: [artifacts/native-goal.json](artifacts/native-goal.json). A prior overlong objective was rejected by the native 4000-character limit; [failed attempt](artifacts/native-goal-rejected-attempt.json) remains recorded. No guarantee was dropped. No implementation has begun; unrelated goals remain untouched.


## Baseline runs completed

B001: `make -j4 test` **FAILED**, exit 2, inherited `TNY_TOOLS=terminal`. B002: full Darwin `make quality` **PASSED**, exit 0; it explicitly omits Linux GCC analysis. B003: `make -j4 leaks` **FAILED**, exit 2; no passing aggregate is claimed from individual zero-leak reports. Full original logs and revision/status records are retained in artifacts/baseline-*.

A second pristine detached worktree at `/Users/tomas/projects/tny-open-issues-baseline-2026-09-11` now runs separately recorded normalized tests/leaks with `env -u TNY_TOOLS`; this isolates the baseline from any eventual implementation writes. Results are pending, not assumed to pass.


## Current state after A1 — 2026-09-11T19:45:33.084410+00:00

39 requirements / 49 invariants remain active. All product source unchanged. B004 normalized full test and B005 normalized Darwin leaks **PASS**, exit 0; [exact runs](artifacts/baseline-normalized-runs.json), [test log](artifacts/baseline-normalized-test.log), [leak log](artifacts/baseline-normalized-leaks.log). The preceding pending-baseline paragraph is historical and superseded by these results. This does not turn B001/B003 failures into passes or prove Linux leak coverage.

[Review dispositions](review-dispositions.md), [A1 amendment](artifacts/amendment-1.md), [active requirements](artifacts/requirements.active.json), [active invariants](artifacts/invariants.active.json). A fresh design review remains a required preimplementation gate.


## First implementation gate passed — 2026-09-11T19:50:43.292804+00:00

Fresh reviewer `a0cbdcbc-29b1-493e-b793-95e607ba0b3d` approved the amended first #123 slice with no blocking design findings. The primary reverified the reviewer input hashes, all 1,068 baseline tracked inputs, initial snapshot hash, completed normalized baseline checks and the active native goal with all 49 IDs. [Guard record](artifacts/preimplementation-gate.json) and execution chain precede the first product write (proposed ADR) and bounded implementation session `41eaddaa-3242-4e37-b3de-4e09353d4e4d`. This is not behavioral verification; the required different fresh first-slice review is pending.


## Independent acceptance and Linux baseline — 2026-09-11T20:10:42.489839+00:00

The primary-authored localhost/process/state C123 oracle ran 13 cases against the unchanged baseline binary: 12 FAIL, 1 PASS. This is reproduction against NEW requested criteria, not a failure of the original full suite. [Harness](artifacts/acceptance/subagent_acceptance.py), [run record](artifacts/acceptance/subagent-before.result.json), [full log](artifacts/acceptance/subagent-before.log). No new implementation pass is claimed.

Task-only Linux image built successfully on retry (the first CLI-flag failure remains preserved). A private Linux aarch64 container now executes the same baseline revision from a git bundle, with read-only host inputs, no real provider credentials and an explicit Docker context. The user default context remains unchanged. Native Linux test/valgrind results are pending, not assumed.


## First-slice independent behavior check

The primary built an isolated snapshot from the baseline plus current subagent changes and ran an independently authored local HTTP/process/state oracle. The initial run passed 12/13; the remaining assertion incorrectly disallowed the contract's explicit `stale` synonym. [Oracle correction](artifacts/acceptance/subagent-oracle-correction.json) preserves the original failing result and source; no product code or contract criterion was changed. The corrected oracle requires `stale` or `interrupted`, `running:false`, and `exit_code:null`. It still fails 12/13 baseline cases and **passes all 13** against the isolated first-slice binary. [Exact runs](artifacts/acceptance/subagent-v2.runs.json), [source snapshot manifest](artifacts/first-slice/snapshot.json).

A DIFFERENT fresh read-only reviewer `d15820da-1861-4b3f-af32-a01b979dfea4` is inspecting the frozen raw code/tests and a scope-only contract excerpt without prior verdicts or worker conclusions. Pattern reuse is still blocked until review findings are resolved. The live worker may continue #123 tests/docs; snapshot results do not automatically prove a later changed tree.


## Resume — 2026-09-11T22:28:32.491591+00:00

Overall remains **INCOMPLETE**. Native goal active and original initial contract/ADR bytes verified. [Resume manifest and native read](artifacts/resume-20260911/discovery.json); [complete GitHub reconciliation](artifacts/resume-20260911/github-reconciliation.json). Recovered worker code is present; earlier ledger NOT STARTED for #123 is superseded by IMPLEMENTED / CURRENT VERIFICATION PENDING, not verified completion. The previous independent reviewer approved the first pattern with two findings; current-state re-review follows their correction. Linux baseline retry and Valgrind passed; the preceding failed Linux test remains preserved. No final-tree platform pass is inferred.


### Corrected #123 slice

R002 demonstrates the actual premature wind-down failure. R004 demonstrates the same marker oracle passing after correction. R005/R006 pass real nested CLI/loopback lifecycle and diagnostic fixtures; R006 also records expected socket-reset noise during cancellation. R007 passes all 471 unit tests / 11,745 assertions, zero skipped, on its recorded current source manifest. Commands, environment, timestamps, exit status and complete logs are under artifacts/resume-20260911/R00*.json and .log. These are macOS observations, not full-suite, hosted-CI or cross-platform proof.

Different fresh independent corrected-state review d4040c2c-78eb-4a89-b8c3-ed083c126b72 found no defects and approved reuse. Every inspected runtime/test hash was rechecked at the checkpoint. The global result remains INCOMPLETE; native goal remains active.


### Mutation harness freshness correction

The first controlled #123 fault run is **INVALIDATED**, not a six-kill/two-survivor proof. All eight intended replacements matched once, but GNU make 3.81 sometimes rebuilt the object without relinking an executable in the same timestamp second. M123.2a/M123.5 build logs expose missing links; other kills could also target the preceding mutant. No product defect or test weakness is inferred from those counts. Raw results and the exact original runner bytes remain preserved; see artifacts/resume-20260911/controlled-123-first.disposition.json. The corrected runner removes both the disposable object and executable before each build and records object/binary hashes. All eight originally planned faults will rerun unchanged, with unmodified and restored passes required. No product test or original criterion was weakened.


### Stronger exit-status regression: Linux failure and correction

The #123 Linux frozen-state runs L004/L005 passed both real nested-child fixture suites. L003 unit and L006 Valgrind exited 2: the newly added complete-success-JSON/process-exit-7 regression reused the `script` buffer later expected to contain a process-success reply, causing the separate wrong-session-ID test to observe CHILD_FAILED instead of INVALID_RESPONSE. The production outcome classification was correct. A separate `failed_script` buffer fixes the test arrangement without weakening either oracle. Those two failed runs and their original frozen source/archive remain preserved; they are not passing unit/leak results. New source-bound runs are required.

Mac execution access temporarily failed even for pwd/git status, then recovered. A fresh read of the same native goal returned active and the immutable contract SHA matched; see artifacts/resume-20260911/native-goal-access-recovery.json. No goal redirection or completion occurred.


### Capability prerequisite underway

A4 and proposed ADR0089 were recorded before capability implementation. Qualified independent approval 77a85d3c-ac1c-4317-bed4-adefab0c9d73 was resolved through the exact source-order check, existing ACP parser reuse, explicitly unverified wording and preserved pending-queue refusal. Worker 3f51a198-ae9c-46d1-9925-9ae61a5d3172 operates only in /Users/tomas/projects/tny-open-issues-capabilities-2026-09-11, references the canonical contract/goal and has bounded ownership with integration bases recorded in artifacts/worker-image-capabilities. No capability runtime pass or full #126 completion is asserted yet.


### Portable launch diagnostics: current bounded proof

[L203 unit](artifacts/resume-20260911/L203-unit.json), [L204 nested subagent flow](artifacts/resume-20260911/L204-subagent-integration.json), [L205 diagnostics](artifacts/resume-20260911/L205-subagent-diagnostics.json) and [L206 full Linux Valgrind](artifacts/resume-20260911/L206-valgrind.json) exit 0 on the A5 frozen subagent source. [R008 macOS leaks](artifacts/resume-20260911/R008-macos-launch-leaks.json) also exits 0, with the existing platform suite exclusions explicitly retained; Linux Valgrind covers its whole unit binary. This is not final integrated proof for all six issues.

[M123.7 raw result and plan](artifacts/resume-20260911/controlled-123-preflight-linux/result.json) demonstrate the planned missing-preflight fault is compiled, behaviorally killed and restored; the source copy is unchanged. Fresh reviewer 76099816-b17c-4f7c-a2c4-2c3777fd8cec approved the process seam, with its header clarification now resolved. The earlier L106 failure remains preserved.

R009 did not execute any mutations: a task-driver parent-directory calculation used docs/ instead of the repository root, and Python exited 2 for a missing runner path. It is an unrun fault set, not a failed product or successful mutation run. Its log remains preserved; a corrected explicit-root retry is required.


### Header trust review and independent probes

The first #122 code review found arithmetic-lossless JPEG misclassification and identified failure-metadata asymmetry. The primary has not accepted parser/strict-size reuse: original caller-parity criteria still apply. The compiled read-only header probe returned dimensions for six unsupported/corrupt header cases; its two VP8X reserved-bit expectations were independently corrected after reading the mandatory reader-ignore rule. The entire exploratory plan/results remain preserved at artifacts/resume-20260911/header-probe-before. A6 records additive guard/regression obligations without reclassifying the wrong exploratory oracles as product failures.

R010's corrected all-eight current-subagent fault set passes on the A5 frozen source: artifacts/resume-20260911/controlled-123-portable-launch-retry/result.json. Each original passes, each compiled fault is caught behaviorally, original bytes/build/tests are restored, and the source copy is unchanged. Combined with separately recorded M123.7, this is bounded #123 fault proof, not the still-unrun whole issue-set mutation gate.


### Capability integration and remaining review

The capability worker's owned changes were integrated using preserved before-images and git three-way merge, with every source/destination hash rechecked before writing. Only independent fixture comments and two wasm invocations conflicted; both were retained. Record: artifacts/resume-20260911/capability-integration/applied.json. The primary completed A4's requested native preamble using the existing policy-label accessor and added pointers to the actual settings/tool documentation paths. Neither old #123 nor new #122 edits were overwritten. No capability code review or final integrated acceptance is claimed yet.

A6 parser tests initially failed because a newly written JPEG test modified the segment length at byte23 rather than precision at byte25. That incorrect test arrangement was corrected; R012 remains failed in the record. R014 passes both focused header unit tests, and the independently compiled corrected corpus passes all12 cases (header-probe-after/result.json), including mandatory acceptance of ignored VP8X reserved fields. Full integrated checks/mutations/platforms still require reruns.


### Reviewed combination and container regressions

R019 reproduces read_image incorrectly advertised in the fully configured raw-schema branch. R020 selected a nonexistent test name and ran zero tests: its zero exit is **NOT A PASS**, superseded by R021's correctly selected failing partial-chunk regression. Both original logs remain. The product fixes add the missing schema-filter predicate and use size_t/subtraction RIFF bounds with a distinct incomplete-tail status. R022/R023 below are required after those changes; no older pass is treated as current evidence.

The Nix baseline passes at986,072B while the frozen integrated package fails at1,051,608B against the same1,048,576B gate. This65,536B increase and3,032B over-budget result is an introduced blocker, not a baseline failure. Frozen Linux full-test P03 timed out in a dictation fixture; P04 quality failed due missing shellcheck/shfmt and an archive lacking Git inventory, so even its empty C formatting invocation cannot count as proof. P05Valgrind, P06/P07image fixtures, P08wasm builds, P09–P12actual wasm image/capability/unsupported-context fixtures and P14input-integrity passed on that explicitly frozen pre-A8 input. Missing/pending full/platform gates remain.


## Resume 2026-09-12T07:26:44.217109+00:00

Read the same active native goal and all canonical amendments/evidence. Initial contract and every baseline ADR unchanged; original checkout clean. [Current input state](artifacts/resume-20260912/resume-state.json). Both late workers finished; their results are review inputs, not accepted completion. Hash-chain verification recognizes the original compact JSON and later default JSON encodings; all links verify and no old row was edited. Final ADR collision decision remains pending user authorization; independent implementation continues.


## Resume correction results 2026-09-12T07:46:15.554189+00:00

N001 full macOS suite, N002 full macOS leaks and N003 quality all pass against unchanged restored inputs. No standing leak exemption is accepted. N004 corrected strict unit480tests and N006 Dimensions18tests pass; N005 ABI tests/inventories pass with only unrelated mutation-runner source added concurrently (raw manifests retained). N007 clean native CLI and separate dictation-fixture build uses actual -flto and passes size-check at903,184bytes on this Mac. This is a native build/size result, not runtime or Linux/Windows final proof. Original default Nix size failure remains until new exact-state check.


### Deterministic late-cancel fixture setup correction

N008 failed before running the behavioral test because `make lib-shared` also requests a frozen ABI0 compatibility source unavailable in a deliberately source-only scratch directory. The fixture needs only the actual current toolkit library, so its build now uses the established `lib-shared-active` target. The full separate `make test-abi` gate still includes and passes the ABI0 baseline; no compatibility requirement is removed. N008 and its raw setup failure remain visible; N009 retries the same original/fault/restored cancellation oracles.


N009 reached the new late-cancel fixture but captured the concurrently edited, unfinished manifest service and failed its compile. No cancellation oracle ran and no compiler failure is a mutation kill. N010 instead uses an explicit reconstructed frozen pre-manifest source: original frozen files plus SHA-verified manifest-worker before-images and the new test files. This establishes the missing strict-finalization boundary on the reviewed completed slice without racing active implementation; final integrated code must still rerun the test. The failed N009 snapshot and logs remain preserved.


### Real wasm/browser environment setup failures retained

The first corrected-event Node run omitted the fixture's explicit TNY_TEST_EXPECT_WASM=1 selector, so it attempted an impossible native MCP subprocess and failed; this does not establish a wasm code defect or a pass. A selected image-input fixture was absent from the event-only frozen snapshot and returned missing-file2; capability was verified in a different recorded snapshot, not implicitly in this one. Browser dependency installation reached the actual Playwright installer but failed because the task container's default user cannot run apt through su. The retry sets the correct wasm fixture selector and installs only task-container dependencies as root, then browser payloads as the original task user. No host package or credential change. All first-run logs remain.


## Continuation after restored execution — 2026-09-12T09:36:54.649182+00:00

The same native goal is active. Read all amendments through A12 and latest evidence; initial SHA and all baseline ADR names/bytes still match, original checkout is clean. Resume manifest and complete current GitHub reconciliation: artifacts/finish-20260912/resume.json and github-reconciliation.json. Completed manifest/event workers and ongoing isolated job/export workers were inspected; no old result is assumed current merely because it says complete. ADR prefix resolution remains the user decision requested in this continuation; no waiver or rename has been applied.


### Continuation record constructor correction

The integration guard stopped before product writes because continuation row36 supplied the reserved record_sha256 key as a payload digest. The helper hashed that preimage and then replaced the field. Original pretiming rows1–35 are unaffected and unchanged; row36 is preserved, with its exact independently verified preimage and resume-file digest at artifacts/finish-20260912/resume-chain-record36-preimage.json. The recorder now rejects reserved payload keys and explicitly validates this one recorded preimage instead of claiming it uses the normal encoding. The attempted event-merge check driver started against the still-unmerged tree; its results are source-bound preintegration observations, not event completion. No historical row or initial contract was rewritten.


### Reviewed event integration — 2026-09-12T09:48:11.754958+00:00

The corrected event slice is now merged into the canonical integration tree with before-image hashes. Capability and manifest changes are retained. Reviewed production files still match review f2f228a4-ab71-4268-a5e0-960ac898b84c. A later terminal-test-only delta adds post-verdict cleanup and direct child PID tracking; it is preserved and requires current execution. The two unit-test blocks and both wasm fixtures are retained. Plan/applied records: artifacts/finish-20260912/event-integration. Earlier F001/F004 passed against the preintegration input; F002/F003 failed because their test files were not yet integrated, never counted as passes.


### Job component paused and resumed at independent review checkpoint

Review b3784906-e36c-4a0d-b2a1-724583a1a6ee found a stale retry transaction and wrong-kind credential carriers. The original worker PID was verified by command/cwd, its content manifest saved at artifacts/finish-20260912/jobs-review-checkpoint-before.json, and only that task-owned worker received SIGINT. It exited; no completion or passing unfinished run is inferred. A fresh bounded implementer resumes the preserved code under A14; all original before-images remain available for integration.
