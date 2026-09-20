# ADR 0157: compile purposeful file-defined nesting into one durable team

- Status: accepted
- Date: 2026-09-20

## Context

ADR 0156 added opt-in collective facilitation, shared admission and durable peer
mailboxes, but membership remained a count chosen at runtime and nested collaborators
were forbidden. Repeatable work needs reviewable identities, purposes and group
ownership without creating recursive supervisors or weakening the existing durable
job invariants.

A source file may change after a session begins. Reading it again on resume would
silently change membership and authority. Coordinators can also deadlock a bounded
pool when they start and wait before the peers whose evidence they require.

## Decision

Add the closed, versioned JSON contract in `schemas/swarm.schema.json` and select it
with `--swarm-file PATH`. `tny swarm validate FILE [--json]` performs the same strict
parse and semantic validation without session, job, workspace or provider effects.
Version 1 requires a root purpose/coordinator, named agents, and recursively named
groups whose coordinators are mandatory. Limits are 64 KiB, 64-byte names,
4096-byte purposes, depth 4 and 16 launched participants. Names are globally unique;
duplicate or unknown fields and blank purposes are rejected.

The current session remains the root coordinator. Every nested coordinator and agent
is flattened into one worker item in the existing durable team DAG, carrying explicit
swarm role, group, purpose and upward-coordinator metadata. One shared admission scope,
attempt fencing, permission policy, workspace ownership and durable mailbox applies
to the entire tree. Compiled concurrency equals validated membership so coordinators
cannot consume the team-local runnable limit before their peers; global admission is
still authoritative. No recursive supervisor, scheduler or broker is added.

Stable identity and purpose enter the system-policy prefix. The current root task and
mailbox deliveries remain dynamic. Direct peer discussion is available, nested
coordinators receive scoped completion notices and synthesize upward, waits remain
bounded, and acknowledgements/replay keep their existing semantics. Communication is
optional and neither completion nor delivery guarantees progress or convergence.

Store the canonical definition, SHA-256 digest, absolute source provenance, participant
count, activation state and resulting run ID in the session. Checkpoints preserve the
same resolved snapshot. Resume uses the snapshot without rereading the source; an
explicitly supplied file must validate and match. Persist `launching` before submission
and refuse automatic resubmission if the outcome is uncertain.

Keep `--swarm[=N]` and `/swarm [N]` unchanged. Purposeful activation is supported only
for native local saved Darwin/Linux leads with the jobs/watch seams; validation is
context-free. Unsupported execution contexts fail before launch/provider effects.

## Consequences

Definitions are executable, inspectable inputs rather than advisory prose, and nested
ownership is visible in durable status. Flattening keeps one authority and failure
model, but nesting is intentionally bounded and does not provide autonomous recursive
orchestration. Reserving all team-local participant slots avoids coordinator/peer pool
deadlock at the cost of requesting the full validated concurrency from global admission.

Canonical snapshots make resume deterministic even if the file is edited or removed.
An interruption during submission can require operator inspection because tny favors
duplicate prevention over speculative recovery. Purposeful collaboration may still
fail, time out, disagree or produce a wrong synthesis; the runtime does not claim
guaranteed convergence.

## Alternatives rejected

- Recursive coordinator-owned teams: splits admission and authority, complicates
  attempt fencing and can deadlock bounded capacity.
- Reread the source on every resume: makes saved sessions depend on mutable external
  state and can silently change participants.
- Encode roles only in prompts: loses validation, durable identity and scoped runtime
  behavior.
- Reserve fewer runnable slots: allows coordinator waits to starve queued peers.
