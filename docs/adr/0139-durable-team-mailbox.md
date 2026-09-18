# ADR 0139: Bounded durable collaboration mailboxes over job authority

- Status: Accepted for the mailbox service slice; public integration pending
- Date: 2026-09-18
- Issue: #156 (parent #152; identity/control #153)
- Contract: [team-mailbox.md](../team-mailbox.md)

## Context

Synchronous subagent follow-up and interrupting steer are not nondestructive
messages to a busy worker. The existing durable job record already owns execution,
item membership and retry attempts. A second queue or daemon would create another
source of execution authority and new recovery races.

## Decision

Implement a C11 service with a public C ABI in `core/team_mailbox.c/.h`. Use the
existing 32hex job ID as run, item index as task, and job/item attempts as fences.
Store one bounded mailbox record beside `job.json`, under its existing nonblocking
`state.lock`. Reuse private atomic writes in `util/jobs_host` and confined bounded
reads in `util/image_io`. The service does not start work or change job records.

Caller identity is an explicit **trusted C input**, separate from untrusted
message arguments. Require a callback to verify the private member capability or
lead authority and permission ceilings against the authoritative record read
under lock. Do not accept an authorization-granting public sender/session field.
The lead owns capability provisioning: random secret in private launch payload,
verifier in job record, environment handoff rather than argv/logs. The service
validates record structure, all task indices, endpoint membership and fences in
addition to the callback. Same-user shell privileges are not sandboxed.

Permit lead↔worker messages; worker↔worker requires explicit authoritative opt-in.
Reject new messages to/from terminal/canceled members. Retain access to accepted
receipts and explicit read/delivery/ack for the current attempt after completion.
Never reinterpret an old attempt's inbox as a new attempt's context.

A caller chooses a stable message ID. Successful send follows persistence.
An identical retry returns the original receipt; any change to identity, attempt,
recipient or content conflicts. Run-global sequence orders committed messages.
Queued, delivered and acked are separate persisted states. Only the recipient can
read or advance states. Read is pure; mark-delivered follows consumer persistence;
ack is explicit and requires delivery. No state can regress.

Bound payloads to 16 KiB, outstanding records to 64 per recipient task and total
retained records to 256 per run. Retain full acked records for exact duplicate and
conflict detection. No TTL or eviction: lifetime history exhaustion explicitly
backpressures. Retention ends with authorized enclosing-job deletion. Inbox calls
return at most 16 messages and 64 KiB payload; no provider callbacks or wakeups.

The native adapter must deliver at `start_post` before request creation, never
inside a tool or by backend reentry. Persist transcript context with stable IDs
before marking delivered. Replay unacked records, including delivered records,
and dedup against the persisted transcript. Messages remain untrusted user
context, never system policy or execution/check approval. Host providers without
a safe boundary support explicit read/queued-next-turn only. SSH/embedded/wasm
execution mutation is unsupported before side effects.

## Alternatives and tradeoffs

- A session ID or public sender parameter is not authentication. Reject it.
- A second scheduler/daemon duplicates authority. Reuse jobs and their lock.
- Exactly-once reasoning is not enforceable. Provide at-least-once delivery with
  consumer dedup and explicit ack, not exactly-once external effects.
- Evicting acked IDs would let old retries enqueue again. Retain bounded lifetime
  history and fail visibly when full instead. Heavy discussion needs a later
  explicit archive/abandonment design or a new run; no hidden resets.
- A single atomic record keeps updates transactional and easy to inspect, but
  rewrites history while holding the short job lock. The size is bounded; no
  latency/performance improvement is claimed. Contention returns BUSY immediately.
- File fsync + rename inherits the existing host seam's process-crash guarantees.
  No parent-directory fsync means power-loss durability is not proved here.

## Verification and open integration

Focused tests call real C/host code and inspect actual durable records, with
SIGKILL/reopen, duplicate/concurrent sends, order/state transitions, capacity,
capability/member/attempt denial, terminal recipients and lock contention.
The callback fixture is not production authentication. The transcript fixture is
not the native provider loop. Public CLI/tool parity, safe-boundary busy delivery,
clarification round trip, real persisted-transcript crash dedup and platform
capability checks remain lead-owned acceptance criteria. See the contract page
for exact ABI and wiring. This ADR and helper tests do not complete issue #156.
