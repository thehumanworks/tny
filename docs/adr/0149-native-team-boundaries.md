# ADR 0149: Native team boundaries, permission ceilings and safe delivery

Date: 2026-09-18. Status: proposed; integrated verification pending.
Related: #153, #156–#158; ADRs 0143, 0145–0148 and 0126.

## Decision

A DAG job is a run; an item index is a task and its execution attempt is fenced
by the existing jobs owner. The private child handoff supplies a random per-attempt
member capability in environment, never argv, job metadata or canonical logs.
Only its SHA-256 verifier enters the private job record. A capability is distinct
from a session ID. Synchronous subagents do not inherit a parent's membership.
The submitting parent is captured from the active tool session, not JSON input.

Mailbox CLI/tool/interception adapters share one service and separate send,
read, acknowledgment and retirement permission identities. Ordinary local CLI
operators have the same-user authority as jobs; nested agents must authenticate
as the recorded parent or a private member. Shell processes under the same OS
user are not security sandboxes and can reach that user's files.

The submitting parent is the administrative lead (mailbox task -1). Configured
item roles remain descriptive and never confer that authority. Indexed members,
including a lead-role item, need explicit peer opt-in to message other indexed
members. Parent-to-worker and worker-to-parent routing does not require it.
Attempt retirement is explicit and retains receipts. Old-attempt messages are
never silently delivered to new work. Workspace mutations require the recorded
parent/operator; a member may inspect only its own finished task.

The native loop pulls bounded messages only before a new model call, after
previous tool work has quiesced. It saves untrusted user-role context and dedup
keys before marking messages delivered. A durable cursor advances only after
that mark, so unacked earlier messages cannot starve later deliveries. Ack is
separate. A crash between these steps can replay the same message ID; persisted
context keys prevent duplicate native insertion. No exactly-once reasoning or
external-effect guarantee follows. Host loops do not receive unsafe injection.

Completion notifications use authoritative item outcomes at the same boundary;
execution success is not verification or integration. Reads and timeouts do not
cancel work. A failed persistence operation stops a provider request rather than
silently losing a required notification. At a safe boundary, short mailbox-lock
contention gets at most 250 ms of control-only pumping. Persistent contention
stops the request instead of posting without queued context. Tools are not
interrupted and the backend is never reentered. Canonical status events report
durable context delivery without exposing the payload or capability.

`shared_read_only` is an inherited permission ceiling before yolo, settings and
session grants. Native file writes, write-capable terminal commands and child
launches are denied. Simple read commands remain available to terminal profiles.
The private/public checkpoint snapshot retains this bool; recovery cannot relax
a newly resolved read-only ceiling. This stays within ADR0126's existing private
ownership area and does not change the public C ABI. Worktree isolation itself
is still not a privilege boundary.

Owned job descendants cannot start first-party background terminal commands.
Collection of existing terminal handles stays separate. This restriction uses
the inherited job-parent marker, not membership alone, because synchronous
subagents intentionally lose member capabilities. Ordinary non-job background
terminals keep the upstream completion/observer behavior. Arbitrary same-user
shell daemonization is not contained by this policy.

An enrolled shared-admission item cannot recursively launch native subagents or
nested jobs. This initial explicit refusal avoids permit deadlock and hidden
nested enrollment. Local limits are additional ceilings. An admission claim is
one top-level job launch, not an HTTP request, token allowance or currency amount.
No global guarantee covers arbitrary same-user programs or unenrolled SDK loops.

## Alternatives and verification

A separate always-on controller, a second provider loop, prose-parsed checks and
PID-derived membership were rejected. Raw queued text never becomes system
policy, permission approval or a verification command. Existing synchronous
subagent APIs remain synchronous and preserve their ordinary behavior.

The delivery contract requires real busy-tool/clarification fixtures, loss and
replay barriers, wrong-member and stale-attempt rejection, permission/intercept
parity, workspace conflict preservation, shared admission races, full native
quality/leak tests, independent review and explicit platform results. This ADR
records the intended integrated design; it is not evidence those gates passed.
