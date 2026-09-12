# Independent design review dispositions

Both original review results remain preserved verbatim in artifacts. This record is implementation coordination, NOT input for subsequent independent reviewers. Neither review approved implementation.

Reviewer A: d6ba1ccf-01e7-48e7-a996-ed2e9aabe0c4; Reviewer B: 49d8b7d9-6ebd-40ff-b15e-e6a02866e972. Both used read-only tools, no children and no commands. Source/content manifests and original prompts are in their .run.json/.prompt.txt artifacts.

Finding | Disposition / evidence | Gate
--- | --- | ---
A-F1 | Completed original baseline plus separate pristine normalized test/leak runs, both normalized exit 0, clean source; A1 requires completion guard | Resolved pending re-review
A-F2 | Current capability absence confirmed with git grep/backend.h; R126.7 and explicit prerequisite added | Acceptance strengthened; behavior NOT IMPLEMENTED
A-F3 / B-F2,F5 | A1 selects create-without-id, rejects named IDs before launch, preserves generated IDs, defines real lifecycle and corrects stale relationship/configure/queue docs | Resolved design pending re-review; code NOT IMPLEMENTED
A-F4 / B-F1 | A1 defines exact SUBAGENT_* prefix, classes, location and mutation M123.4 | Resolved design pending re-review
A-F5 | A1 explicit all profile, resolved credential/flag/provider/context matrix | Resolved design pending re-review
A-F6 / B-F7 | No raw failure-body echo; sentinel diagnostics/session/event/argv tests and M123.5 required | Pending implementation/verification
A-F7 | Added five scope/prerequisite requirements and active inventories; no original rows dropped | Resolved mapping pending re-review
A-F8 | SDK/ABI and shared wasm checks explicitly added | Pending implementation/platform verification
A-F9 | A1 exact dimensions/status/strict retention/error behavior and primary-source requirement defined | Pending implementation/protocol verification
A-F10 | A1 complete dependency DAG and cross-slice pending gates | Resolved design pending re-review
A-F11 | ADR/process reuse, queued/PID/wait/retry cases, M124.6 | Pending implementation/verification
A-F12 | IM7 contract, unsafe syntax/aliasing/dependency/permission tests, M125.3/.4 and M-G2 | Pending implementation/verification
A-F13 | Do not change ADR README. New ADR files plus feature/evidence links satisfy AGENTS.md recording obligation without mutating any baseline hash. No authority exists requiring index edits; reviewed alternative removes proposed conflict, not a user guarantee | Resolved by preservation alternative; fresh review to challenge
A-F14 / B-F3 | Global 0030/0045 duplicate conflict retained. User asked whether ONLY existing collisions may be exempted; no answer/authorization received | BLOCKED final gate
A-F15 | Current evidence summary refreshed; historical records retained and marked superseded; native API records linked | Resolved documentation
A-F16 | A1 adds all suggested critical mutation families, preserves initial set | Pending execution, never counted as kills

No preimplementation approval is inferred from this disposition record. A fresh amended-contract review is required.


## Corrected #123 state — fresh review d4040c2c-78eb-4a89-b8c3-ed083c126b72

Read-only separate Claude session, no child-agent or command/write tool. Original issue and prescriptive contract excerpt plus a frozen raw-code/test copy; prior findings/verdicts/evidence were excluded. Question: do launch/cancel/error/state boundaries meet ownership, no-reentrancy and no-secret guarantees, and do the tests exercise consequential cases? Result: explicit no defects and pattern reuse approved. All inspected runtime/test file hashes still matched at disposition. Raw prompt, identity, snapshot and response: artifacts/resume-20260911/review-123-corrected.*.

Earlier first-slice findings: the 3-second launch grace was corrected using the child's shared 5-second deadline plus 1-second slack; the new marker regression fails before and passes after (R002/R004). The readlink truncation guard now rejects a full buffer with ENAMETOOLONG; its Linux runtime coverage remains pending. The reviewer read tests but did not execute them; its language about tests being proof is not itself credited as execution evidence. Current observed execution is recorded separately in R003–R007, including 471 passing unit tests. The overarching ADR-prefix blocker and remaining platforms/mutations are unchanged.


## Image-input prerequisite design

Original e574e4c7-c4bd-4740-9ccb-0e8da9230b69: not approved. Its auth-shadowing, shared-gate and actual-parser findings led to a distinct top-level provider map, exact engine/queue/flush/tool/CLI seams and actual-parser tests. Fresh reviewer 77a85d3c-ac1c-4317-bed4-adefab0c9d73: qualified approval of that revised design, original requirements and raw source only, no previous verdicts. All four qualifications resolved in A4 before implementation: reuse acp_provider_name; source confirms cli_make_ctx resolves before cmd_ask/session creation; true label is configured, unverified; refused flush preserves pending images/count. No runtime proof is inferred from approval. Prior reviewer inputs/results remain preserved.


## Portable launcher refinement — 76099816-b17c-4f7c-a2c4-2c3777fd8cec

Fresh read-only separate session, original #123 and A5 normative guarantees, frozen raw process/subagent source plus outcome test excerpts. No prior verdicts or execution conclusions. Result: no correctness/safety finding, safe reuse; the one minor header-documentation request was resolved by explicitly naming preflight errno origins and the non-authorizing/non-atomic limitation in process.h. Runtime files and full outcome tests still matched the reviewed frozen manifest at disposition. Raw identity/input hashes/question/result: artifacts/resume-20260911/review-launch-portability.*.

Execution is separate evidence: L203 unit, L204/L205 real subagent fixtures, L206 full Linux Valgrind and R008 macOS leaks pass against subagent-portable-launch.manifest.json, not the moving image/capability tree. Linux controlled fault M123.7 has original pass, compiled removed-preflight failure at the intended LAUNCH_FAILED-vs-CHILD_FAILED assertion, restored pass and unchanged source; no compiler/leak failure was counted as a kill.


## Structured strict-failure detail design — returned reviewer 1dde9053-32bb-4b8d-b236-b5504b013e1c

Fresh read-only review of original #122, exact proposal and raw caller/SDK/ABI/test sources; no prior verdicts or run conclusions. Conditional approval: (1) existing errors-have-no-result docs/strict ABI test explicitly become a narrow local-detail exception; (2) non-JSON interception must clear the populated buffer; (3) typed error prefix must carry rather than discard JSON; (4) NAPI failure path reads before destroy with distinct failure type; (5) Python result copied before raise/destroy, with actual _binding.py inspected by primary; (6) name is image_detail/imageDetail, not the successful ImageResult type. All are mandatory in A9 before implementation. Returned session ID differs from requested launch ID, both preserved without inventing identity linkage. Generic diagnostics and frozen ABI remain unchanged; actual-code review is still pending.


## Restored strict-image result review — 798913bc-ab5f-4666-b788-b2e5cd05012c

Read-only fresh reviewer, original A9 guarantees and frozen source/tests; no implementer verdicts. Actual code/handle/privacy boundary approved with two low corrections. The primary now stores an exact locally derived image_failure_status instead of deriving strict status from diagnostic text, and wipes partial detail bytes before resetting their length on OOM. N004 unit480/480 and N006 Dimensions18/18 pass current unchanged inputs; N005 ABI30tests and all baseline comparisons pass. N005's broad manifest differs only by the independent new tests/mutation/open_issues_20260911_image.py file, not by any compiled/runtime/test/ABI dependency; this concurrency is explicitly retained in its before/after manifests. No final whole-tree acceptance inferred.

Reviewer concern about absent typed-tool/interception coverage is resolved by the actual test_image_workflow.py cases (typed error-prefix JSON, interception --json/plain output, safe fields and old-file preservation) executed by N001/N006, not by relaxing scope. Deterministic late concurrent cancellation and remaining SDK validator mutants remain active verification tasks, not satisfied by review. The reviewed image service/strict-failure pattern may now be used by the next originally ordered #127 manifest slice; that new persistence boundary still needs independent actual-code review before exports/jobs.


## Corrected canonical events and durable-job design checkpoint

Event reviewer f2f228a4-ab71-4268-a5e0-960ac898b84c: explicit approval/no material findings on corrected source. Current hashes match; N011/N012/N013 establish actual corresponding native behavior. Fourteen controlled event faults are separately accounted with successful original/restored checks. Actual wasm stdout readiness/backpressure and browser execution remain required; static review does not cover them.

Jobs design reviewer263228c1-cea1-4d6d-a847-c62cbfe1eae3 found one unresolved textual lock-mode issue; A11 explicitly requires every other-owner probe LOCK_EX|LOCK_NB, deny on contention, and projects both queued/running abandoned jobs interrupted. All other descriptor/ack authority/cancellation sites/retry-reset/log-bound mechanisms matched the reviewed source. These precision corrections are binding before implementation; no final artifact gate or cleanup guarantee was reduced.


## Preview design discovery correction

Reviewer a3e66201-4366-4313-aafe-e70e10366d12 inferred two attachment queues because the backend bridge body was not included. Actual openai.c:2410–2416 delegates straight to tools_queue_image(&o->env), and the same env is flushed before the next provider request; that finding is disproved by source, not ignored. Its meaningful stricter-auto gate/wire-status/ACL/WASM-surface/origin findings are incorporated in preview-design-v2.md. The primary additionally identified the existing validate-then-reopen pathname issue for two generated previews reusing one output in a tool batch; V2 captures the exact loaded bytes in the existing queue and shares the existing image_url builder. No preview code or final gate is claimed by this design correction.


### A13 design review resolutions

The final owned-plan design review f8262745-c272-4b43-9420-7dbf4fac2d4b did not approve its earlier borrowed-settings wording. A13 resolves F1 with deep-owned settings, F3 with mandatory nonnull permission-gated prepared execution, and F4 with explicit fail-closed effective-provider grant semantics. F2's const-pointee objection is contradicted by an actual C11 compile+execution probe, preserved verbatim. The one-time-grant premise and explicit retained IO-before-late-cancel order were independently verified sound in that review. This records concrete dispositions, not a claim that the prior text was approved; actual-code review and behavioral proof remain separate pending gates.


### Preview design review resolutions for A15 first slice

The concrete one-queue/loaded-byte/control bridge was independently verified in b2f646ac-4a4e-4e3d-8c41-f3ae704b0b9e. Atomic design reviewer28f21ad3-ff47-4042-9717-6d1e52a323f9 identified D1turn-active-not-batch-active, D2undisposed fatal retained queues, D3manual-only compatibility. A15 pins admission to an owning continuable batch, separates refusal (preserve) from recorded-terminal cleanup (free), and adds a distinct preview-fatal outcome while keeping legacy manual-only behavior. The pure helper is explicit for native/wasm and new control fields optional. These are corrected requirements before first-slice code, not a claim the earlier proposal was approved. Actual-code review and all full #126 integrations remain pending.
