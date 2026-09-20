# ADR 0158: Capability-aligned purposeful participant context

Status: accepted. Date: 2026-09-20. Extends ADR 0157.

## Evidence

The retained 12-trial matched evaluation at `00e717f` produced correct artifacts
in both arms, but three of six tny runs had unsuccessful orchestration. Five
read-only workers requested forbidden compound shell commands or Python probes.
The inherited permission ceiling correctly denied them; advertised tools and
role context had failed to explain that ceiling. One additional participant was
cancelled by the root. These outcomes must not be relabelled as swarm success.

## Decision

For purposeful read-only participants using the all-tools profile, advertise the
native safe-tool set plus team controls/mailboxes and bounded job/workspace
inspection. Hide terminals, mutators and launchers from this advertised surface.
Do not change execution permission checks, custom-tool registration, ordinary
teams, the root, or explicit shell-only profiles. Schema filtering is guidance,
not authority enforcement or an OS sandbox.

Keep capability and purpose guidance in the stable system prefix. Explain that
root task text supplies context rather than overwriting each participant's role.
Coordinators synthesize evidence, not gain peer cancellation/collection authority.
Include concrete typed mailbox examples, and make the root responsible for
executable checks, reconciliation and disclosure of unsuccessful participants.
Do not cancel a healthy peer merely to declare completion; bounded waits and an
honest incomplete outcome are preferable. This is orchestration guidance, not
a runtime guarantee that a model follows it.

## Consequences

The v1 manifest still intentionally has no per-agent writable workspace option.
The fixed review topology may cost more than a single agent and is not a claim
that swarms improve every task. Publish the original failed evaluation and a
separate post-fix ladder, with model usage, cache coverage, coordination, and
external correctness. Count failed/cancelled model sessions from attempt logs;
job `session_id` is success-only provenance, not a launch counter.
