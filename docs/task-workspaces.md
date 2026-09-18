# Managed task workspaces

Native jobs prepare declared isolated workspaces before worker launch and record
the resolved cwd with the item. Public inspection, integration and cleanup are
available through `task-workspace`, all-tools native tools and the terminal
adapter. They share the job's authority and attempt fences; they do not create a
second scheduler. See [team control](team-control.md).

## Choose a policy explicitly

DAG ask items default to **shared read-only**:

```json
{"role":"worker","prompt":"Review only; report evidence.","workspace":{"policy":"shared_read_only"}}
```

The native worker tool policy enforces read-only access. Merely writing “read
only” in a prompt is not the enforcement mechanism. For editing, opt in:

```json
{"role":"worker","prompt":"Implement the assigned change, check it, and commit intended files in this worktree.","workspace":{"policy":"isolated"}}
```

`shared_writable` is a separate explicit opt-in; concurrent edits there can race.
Prefer isolated Git editing for independent implementation tasks. An inherited
read-only worker cannot request a writable workspace to escape its policy.
Worktrees isolate files, **not privileges**: same-user processes can reach other
paths and Git metadata. They are not a security sandbox.

Isolated preparation requires a local Git repository. With no `base`, the launch
checkout must be clean, including untracked files. An explicit
`workspace:{"policy":"isolated","base":"COMMIT"}` selects a commit and
acknowledges that launch edits are excluded. There is no implicit stash, reset,
commit or snapshot. Ignored launch files are not copied. Preparation can start
from detached HEAD, but integration needs the original named launch branch.
Wasm, SSH and embedded mutation are unsupported; non-Git isolated work refuses.

## Public operations

After the job and its owned processes have stopped, replace `RUN_ID`, task and
attempt with recorded values:

```sh
tny task-workspace inspect --run RUN_ID --task 0 --attempt 1 --json
tny task-workspace integrate --run RUN_ID --task 0 --attempt 1 --json
tny task-workspace cleanup --run RUN_ID --task 0 --attempt 1 --json
```

These are separate decisions, not a command sequence to run without review.
In the all-tools profile the corresponding tools are `job_workspace_inspect`,
`job_workspace_integrate`, and `job_workspace_cleanup`, each with:

```json
{"run":"RUN_ID","task":0,"attempt":1}
```

Terminal-profile agents use direct `tny task-workspace ...` calls. The trusted
adapter retains caller identity; a shell wrapper does not grant it. No operation
accepts an arbitrary worktree path as authority. The job service checks run,
member/parent identity, attempt, permissions and active ownership before Git work.
An indexed lead does not acquire parent authority from its role. Do not inspect
or integrate from inside a still-running member and wait for your own whole job.
A completion event may arrive before supervisor finalization; an active-owner
refusal means wait boundedly and retry, not force removal.

## Inspect, integrate, then check

Inspection returns provenance (run/task/attempt, branch, base, launch checkout,
workspace and HEAD), `patch`, porcelain `status`, `dirty`, and `accepted:false`.
The tracked binary patch is relative to the base and includes committed changes.
Status lists untracked/ignored files, but their contents are not in the patch.
Commit intended new files before integration. Inspection refuses patch/status
output reaching 16,000 bytes rather than publishing a truncated artifact. Large
results remain in the worktree/branch for manual inspection.

Integration is **explicit**, never triggered by worker prose, child exit zero,
collection or a role label. Stop workers and concurrent human edits. The service
requires the original launch repository/branch and clean worker and launch trees,
including ignored files. It snapshots the worker and performs a no-fast-forward,
no-commit merge of that revision. Repository merge drivers still apply.

A successful response reports `status:"integrated"`, not acceptance. Inspect the
staged diff. Run a caller-configured check using ordinary terminal in the launch
checkout, recording command, cwd, exit and output in the parent session. Then
commit only if explicitly authorized. `team verify` execution is unsupported;
it refuses and creates no fake evidence. Even a successful manual check does not
turn the team job's `verification:"unverified"` into an accepted state.

A conflict returns exit 2 and `status:"conflict"`, with `accepted:false`. The
launch index and conflict files remain; both worker branches/worktrees remain.
Inspect and resolve manually, or explicitly abort the merge after review. The
service never automatically aborts, resets or discards conflict state. Other Git
failures can also leave merge state; inspect before retrying.

## Preservation and cleanup

Private state lives under the absolute Git common directory:

```text
tny-tasks/<run>-<task>-<attempt>/
  lock
  owner.json
  result.json
  tree/
```

Branches are `tny-task/<run>-<task>-<attempt>`. Metadata and exact repository,
branch and top-level path must match on reopen. Foreign trees, symlinks and name
collisions refuse; a matching name alone does not authorize adoption. Attempt
locks serialize operations; an integration lock serializes merges across attempts.
These locks do not stop arbitrary editors.

Closing a handle, failure and cancellation do not delete worker edits. Dirty,
untracked **or ignored** files prevent cleanup. Explicit cleanup removes only a
verified clean owned tree. It preserves branches and metadata, including unmerged
commits, and is idempotent after successful removal. There is no automatic prune,
force removal, recursive deletion or branch deletion. Retained editing attempts
can block replay/retry: inspect them and use a new explicit job rather than
silently overwriting their work.

A crash during reservation can leave uncertain metadata, which is preserved and
refused rather than automatically adopted. Inspect residue before choosing a new
attempt. `result.json` is a bounded snapshot, not verification evidence or an
acceptance decision. See the
[review/implement/manual-check template](../examples/swarm/review-implement.md)
and [ADR 0145](adr/0145-managed-task-workspaces.md).
