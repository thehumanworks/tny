# Issue snapshot

Base: `89bcd5918da0e225e1806813a206d007daafac0a`.

## #153: Swarm 1/6: asynchronous team control and one-command lead/worker launch

State at retrieval: OPEN.

Parent: #152. Priority: P1. Size: L. Baseline: `cea97b372e6671acd5bca88341c10d85d34486d6`.

## Evidence and impact
- [`subagent.c:180–235`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/subagent.c#L180) waits for one child to finish; [`tools.c:175–181`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/tools.c#L175) exposes only create/message/inspect/lifecycle.
- Async batch APIs already exist at [`tools.c:182–216`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/tools.c#L182). Shell profiles already guide agents to `ask -B` and `session --wait`.
- [`tui_agents.c:17–37`](https://github.com/thehumanworks/tny/blob/cea97b3/src/tui/tui_agents.c#L17) displays a flat workspace session list, not a lead/worker task tree.

Users can construct a swarm manually, but must own ID bookkeeping, result collection, provider selection, integration and failure handling. The missing abstraction is a small shared control layer, not the ability to start processes.

## Tasks
- [ ] Write an ADR for run/task/attempt/parent-session identity and explicit lead/worker roles. Specify capability differences across native, shell profiles, host backends, SDK, SSH and wasm.
- [ ] Add an opt-in team-control service over existing sessions/jobs. Spawn returns a handle immediately; status/collect/wait-any/cancel expose canonical terminal outcomes and provenance. Keep synchronous subagent behavior compatible rather than silently changing it.
- [ ] Expose a discoverable CLI entry point and equivalent agent operations. Supply a minimal lead + two read-only workers example and a review/implement/verify template. Permit explicitly selected worker provider/model/effort without widening permissions or persisting credentials.
- [ ] Record parent-child membership and show a run-filtered task tree in `agents --json` and the dashboard. Do not infer team membership from possession of an arbitrary session ID.
- [ ] Deliver completion/failure notifications at existing safe engine boundaries so the lead need not spend model turns polling. Preserve a bounded collection/wait fallback for non-interactive clients.
- [ ] Separate execution success from accepted task completion. Record configured verification commands/results or an explicit unverified state; do not parse worker prose as authority to execute checks.

## Acceptance
- Mock providers prove overlapping workers and a responsive lead, correct ID/result correlation, one-worker failure, and cancellation that leaves unrelated sessions running.
- A terminal-profile lead can use the same control service as the all-tools profile. Retain the documented launch-only meaning of background exit status.
- Identity survives client exit; this issue defines records for the durable-DAG follow-up rather than inventing a second recovery scheme.
- Unsupported execution contexts fail clearly before side effects; document wasm behavior. Host backends retain ownership of their loops.
- Add focused tests and documentation, run relevant integration/ownership tests plus `make test`, `make quality`, `make leaks`, size checks; update Nix inputs for any new fixture/target/tool.

Dependencies: coordinate record schema with Swarm 2/6; messaging, safe worktrees and shared admission build on these identities. Reuse #124 and ADR0093 rather than rebuilding durable job execution.


## #155: Swarm 2/6: durable workflow DAGs and resume of verified successes

State at retrieval: OPEN.

Parent: #152. Coordinate run/task identities with #153. Priority: P1. Size: L. Baseline: `cea97b372e6671acd5bca88341c10d85d34486d6`.

## Evidence and impact
- [`shell/tny-workflows.sh:836–844`](https://github.com/thehumanworks/tny/blob/cea97b3/shell/tny-workflows.sh#L836) deletes the previous run directory and resets every task to pending.
- [`workflow.py:505–508`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/python/src/tny/workflow.py#L505) creates a fresh in-memory task map/semaphore on each run.
- [`docs/workflows.md:447–457`](https://github.com/thehumanworks/tny/blob/cea97b3/docs/workflows.md#L447) explicitly documents no retry/cache/cross-run resume. This is an enhancement, not a violation of the existing contract.
- Durable jobs already preserve attempt records and verify carried output hashes: [`jobs.cpp:2030–2079`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/jobs.cpp#L2030). Supervisor loss and uncertain cleanup deliberately fail closed.

A multi-stage run cannot currently reuse job retry integrity to resume its dependency graph. Restarting the script can repeat successful paid work and side effects.

## Tasks
- [ ] Add an opt-in durable workflow record: versioned DAG definition, stable run/task/attempt IDs, task configuration fingerprint, dependency/result hashes, execution handles and explicit states.
- [ ] Reuse existing job/runner ownership and atomic persistence. Preserve successful outputs and attempt history; record a task claim before starting it. Keep existing ephemeral workflows compatible.
- [ ] Expose inspect/resume/retry-failed through shared control and a documented shell path. Define SDK support explicitly; do not silently rerun an in-memory DAG under a different persistence model.
- [ ] Reconcile live handles before resuming. Reuse a completed task only if its definition, dependencies, result evidence and relevant workspace revision remain valid; invalidate affected descendants when inputs change.
- [ ] Distinguish known-not-started, completed, failed and uncertain-in-flight work. Require explicit resolution for uncertain external side effects or cleanup holds. Do not promise exactly-once provider/tool execution.
- [ ] Keep accepted verification evidence distinct from integrity hashes: a hash verifies the recorded artifact, not code correctness.

## Acceptance
- A barrier-based fixture completes A, interrupts B, and leaves C dependent on B. After client/supervisor loss, resume preserves A without another provider request and handles B according to its recorded certainty; C runs only after B succeeds.
- Tests cover repeated resume, concurrent resume, changed prompt/dependency, corrupted artifacts, failed descendants, permission ceilings and unknown cleanup.
- Existing job retry/cancel/reservation tests and shell/Python/TypeScript workflow semantics remain green.
- Update docs and add an ADR; run `make test`, `make quality`, `make leaks`, relevant mutation/ownership checks and size checks. Update Nix fixture/target lists when needed. State native/wasm/SSH support without adding a daemon.


## #156: Swarm 3/6: durable asynchronous agent mailboxes and safe delivery

State at retrieval: OPEN.

Parent: #152. Depends on the identities/control contract in #153. Priority: P1. Size: L. Baseline: `cea97b372e6671acd5bca88341c10d85d34486d6`.

## Evidence and impact
[`subagent.c:61–64`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/subagent.c#L61) explicitly rejects queued-message/relationship actions. [`subagent.c:450–454`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/subagent.c#L450) rejects a message while the child runs. [`docs/features/mcp-and-skills.md:343–350`](https://github.com/thehumanworks/tny/blob/cea97b3/docs/features/mcp-and-skills.md#L343) defines `message` as a synchronous follow-up. CLI steer interrupts/takes over a turn; it is not a nondestructive collaboration inbox.

Parallel agents cannot exchange a clarification, discovered constraint or partial result through a first-class durable queue while they work.

## Tasks
- [ ] Specify an ADR for addressed lead-to-worker/worker-to-lead messages and opt-in peer messages. Include stable message ID, run/sender/recipient identity, sequence, payload bound, delivery/ack state and retention.
- [ ] Persist before acknowledging send. Add inbox/read/ack and asynchronous send operations through the same CLI/agent service. Preserve the existing synchronous `subagent.message` contract.
- [ ] Deliver at quiescent native engine boundaries. Do not re-enter an active backend or interrupt an in-flight tool by default. Distinguish queued, delivered and acknowledged; document long-running-tool delays.
- [ ] Validate run membership and permission ceilings. Treat message text as untrusted collaboration context, not system policy or approval. Keep secrets out of transport metadata/logs.
- [ ] Define crash replay and deduplication. Use explicit at-least-once delivery with consumer deduplication where appropriate; do not claim exactly-once reasoning or external effects.
- [ ] Bound inbox size and wakeups, report backpressure, and handle completed/cancelled recipients. Keep steering/cancellation a separate explicit operation. Host backends without a safe injection boundary get a documented queued-next-turn or unsupported result, never an approximation.

## Acceptance
- Send while a fixture worker is busy; sender returns immediately, the active tool is not replayed/interrupted, and the message appears at the next documented safe boundary.
- Kill sender/recipient around persistence and acknowledgment barriers; recovered delivery is neither lost nor invisibly duplicated in context.
- Test ordering, duplicate IDs, oversized payloads, full inboxes, membership rejection, cancelled runs, and unrelated-session isolation.
- Multi-agent integration fixture proves a worker clarification reaches the lead and the reply changes only the intended task.
- Reuse canonical events, retain the single-event-loop/public-C-ABI boundaries, document wasm/SSH behavior and update Nix inputs. Run focused tests plus `make test`, `make quality`, `make leaks` and size checks.


## #157: Swarm 4/6: managed worker worktrees and explicit result integration

State at retrieval: OPEN.

Parent: #152. Depends on task/run identities in #153. Priority: P1 for editing swarms. Size: M/L. Baseline: `cea97b372e6671acd5bca88341c10d85d34486d6`.

## Evidence and impact
- Native children inherit `ctx.cwd`: [`subagent_plan.cpp:68–72`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/subagent_plan.cpp#L68); the API accepts only action/id/prompt: [`subagent.c:105–113`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/subagent.c#L105).
- [`docs/workflows.md:454–455`](https://github.com/thehumanworks/tny/blob/cea97b3/docs/workflows.md#L454) warns parallel agents may conflict and recommends caller-created worktrees/task cwd.
- Generic `--worktree` and task `--cwd` already exist. The gap is managed per-task ownership and integration, not missing Git support.

Process isolation does not isolate working files. An easy swarm entry point must not make concurrent editing of the same checkout the accidental default.

## Tasks
- [ ] Add a task workspace policy: shared read-only, isolated Git worktree, or explicit shared writable opt-in. Explain that worktrees are not security sandboxes; use existing permission/tool policy for enforcement.
- [ ] Reuse current worktree helpers to create/own each editing task's branch/path. Record base commit, run/task ID, status and final revision/diff. Never adopt or delete an unrelated user's worktree.
- [ ] Preserve dirty/untracked starting work. Require an explicit snapshot/base choice or clear refusal rather than silently excluding user changes. Define non-Git, SSH and wasm behavior.
- [ ] Hand the lead structured patch/commit artifacts and validation results. Add explicit integration/check/conflict steps; do not automatically merge on child exit zero or worker prose.
- [ ] On cancel/failure, preserve unsaved diffs and offer inspect/keep/remove only with proven ownership. Cleanup must be idempotent and safe after crashes.
- [ ] Add a documented read-only fan-out -> isolated implementations -> integration -> tests example.

## Acceptance
- Two mock editing workers modify the same relative filename in separate worktrees; neither changes the launch checkout. Integration reports a real conflict without discarding either diff.
- Tests cover dirty/untracked launch trees, existing branches/path collisions, cancellation, process loss, repeated cleanup, unrelated worktrees and task permission ceilings.
- The run reports patch provenance and verification outcomes. Failure to merge or verify prevents an accepted-completion state.
- Preserve existing `--worktree` behavior, run relevant worktree/job/session integrations plus `make test`, `make quality`, `make leaks`, and size checks. Update docs/ADR and Nix test inputs for new fixtures.


## #158: Swarm 5/6: shared admission limits and run-level usage budgets

State at retrieval: OPEN.

Parent: #152. Coordinate identity/account scope with #153. Priority: P1. Size: L. Baseline: `cea97b372e6671acd5bca88341c10d85d34486d6`.

## Evidence and impact
- Each supervisor reads its own concurrency and allocates local slots: [`jobs.cpp:3438–3446`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/jobs.cpp#L3438). Admission checks only its local active count at [3574–3583](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/jobs.cpp#L3574).
- Python also has a per-workflow semaphore: [`workflow.py:507,526–528`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/python/src/tny/workflow.py#L507).
- TypeScript's `ask` returns usage at [`index.mjs:342–352`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/typescript/dist/index.mjs#L342), but `NativeWorkflowRunner` omits it from the task result at [904–909](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/typescript/dist/index.mjs#L904). Python collects text/errors/stop reason but no task usage at [`workflow.py:304–343`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/python/src/tny/workflow.py#L304).
- `max_steps` defaults to unlimited in [`config.c:599`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/config.c#L599); native child argv at [`subagent_plan.cpp:68–102`](https://github.com/thehumanworks/tny/blob/cea97b3/src/core/subagent_plan.cpp#L68) does not forward an explicit parent `--max-steps` value.

Per-batch bounds are valid but not shared limits: ten batches each allowing 16 items can admit 160 top-level items. There is no built-in run total in workflow results for the lead to use. This is not a measured provider overload claim.

## Tasks
- [ ] Retain available usage per task/attempt in both SDKs and durable run records. Aggregate without double-counting resumed/retried/carried attempts; distinguish billed attempts from reused artifacts. Missing usage is unknown, not zero.
- [ ] Add opt-in process-safe admission shared by run/workspace/provider-account scope, with fair bounded queues and structured queued reasons. Keep local concurrency settings as additional ceilings. Avoid putting API keys in scope IDs or records.
- [ ] Specify acquired/released permits and crash recovery using existing ownership seams; stopped processes must not be treated as dead. Handle nested lead/worker execution without permit deadlock or bypass.
- [ ] Propagate explicit step/depth/deadline ceilings to children and add run-level request/token budget policies. Pause new admissions or cancel according to explicit policy at safe boundaries.
- [ ] Keep hard launch/step limits distinct from best-effort usage/cost limits. Provider usage may arrive late or be unavailable; monetary estimates need known pricing and must be labeled estimates, especially for subscriptions.
- [ ] Expose effective limits, usage, pending reason and exhaustion outcome in JSON/dashboard. Define support for direct background launches, jobs, SDK and host-owned loops; do not imply a global guarantee for paths not enrolled.

## Acceptance
- Two independently submitted fixture batches share a cap of two; the fixture never sees more than two enrolled requests at once. Cover cancellation/admission races, supervisor death, paused owners, fair progress and nested work.
- Verify no new work starts after a hard budget is exhausted and no recorded usage is counted twice on recovery. Unknown usage stays explicit.
- Add tests for explicit parent step-limit inheritance and secret-safe scope identities.
- Benchmark queue/admission overhead and capture task/run usage from deterministic fixtures. Run relevant jobs/subagent/SDK tests plus `make test`, `make quality`, `make leaks` and size checks. Update ADR/docs, wasm capability behavior and Nix inputs.


## #159: Swarm 6/6: allocate fan-in prompts after admission and support selective context

State at retrieval: OPEN.

Parent: #152. Independent optimization; can start before the new swarm service. Priority: P2. Size: M. Baseline: `cea97b372e6671acd5bca88341c10d85d34486d6`.

## Evidence
Python renders dependency context before acquiring its semaphore: [`workflow.py:525–528`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/python/src/tny/workflow.py#L525). TypeScript does the same before `semaphore.run`: [`index.mjs:1073–1081`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/typescript/dist/index.mjs#L1073).

The existing direct-output byte bound is real and must be preserved: [`workflow.py:246–278`](https://github.com/thehumanworks/tny/blob/cea97b3/sdk/python/src/tny/workflow.py#L246), [`tny-workflows.sh:498–533`](https://github.com/thehumanworks/tny/blob/cea97b3/shell/tny-workflows.sh#L498). Current fan-in is whole included output or ordering-only, not unlimited context. Large included outputs fail above the bound rather than falling back to selective artifact retrieval.

### Reproduced during the review
Using the Python SDK's fake-runner seam, no provider/network calls:
1. Produce one 262,144-byte result.
2. Add 32 consumers, each depending on that result, with `max_concurrency=1`.
3. Block the first consumer runner on an asyncio event and wrap `_render_prompt` to count completed consumer renders.
4. Yield once, before releasing that consumer.

Observed: **all 32 consumer prompts had been rendered**, totaling **8,395,008 prompt bytes**, while only one consumer was admitted. This is constructed prompt-byte volume, not an RSS measurement. The run completed successfully after release. Existing Python workflow tests also passed (13 tests).

## Tasks
- [ ] Move expensive composition inside the admission slot in both SDKs, or use an equivalent bounded lazy representation. Preserve dependency failure checks, declared edge order, cancellation responsiveness and existing runner contracts.
- [ ] Add a regression fixture that holds the first consumer and asserts waiting tasks do not materialize copies. Measure peak memory for wide fan-out before/after, not just elapsed time.
- [ ] Add opt-in context selection beyond whole-output/none: explicit summary or structured fields, and an artifact reference with provenance plus bounded read access. Never silently truncate a result or automatically trigger another paid summarization call.
- [ ] Bound the complete composed input as well as direct output bytes, and expose token estimates only where supported. Preserve byte-bound behavior for providers without a reliable tokenizer. Ensure referenced artifacts are accessible in the actual SDK/SSH/workspace execution context.
- [ ] Keep outputs labeled as untrusted dependency data. Preserve current default behavior for existing workflows unless an ADR explicitly changes it.

## Acceptance
- Under concurrency one, the 32-consumer regression renders at most the admitted consumer prompt while it is blocked. Run the equivalent JS test.
- Tests preserve mixed fan-in ordering, `--no-context`, bound failures before provider execution, cancellation and failure propagation. Test explicit selective-context behavior without losing original artifacts.
- Publish reproducible baseline/candidate peak memory, serialized prompt bytes and latency; claim only measured improvements. Add SDK/shell conformance checks for any shared new option.
- Run SDK and shell-workflow tests and applicable quality checks; run root gates if native code changes. Update docs/ADR and Nix inputs for any new tests/tools. State browser/native applicability accurately.

