# Purposeful file-defined swarms

Purposeful swarms turn a checked, versioned JSON definition into one durable team.
The file names the root purpose and coordinator, every participant and purpose, and
any nested groups. The current saved session is the root coordinator; coordinators
of nested groups are real launched participants.

Validate a file without opening a session, creating a job or contacting a provider:

```sh
tny swarm validate examples/swarm/purposeful-review.json
tny swarm validate examples/swarm/purposeful-review.json --json
```

Activate it for a native local saved turn:

```sh
tny --swarm-file examples/swarm/purposeful-review.json ask \
  "Review the parser change and reconcile the evidence"
```

`--swarm-file PATH` is accepted globally and as an ask-local option. Relative paths
resolve from the launch directory. It cannot be combined with `--swarm[=N]`: the
definition itself fixes the number of launched participants. Existing numeric
`--swarm[=N]` and interactive `/swarm [N]` behavior is unchanged.

## Version 1 format

The normative machine-readable shape is
[`schemas/swarm.schema.json`](../schemas/swarm.schema.json). The root object has
exactly these fields:

```json
{
  "version": 1,
  "purpose": "One non-blank root purpose",
  "coordinator": {"name": "lead", "purpose": "Own synthesis"},
  "agents": [
    {"name": "reviewer", "purpose": "Review the implementation"}
  ],
  "swarms": [
    {
      "purpose": "Verify behavior",
      "coordinator": {"name": "verification-lead", "purpose": "Synthesize checks"},
      "agents": [
        {"name": "tester", "purpose": "Run focused tests"}
      ],
      "swarms": []
    }
  ]
}
```

Every object is closed: unknown and duplicate fields are errors. All five root
fields and all four nested-group fields are required, including empty `agents` or
`swarms` arrays. Names are non-empty UTF-8 strings of at most 64 bytes without
surrounding ASCII whitespace and must be globally unique, including the root
coordinator. Purposes are non-blank
UTF-8 strings of at most 4096 bytes. Files are at most 64 KiB. The root is depth 1;
the maximum depth is 4. The whole tree must launch 1..16 participants, excluding
the current root coordinator. A nested coordinator counts as a participant.

Runtime validation is authoritative where JSON Schema cannot express byte limits,
duplicate JSON object keys, or global uniqueness. Validation completes before a
session, workspace, job, runner or provider effect. Unsupported execution contexts
also fail before those effects.

## Runtime model

tny flattens the bounded definition tree into one existing team DAG. It does not
start recursively nested supervisors or introduce another scheduler. All nested
participants share the parent session's admission scope, permissions, workspace
rules, attempt fencing and durable run mailbox. The compiled concurrency equals the
validated participant count so a waiting coordinator cannot occupy all available
slots before its peers launch; the ordinary global admission cap remains decisive.

Each task persists its swarm name, agent/coordinator role, group, own purpose, group
purpose and upward coordinator identity. Direct group-peer task indices and the
current root task are supplied in the task prompt. Stable identity and purpose live
in the system-policy prefix; changing task text and delivered messages remain
dynamic. Nested coordinators receive completion notices for their direct group and
child-group coordinators, then synthesize upward. Participants may discuss directly
through authenticated durable mailboxes; whole-run publication is available but is
not required.

Messages preserve the existing persist-before-success, replay, acknowledgement and
attempt rules. Participants should acknowledge processed receipts, prefer scoped
evidence, and use bounded waits. A completed task is not proof of agreement, correct
synthesis, progress or convergence.

## Resume and failure behavior

Before activation, tny canonicalizes the validated definition, records its SHA-256
digest and absolute source provenance, and stores the snapshot in the session and
checkpoint. Resume, runner rebind and checkpoint restore use that snapshot and do
not reread the source file. Supplying the same `--swarm-file` on resume validates it
and requires its canonical digest to match; a changed file is refused. An existing
run is registered rather than relaunched.

Activation permission is resolved before intent. The intent contains a fresh
128-bit activation identity before job submission, and the durable job records the
same identity with its parent session and definition digest. If interruption leaves
the state at `launching`, resume scans the bounded private job store: exactly one
matching run is adopted only after its capacity, ordered membership and coordinator
links match the canonical saved manifest; no match retries the same identity; multiple
or malformed matches fail closed. An `active` restore performs the same provenance and
topology checks before provider I/O.

All `swarm_*` request metadata is compiler-owned. Public `team start` and `jobs submit`
JSON cannot assert it, even partially. The internal compiler passes the validated
manifest through a non-serialized API and jobs validates the complete projection
before it creates a directory.

## Platform support

Purposeful activation has the same boundary as collective teams: native local saved
sessions on Darwin and Linux with supported job execution and directory watches.
wasm, Windows/MSYS, SSH, embedded and ephemeral contexts reject activation.
`tny swarm validate` is local and context-free wherever the CLI can read the file.
No provider request is needed for validation.

See [collective swarm mode](collective-swarm.md), [team control](team-control.md),
[mailboxes](team-mailbox.md), [ADR 0157](adr/0157-purposeful-file-defined-swarms.md),
and the [implementation evidence](verification/purposeful-swarms/implementation.md).
