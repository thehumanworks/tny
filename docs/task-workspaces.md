# Managed task workspaces

This is the native C workspace helper for issue #157. Job submission and public
CLI/tool wiring are separate integration work. This page does not claim that an
unwired command or SDK policy exists.

## Policy and boundary

The job service remains the authorization and execution authority. It must bind
operations to its existing **32-lowercase-hex job ID**, stable zero-based item
index, and positive attempt. It must check membership, permission and active
worker state before each operation. Do not run Git while holding the job state
lock. There is no second run database or scheduler here.

The service should select one of three explicit policies:

- **Isolated Git editing**: default for editing tasks; use this helper.
- **Shared read-only**: the caller's permission/tool policy must prohibit writes.
- **Shared writable**: explicit opt-in only; concurrent file edits can conflict.

A worktree isolates files, **not privileges**. Workers have the same OS identity,
can reach other paths and Git metadata, and are not sandboxed by these helpers.
Private metadata prevents accidental adoption of foreign trees; it is not a
security boundary against a malicious same-user process.

Native local Git repositories work. Non-Git directories and remote SSH URIs fail
before managed filesystem creation. This helper does not provision remote trees:
an SSH job must refuse isolated policy before launch or use an explicitly chosen
shared policy. Native execution *on* a remote machine can call the local helper
there, under that machine's job authority. Wasm returns a clear unsupported error
without Git or filesystem side effects. Host backends must receive the prepared
cwd through their existing launch seam; the helper cannot enforce host tool
permissions.

## C interface and wiring

`src/util/task_workspace.h` is C11 with an `extern "C"` boundary for the jobs owner.

| Operation | Contract |
| --- | --- |
| `task_workspace_prepare(cwd, id, base, &w, err, cap)` | Create a new isolated branch/tree, holding an attempt lock. `base == NULL` requires a clean launch tree. |
| `task_workspace_open(cwd, id, &w, err, cap)` | Reopen only private metadata for this repository and exact identity. Never adopt a caller path. |
| `task_workspace_path(w)` | Borrowed absolute cwd for the admitted worker. |
| `task_workspace_inspect(w, &result, err, cap)` | Persist and return provenance, HEAD, tracked binary patch and porcelain status. |
| `task_workspace_integrate(w, err, cap)` | Explicit merge into recorded launch checkout/branch. 0 means merge staged or already integrated; 1 means conflicts preserved; -1 means refusal/error. |
| `task_workspace_cleanup(w, err, cap)` | Remove only a verified clean owned tree, never its branch or metadata. Repeated removal succeeds. |
| `task_workspace_close(w)` | Release lock and memory only. Never deletes files, including on failure/cancel. |
| `task_workspace_result_free(&result)` | Release snapshot strings. |

Inputs are validated before creation. A null base requires no tracked or untracked
launch changes. An explicit commit-ish acknowledges that launch edits are **not**
included; it resolves to a commit before preparation. There is no implicit stash,
commit, reset, or snapshot. Ignored launch files are not copied. Detached launch
HEAD is allowed for preparation but cannot be an integration destination.

The job service should persist policy, identity, returned cwd and preparation
outcome with the item. On recovery, try `open` for a known preparation, not another
`prepare`. Do not infer membership from workspace metadata. Fence stale attempts
before opening or publishing results. Keep the handle while the worker runs, or
use job-service active-worker checks if it must be reopened in another process.
After the worker stops, call `inspect`, attach the structured result to the job
artifact, and close. Child exit zero or worker prose never triggers integration.
Authorization and validation/check results belong to the job service, not this
Git helper.

The result includes run/task/attempt, branch, base commit, launch path/branch,
workspace path, final HEAD, `patch`, `status`, and `dirty`. The patch includes the
tracked working-tree result relative to base, including committed changes. Status
lists untracked and ignored files, but their contents are **not** in the patch.
Commit intended new files before integration. The Git helper captures bounded
output; inspection refuses any patch/status reaching 16,000 bytes rather than
publishing a truncated artifact. Large results stay in the worktree and branch
for manual inspection. `result.json` is a snapshot, not a verification decision.

## Ownership, collisions and recovery

State lives under the repository's absolute Git common directory:

```text
tny-tasks/<run>-<task>-<attempt>/
  lock           # exclusive advisory attempt lock, close-on-exec
  owner.json     # private provenance and ready/removed state
  result.json    # most recent successful inspection
  tree/          # Git linked worktree
```

The corresponding branch is `tny-task/<run>-<task>-<attempt>`. Directory modes are
0700 and metadata files 0600. Worktree and branch paths are derived, never read
from arbitrary caller paths. A reopened tree must match the recorded Git dir,
common repository, branch and exact top-level path. Symlink state/metadata is
refused. Existing branches, reservation directories and tree paths are never
adopted, even if their names match.

A crash after reservation but before `owner.json` is saved leaves an uncertain
reservation. It is preserved and refused, not automatically resumed or deleted.
Use a new attempt after inspecting the residue. A crash after successful
preparation can reopen safely. If Git removed the tree before the removal marker
was saved, cleanup finalizes the missing tree idempotently. A replacement tree is
never removed. Metadata uses atomic rename; power-loss durability is not promised.
All automatic cleanup is conservative: dirty, untracked **or ignored** files
prevent removal. Branches and snapshots remain even after clean removal, so
unmerged commits are not discarded. No prune, force-remove, branch deletion,
recursive deletion, or cancellation cleanup is performed.

## Explicit integration and checks

Stop workers and concurrent human edits before inspection/integration/cleanup.
Attempt locks serialize helper operations; a repository integration lock also
serializes merges across attempts. These locks do not lock arbitrary editors.
Integration requires the original launch branch in the original repository and
both trees clean, including ignored files. It snapshots the worker, then runs a
no-fast-forward, no-commit merge of that exact revision. It does not run commit
hooks or make an acceptance decision. Git merge drivers remain repository config.

On success, inspect the staged diff, run checks, then explicitly commit. On a
conflict, Git index stages and conflict files remain in the launch checkout; both
worker branches and patches remain unchanged. Resolve manually, or explicitly
abort the merge after inspecting it. The helper never automatically resets or
aborts, including after subprocess failures. A non-conflict Git failure can also
leave merge state: inspect before retrying.

Example workflow (service/API sequence, **not invented CLI syntax**):

1. Fan out read-only review tasks using the existing read-only permission policy.
2. Admit two editing items. Prepare `(job_id, 0, attempt)` and `(job_id, 1, attempt)`
   from a clean launch checkout. Pass each returned path as its worker cwd.
3. Each worker edits `same.txt`, runs its checks and commits its intended result.
   The launch checkout is unchanged.
4. After both stop, inspect each workspace and publish the structured artifacts
   with the job's actual check outcomes. Review the diffs.
5. Explicitly integrate item 0, run repository tests and commit the staged merge.
6. Explicitly integrate item 1. If changes overlap, report the conflict. Preserve
   both branches; resolve it, rerun tests, then commit. Never treat prose as checks.
7. Keep failed/cancelled workspaces for inspection. Remove only clean owned trees
   when authorized; retained branches permit later recovery.

## Focused verification and integration remaining

`tests/test_task_workspace.c` exports `task_workspace_suite` for the existing
Greatest runner, with an optional standalone main. Source discovery already
includes the helper; the lead must register the suite in `tests/test_main.c` and
check Nix packaging. The suite forks: add it to the macOS `LEAK_SUITE_SKIP` list,
like the other process-spawning suites, and cover it with Linux Valgrind. Darwin
`leaks --atExit` stops forked children and deadlocks the two-worker fixture; an
aborted checker can return zero without a report and is not passing evidence.
ASan/UBSan runs the entire standalone suite. No Makefile, Nix, dispatch, jobs or
SDK changes are in this slice.

Standalone check:

```sh
cc -std=c11 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE \
  -DTASK_WORKSPACE_TEST_MAIN -Wall -Wextra -Werror -Wno-deprecated-declarations \
  -Isrc -Ithird_party/yyjson -Ithird_party/greatest \
  tests/test_task_workspace.c src/util/task_workspace.c src/util/git.c \
  src/util/util.c src/util/tny_poll.c src/json/json.c src/util/alloc.c \
  third_party/yyjson/yyjson.c -o /tmp/test-task-workspace
/tmp/test-task-workspace -v
```

Fixtures cover same-path edits, an applyable patch, unchanged launch, real merge
conflict, dirty/untracked launch refusal, explicit base, cancellation, crash
reopen, crash between Git removal and metadata update, repeated cleanup, branch
and path collisions, foreign replacement, symlink metadata, ignored files, and
oversized artifact refusal. They do not prove public CLI/job authorization,
remote execution, wasm runtime behavior, read-only enforcement, or integrated
job/client recovery. Those require lead wiring and end-to-end checks.
