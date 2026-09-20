# Swarm-factory effectiveness: verification contract

Baseline `8f77e71`; worktree `~/projects/tny/.worktrees/swarm-factory`;
branch `feat/swarm-factory-effectiveness`, now merged with main through `e5721a7`.
The main worktree remains unchanged. Writable/yolo defaults from ADR 0159 remain;
read-only and isolated contribution workspaces are explicit overrides.

1. Research primary sources before implementation; document which conclusions
   are studies, implementation reports and local hypotheses. Preserve collective
   direct peer collaboration rather than a delegate-only workflow.
2. Retain version-1 canonical definitions and all existing authorities. Add a
   version-2 contribution contract with deliverable, acceptance criteria, named
   dependencies and explicit workspace capability. Invalid/cyclic/unknown/self
   dependencies, duplicate fields/names and impossible authority fail before
   provider or job effects. Every nested group still requires a coordinator.
3. Compile into the existing durable DAG/workspace system; no second broker,
   scheduler or recursive supervisors. Isolated edits are explicit and never
   auto-merged/auto-accepted. Failed dependencies cannot be silently accepted.
   Carry direct predecessor evidence or pointers with integrity and bounded size.
4. Typed peer communication resolves names through the caller's current run,
   never from model-asserted membership. Content-addressed automatic IDs retain
   retry identity; explicit IDs allow intentional repeated identical messages.
   Preserve at-least-once replay, manual ack, deadlines, cancellation and fail-closed
   attempt validation. No claim that delivery guarantees consensus or correctness.
5. Keep stable identity/contracts separate from dynamic evidence; no broad
   transcript broadcast. Clearly distinguish declared acceptance criteria from
   executed external checks. Success must not be inferred from a nice summary.
6. Test validation, dependency transitions, workspace authority, immutable resume,
   peer-to-peer and upward handoffs, replay, and failure/cancellation. Run quality,
   focused integration/unit, ownership, and leak checks; use serial builds to avoid
   generated-header races. Review with a fresh agent and action material findings.
7. Use existing authorized Codex account/model settings for bounded synthetic
   live evaluation after offline tests; record frozen binary/revision and full
   outcomes/usage, with private raw sessions excluded from publication. Keep
   deterministic held-out oracles outside generated workspaces. No universal
   performance guarantee from a small sample.
8. Commit often, publish a PR/stack or update existing PR as appropriate, preserve
   primary and unrelated worktrees, and report actual verification and CI state.
