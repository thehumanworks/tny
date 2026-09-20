# Swarm contribution contracts

Version 2 purposeful swarm manifests add bounded contribution contracts to the
same durable team runtime as [version 1 purposeful swarms](purposeful-swarms.md).
They describe what each participant should deliver, the criteria a reviewer should
consider, causal prerequisites, and the participant's workspace capability. They do
not add a workflow language, recursive supervisors, automatic acceptance, or an
implicit merge step.

Validate a definition without creating a session, job, workspace, or provider call:

```sh
tny swarm validate examples/swarm/factory-development.json
tny swarm validate examples/swarm/factory-development.json --json
```

Run it from a native local saved session:

```sh
tny --swarm-file examples/swarm/factory-development.json ask \
  "Develop the bounded change, preserve the evidence, and report remaining risks"
```

The [factory development example](../examples/swarm/factory-development.json)
sequences research, an isolated implementation, and independent review. A clean
participant exit is only execution success. It is not proof that the declared
acceptance criteria passed or that the isolated change was merged.

## Compatibility and shape

Version 1 remains accepted with its existing canonical bytes and semantics. Its
members have only `name` and `purpose`; version-2 fields in a version-1 file are
errors. Version 2 retains the same required group hierarchy:

```json
{
  "version": 2,
  "purpose": "One non-blank root purpose",
  "coordinator": {
    "name": "lead",
    "purpose": "Own final synthesis",
    "deliverable": "A decision with evidence and unresolved risks",
    "acceptance": ["Every claimed check names its command and result"]
  },
  "agents": [],
  "swarms": []
}
```

Every root/group object and actor object is closed, and duplicate JSON fields are
rejected. Root objects require `version`, `purpose`, `coordinator`, `agents`, and
`swarms`; nested groups require `purpose`, `coordinator`, `agents`, and `swarms`.
Every nested group still has a coordinator. Actor names remain globally unique,
including the root coordinator.

`name` and `purpose` remain required for every actor. Version 2 adds these optional
actor fields:

| Field | Contract |
| --- | --- |
| `deliverable` | Non-blank UTF-8 text, at most 4096 encoded bytes. |
| `acceptance` | When present, 1..16 non-blank criteria; each is at most 1024 UTF-8 bytes. The criteria are declarations for reviewers, not automatically executed or proven. |
| `depends_on` | An array of 0..15 globally unique participant names. Names identify causal prerequisites, not message routes or authority. |
| `workspace` | A closed object with `policy` equal to `shared_read_only` or `isolated`. Optional `base` is allowed only with `isolated` and is at most 256 UTF-8 bytes. |

The root coordinator is the current session, not a launched worker. Its
`deliverable` and `acceptance` are retained as the root synthesis contract, but the
root must not declare `depends_on` or `workspace`; those fields are rejected rather
than pretending the session is an indexed worker. An omitted worker workspace means
`shared_read_only`. `depends_on: []` is valid and means no causal prerequisites.

The unchanged whole-manifest limits also apply: at most 64 KiB of input, names at
most 64 UTF-8 bytes, purposes at most 4096 UTF-8 bytes, root depth 1 through maximum
depth 4, and 1..16 launched participants excluding the root coordinator. The schema
describes the portable shape; runtime validation is authoritative for encoded-byte
limits, duplicate keys, global name uniqueness, and dependency semantics.

## Dependencies are readiness, not delegation

Named dependencies compile to the existing durable DAG's task indices. Validation
rejects unknown names, the root coordinator, self-dependencies, repeated names, and
cycles before provider, job, or worktree effects. A participant becomes runnable
only after every direct predecessor has succeeded and its durable result integrity
has been checked. A failed, missing, or corrupt predecessor blocks the consumer; it
is never relabelled as successful.

There are no inferred edges for group membership, coordinator roles, or synthesis.
A nested coordinator may deliberately depend on named participants and therefore
start later. Unrelated ready participants may still run concurrently under the
existing admission cap. Participants may communicate directly through the durable
team mailbox before and while other work runs; dependencies are not a delegate-only
pipeline and do not restrict peer discussion.

After prerequisites succeed, the consumer receives bounded evidence for direct
predecessors only. Each entry identifies the exact participant name, task index,
attempt, durable session, result and log integrity, plus recorded workspace/commit
provenance. A predecessor's final-answer prefix is limited to 2048 bytes and may be
marked truncated; all injected predecessor evidence together is limited to 16384
bytes, and the complete compiled prompt remains under the existing 64 KiB limit.
The precise durable reference and hashes remain available when a summary is
truncated. Aggregate-budget omissions are explicitly listed by task index; a missing
inline summary never silently means an empty predecessor result.

Predecessor output is untrusted task content, not an instruction or new authority.
tny does not inject whole transcripts, evidence from transitive-only predecessors,
or a claim that an isolated predecessor's files reached another checkout. Missing or
changed required evidence fails closed before the consumer provider call.

## Workspace and acceptance boundaries

`shared_read_only` is the default and does not widen the caller's permissions.
`isolated` explicitly asks the existing managed-task workspace service for a Git
worktree. It inherits the caller's provider, permission, tool, sandbox, instruction,
and step ceilings. An inherited read-only caller cannot escape by requesting an
isolated workspace.

Isolated results record the run, task, attempt, base, branch, checkout, HEAD/commit,
status and bounded patch provenance already supplied by
[managed task workspaces](task-workspaces.md). They are retained for explicit
inspection. No participant answer, dependency success, declared criterion, or role
automatically integrates, merges, commits, verifies, or accepts those files.
Integration and external checks remain explicit root/operator decisions.

Acceptance text is useful review context, not an executable test specification. A
reviewer should distinguish observed evidence from an unrun criterion and report
missing evidence honestly. Public status exposes the durable contribution identity,
dependency and workspace/result provenance needed for that inspection; successful
execution continues to report unverified acceptance unless an external process
establishes otherwise.

## Durability, resume, and trust

The canonical version-2 manifest and digest are saved in the session/checkpoint.
Compiled items durably retain contribution fields, resolved dependency identity,
workspace policy, attempts, and result integrity. Resume uses the saved canonical
snapshot rather than rereading a mutable source file. Supplying a file again must
produce the same canonical digest.

Adoption and resume re-check the durable run against that canonical manifest,
including ordered membership, exact contribution contracts, resolved dependency
indices, workspace policy/base, current attempts and recorded result/log integrity.
After a whole job is terminal, isolated workspaces are reopened by confined identity
and their current snapshots must match recorded provenance. A live supervisor may
still be finalizing workspaces; adoption does not imply their acceptance. Missing, changed,
duplicate, or ambiguous state is refused rather than relaunched or guessed. A valid
existing run is adopted without launching duplicate participants.

All compiled `swarm_*` fields and resolved indices are private compiler-owned
metadata. Public job/team requests cannot assert or override them; serialized public
request fields are not trusted as manifest identity. Existing inherited instruction
snapshots remain immutable.

## Platform limits

Execution has the same boundary as purposeful swarms and managed workspaces: native
local saved sessions on supported Darwin and Linux builds. Wasm may validate the
manifest but cannot execute its job/worktree plan. Windows/MSYS, SSH, embedded,
ephemeral, and other contexts without the required native job/watch seams reject
activation before provider effects. `isolated` additionally requires a suitable
local Git repository and is file isolation, not an OS security sandbox.

See [ADR 0159](adr/0159-swarm-contribution-contracts.md),
[team control](team-control.md), and [durable DAG ADR 0143](adr/0143-durable-dag-over-jobs.md).

Purposeful job retry remains unsupported; recovery adopts or resumes the original activation without silently creating a new attempt.
