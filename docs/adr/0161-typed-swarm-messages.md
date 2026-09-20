# ADR 0161: typed named messages adapt the durable team mailbox

- Status: accepted
- Date: 2026-09-20

## Context

Purposeful swarm participants have stable names and a durable authenticated
mailbox, but the raw send surface requires a run ID, numeric recipient task and a
caller-chosen message ID. Those transport details are useful for the general
mailbox API and unnecessarily error-prone for named purposeful collaboration.

The factory verification hypothesis is local: a typed named adapter should remove
accidental bookkeeping while retaining the mailbox's authority, replay, attempt
fencing, and conflict semantics. It does not claim that structured prose improves
correctness, creates consensus, or provides exactly-once execution.

## Decision

Add the builtin `swarm_message` tool as a thin adapter over
`tny_team_mailbox_send`. Required inputs are exact participant/root-coordinator
`to`, a closed seven-value `kind`, bounded nonblank `topic`, and bounded nonblank
`text`; `id` and `run` are optional. Strict runtime validation rejects duplicate
and unknown fields, invalid UTF-8/NULs, blank values, invalid IDs and an envelope
larger than the existing mailbox payload bound.

Resolve the run only from the current saved purposeful activation or inherited
member run. Multiple different candidates, an explicit mismatch, or the absence
of either context is an error. Resolve names from the current service-owned job
projection: `swarm_root_coordinator` maps to mailbox lead `-1`, while ordered
`items[].swarm_name` maps to its item index/current attempt. Reject missing,
duplicate, malformed or non-purposeful topology. Model-supplied task/session IDs
never establish identity.

Authenticate during permission preparation with a pure mailbox snapshot, then
prepare the call as `team_send`. At execution, call the unchanged mailbox send,
which authenticates again under the locked current job record and enforces sender,
recipient, peer, attempt, terminal, capacity, durability and conflict rules. This
does not add permission or weaken shared-read-only policy.

Serialize one versioned payload in fixed field order:

```json
{"version":1,"kind":"...","topic":"...","body":"..."}
```

When `id` is absent, hash a domain-separated canonical send identity containing
the exact envelope bytes and length, run, authenticated sender task/job/task
attempts, and resolved recipient task/attempt. Encode the first 240 SHA-256 bits
as `sm1-` plus 60 lowercase hexadecimal characters. Exact same-attempt retries
therefore reconcile to one receipt; changed endpoint attempts do not. Explicit IDs
retain the mailbox's unchanged exact-retry and conflict behavior and let callers
create repeated equal logical messages by choosing distinct IDs.

Return `id`, `topic`, recipient name, sequence, and stored state without echoing
the body or claiming recipient processing. Keep raw `team_mailbox` unchanged for
inbox, read, wait, ack, retire, numeric routing, publication, and recovery.

## Consequences and limits

The implementation introduces no scheduler, broker, polling loop, queue file,
automatic acknowledgment, transcript broadcast, public ABI change, or new
authority. Messages remain untrusted task context, replayable until explicit ack,
and subject to existing bounded history/outstanding limits.

The builtin is advertised in the all-tools profile, including native purposeful
shared-read-only participants. Terminal-only profiles remain unchanged. Native
local saved sessions are supported; wasm, SSH, embedded/library and unsupported
durable-job runtimes refuse before mutation.

Deterministic offline tests cover schema/profile boundaries, strict field and name
validation, UTF-8, current-run ambiguity/mismatch, permission denial, attempt-
scoped IDs, exact retry, explicit conflict/repetition, two peer-to-root sends, and
replay through manual ack. Localhost integration still measures transport behavior,
not general model quality.
