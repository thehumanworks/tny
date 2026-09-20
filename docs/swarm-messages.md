# Typed purposeful-swarm messages

`swarm_message` is a named, typed send adapter for a currently active purposeful
swarm. It uses the existing durable team mailbox; it is not a queue, broker,
scheduler, broadcast channel, or replacement for `team_mailbox`.

```json
{
  "to": "verification-lead",
  "kind": "finding",
  "topic": "parser-boundary",
  "text": "The split-frame fixture covers every byte boundary."
}
```

`to`, `kind`, `topic`, and `text` are required. `to` is the exact name of a
participant or the root coordinator in the active run's canonical purposeful
topology. It is never interpreted as a task ID or accepted from model-claimed
membership. `kind` is one of `finding`, `question`, `answer`, `challenge`,
`decision`, `handoff`, or `blocker`.

Names are at most 64 UTF-8 bytes. Topics are nonblank UTF-8 of at most 256 bytes.
Text is nonblank UTF-8 of at most 16,384 bytes, and the serialized envelope must
also fit the mailbox's 16,384-byte payload bound. Unknown or duplicate fields,
embedded NULs, invalid UTF-8, surrounding transport ambiguity, and invalid kinds
are refused before a message is stored.

## Current-run resolution

`run` is optional. Omission resolves only from one of these authenticated runtime
contexts:

- the current saved session's active purposeful activation; or
- an inherited team member run whose private capability and current attempt still
  authenticate against the locked job record.

If both identities exist they must name the same run. An explicit `run` must equal
that resolved current run; it cannot select another known job. Ordinary sessions,
ambiguous activation state, mismatched runs, stale attempts, forged member
environment, unknown names, and non-purposeful DAGs fail closed.

The adapter resolves the recipient from `swarm_root_coordinator` and ordered
`items[].swarm_name` fields in the service-owned current run projection. Permission
preparation privately retains the authenticated sender attempt, exact recipient
name/task/attempt, topology digest, canonical envelope and ID. Execution sends that
snapshot to the captured mailbox endpoint without resolving model arguments again.
Under the mailbox lock it verifies that the topology digest and name mapping are
unchanged; changed sender, recipient or run attempts are stale and have no message
effect. Peer-to-peer messages therefore still require the run's existing peer
messaging authority; parent/member direction and terminal-state rules are
unchanged.

## Envelope and receipt identity

The stored payload has one deterministic encoding:

```json
{"version":1,"kind":"finding","topic":"parser-boundary","body":"..."}
```

The body is untrusted collaboration context. It cannot grant permission, change
the recipient's task, prove verification, or establish acceptance or consensus.

`id` is optional. If omitted, tny hashes the envelope together with the resolved
run, authenticated sender task/job/task attempts, and recipient task/attempt. The
mailbox ID is `sm1-` followed by 60 lowercase hexadecimal characters (240 SHA-256
bits), fitting the existing 64-byte limit. An exact retry in the same endpoint
attempt derives the same ID and returns the original receipt. A new sender or
recipient attempt derives a different ID.

An explicit ID uses the mailbox's existing 1..64-byte ASCII
letters/digits/`.`/`_`/`-` contract. Reusing it with identical content and
endpoints is an idempotent retry; reusing it with changed content or endpoints is
`MAILBOX_CONFLICT`. To send intentionally repeated equal messages, use distinct
explicit IDs.

A successful call returns only a compact durable receipt:

```json
{"kind":"swarm_message_receipt","id":"sm1-...","topic":"parser-boundary","recipient":"verification-lead","sequence":4,"state":"queued"}
```

Success means the message was persisted. It does not mean the recipient saw,
processed, agreed with, or acted on it.

## Permission, replay, and support

Every call is prepared under the existing `team_send` permission identity before
the durable send. Shared-read-only purposeful participants may use it because the
operation changes harness mailbox state, not workspace files; user rules and the
read-only ceiling are not widened. `TNY_TOOLS=all` advertises it, including to
native read-only participants. Explicit terminal-only profiles retain their
existing surface. Raw `team_mailbox` remains callable and unchanged.

Delivery remains at-least-once context with explicit acknowledgment. Queued and
delivered messages replay through `team_mailbox inbox/read` until the recipient
calls `team_mailbox ack` with the exact receipt ID. Safe-boundary automatic
delivery marks delivery only; it never auto-acks, interrupts a busy tool, promises
exactly-once execution, or turns content into authority.

The tool is supported for saved native local CLI runners on Darwin and Linux.
Wasm, SSH, embedded/library, ephemeral purposeful activation, and runtimes without
native durable jobs refuse before a message effect. See
[durable team mailboxes](team-mailbox.md),
[purposeful swarms](purposeful-swarms.md), and
[ADR 0161](adr/0161-typed-swarm-messages.md).
