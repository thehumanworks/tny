# Purposeful nested swarms: verification contract

Baseline: `28011be` (origin/main); implementation branch:
`feat/purposeful-nested-swarms`. User authorized live evaluation against the
existing Codex account, using `gpt-5.6-sol` or Luna with matching harness models.
No credentials or private account identifiers enter committed evidence.

## Required invariants

1. A versioned file describes a swarm's purpose, coordinator, and named agents
   with individual purposes. Nested swarms each require their own coordinator.
   Missing/duplicate/malformed definitions, excessive depth/size, unsupported
   contexts, and impossible capacity fail before provider or job side effects.
2. Definitions are executable, not merely documentation. The root coordinator
   is the initiating session; nested coordinators and participants retain
   explicit group identity and purpose. Reuse the existing durable team runtime,
   shared admission, attempt fencing, permission and workspace ownership rules.
   No second scheduler, broker, unrestricted recursive spawning, or silent cap
   expansion. Keep existing `/swarm [n]` and `--swarm[=n]` behavior compatible.
3. Communication is purposeful and event-driven. Preserve persist-before-success,
   explicit acknowledgements, replay/idempotency, bounded waits, cancellation,
   and observable failures. Audit mailbox and completion waits for polling and
   queue deadlocks. Do not interpret at-least-once delivery as guaranteed progress.
4. Keep stable role/purpose instructions separate from changing messages;
   prefer scoped/direct evidence and bounded summaries over broadcast storms.
   Never claim actual cache hits without measured provider usage.
5. Persist enough definition/provenance to resume consistently; do not silently
   reread a changed source file. Preserve checkpoints and existing explicit
   instructions. All new behavior has an explicit platform support boundary.
6. Unit/integration tests exercise valid nested execution, missing coordinators,
   duplicates/depth/capacity, failure/timeout, authority, and no-poll waits.
   Run release, relevant regression suites, unit, quality and leak gates; record
   exact revisions/exits and distinguish pre-existing failures from regressions.
   Obtain an independent review and resolve material findings.
7. Live comparison uses identical model/effort, identical task fixtures and
   deterministic external correctness checks across increasing complexity.
   Record actual elapsed time, terminal status, complete available usage
   (including children), observed participation, timeout/failure, and limitations.
   Include repeated matched trials where feasible; no selective failed-run removal.
8. Commit coherent checkpoints; push this branch and create a PR. Leave the
   primary worktree and other worktrees unchanged. Completion requires an actual
   PR URL and an honest reconciliation of every invariant above.
