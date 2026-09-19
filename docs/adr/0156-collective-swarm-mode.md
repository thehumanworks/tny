# ADR 0156: collective swarm policy over durable teams

Date: 2026-09-19. Status: accepted.

## Decision

`/swarm [n]` and global/ask-local `--swarm[=n]` select an opt-in collective
facilitation policy. The initiating saved session remains the lead; n counts
collaborators only. One is valid. Omission is represented distinctly and leaves
the choice to the lead. Explicit n is 1..16, matching existing shared admission.
No scheduler, broker, agent loop or public C ABI is introduced.

Mode/cap live in session metadata and runtime checkpoints, independently of task
presets and explicit system instructions. Idle interactive enablement gracefully
ends and reaps the old session writer, reloads under writer ownership, saves the
selection, then rebinds. Failed session reconciliation restores the old cap.
A new session permits a new cap; an established session cannot widen its scope.
Adoption refuses pre-existing active/uncertain parent-owned jobs. Legacy job
retry and isolated subagent execution are refused in swarm mode; the lead submits
new worker tasks. Collaborators inherit an opt-in collective marker and existing
nested-launch refusals, not the lead's orchestration mode.

Every swarm job submission, including legacy job tools and terminal adapters,
is a worker-only DAG with at most n items. Runtime-owned admission is keyed by
parent session, shared across repeated/concurrent runs. The existing immutable
admission ledger enforces active capacity and retains cleanup holds. The run is
the shared channel; existing DAG/attempt states express specialized workflows.
Arbitrary same-user shells are not a security sandbox.

Channel publish snapshots active recipients (lead plus peers excluding sender)
under the existing state lock and atomically persists all receipt rows in the
same mailbox file before success. A stable publication ID reconciles the original
set after endpoint termination. Independent receipt IDs preserve acknowledgement,
replay, attempt fences and retirement tombstones. Fanout consumes existing per-
recipient outstanding and run-history quotas atomically. Publication results omit
repeated bodies; receivers obtain the original payload. Direct sends stay private.
Compact topic/thread/type/body JSON envelopes are convention, not another database.

Mailbox wait registers a directory watch before the durable snapshot: kqueue on
Darwin, inotify on Linux. Atomic record replacement and queue overflow are hints
to resnapshot. Quiet waits never periodically reread mailbox state; a 50ms control
heartbeat observes cancellation only. Lock contention has a separate finite
250ms retry bound. Timeout is 0..30000ms; empty, deadline, terminal, stale,
cancelled and failure outcomes are distinct. Watch loss fails visibly. Existing
safe-boundary context insertion remains persist-before-delivered, deduplicated
by receipt ID, without implicit acknowledgement or busy-tool interruption.

The bounded collective policy precedes changing context. It asks for ownership,
proposals, counterexamples, peer replies, verification and convergence, while
allowing trivial work without collaborators. It never serializes a changing board
into the system prompt. No provider-cache, quality or performance gains are claimed.

## Alternatives and limits

A new scheduler/channel service would duplicate authoritative jobs and mailbox
state. Best-effort loops of direct sends cannot provide an atomic recipient set.
Periodic inbox/model polling wastes calls and creates avoidable wakeup races.
Filesystem events do not promise exactly-once reasoning or worker progress.

History remains bounded at 256 receipt rows, with 64 outstanding per recipient;
acks free outstanding quota, never history. Sixteen-recipient publications consume
sixteen rows each. No silent eviction is introduced. Windows/MSYS, wasm, SSH,
ephemeral and embedded mode refuse collective mode before inference. Existing
non-collective operation retains its policy and admission behavior.

Validation and known gaps: [evidence](../verification/collective-swarm/evidence.md).
