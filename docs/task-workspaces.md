# Managed task workspaces

This is the native C workspace helper and shared explicit workspace-control
service for issue #157. The CLI adapter and typed-tool adapter are implemented;
command dispatch, tool registry/schema, terminal interception and scheduler
preparation remain lead integration work. Tests exercise the real C service and
CLI adapter through a temporary test-only driver, not an invented public command.
The installed `tny` binary must not advertise the command until dispatch is wired.

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
Greatest runner, with an optional standalone main. Register both
`task_workspace_suite` (six nonfork cases) and `task_workspace_process_suite`
(overlap, conflict and real process loss) in `tests/test_main.c`. Add **only**
`task_workspace_process_suite` to macOS `LEAK_SUITE_SKIP`. Keep the six ordinary
cases in the native leak gate. Linux/Valgrind and ASan/UBSan must run both suites.

Darwin `leaks --atExit` stops forked children and deadlocks the process fixture;
an aborted checker can return zero without a report and is not passing evidence.
The split was checked with `leaks --atExit -- /tmp/test-task-workspace-split -s
task_workspace_suite`: all six cases completed and the tool reported zero leaks.
The nonfork recovery case still tests reopening dirty state and the crash window
between Git removal and metadata update; the process suite retains actual abrupt
child exit without closing its workspace handle. No process coverage was removed.

Source discovery already includes the helper and service. Lead registration and
Nix packaging checks remain. The control-service slice does not alter jobs,
helpers, Makefile, Nix, command dispatch, interception or SDK implementation.

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
oversized artifact refusal. Public dispatch/interception, remote execution, wasm
runtime behavior, read-only enforcement, and integrated scheduler/client recovery
still require lead wiring and end-to-end checks.

## Shared explicit control service

`src/core/tools_workspace.c/.h` supplies the shared grammar, canonical permission
detail, record validation and run operation. `src/cli/cmd_task_workspace.c`
implements `cmd_task_workspace(ctx, globals, argc, argv)` with its own permission
check. There is **no public prepare operation**, automatic integration, automatic
cleanup, or accepted-completion transition.

The CLI grammar for lead dispatch is:

```text
tny task-workspace inspect|integrate|cleanup --run ID --task N --attempt N [--json]
```

Flags are mandatory, integer values are bounded, duplicate/unknown flags are
refused, and no stdin, `--cwd`, session ID, or arbitrary worktree path is accepted
by this grammar. The existing trusted global cwd supplies caller context.
The corresponding typed tools accept exactly `{run, task, attempt}`:

| Tool / permission identity | Operation |
| --- | --- |
| `job_workspace_inspect` | Inspect and publish the existing workspace snapshot. |
| `job_workspace_integrate` | Explicitly merge the committed result, without committing the merge. |
| `job_workspace_cleanup` | Explicitly remove a clean owned tree; retain its branch and metadata. |

`tny_workspace_op_parse`, `tny_workspace_parse_argv`,
`tny_workspace_permission_tool`, `tny_workspace_detail`, `tny_workspace_run`, and
`tny_workspace_render_human` are the shared service API. `tool_workspace_op`,
`tool_workspace_available`, and `tool_workspace_run` adapt typed tools.

The run functions are **trusted, already-permitted entry points**, like the jobs
service. For typed tools and terminal interception, the lead must resolve the
exact operation, compute `tny_workspace_detail`, invoke the permission callback
using `tny_workspace_permission_tool(op)`, and call run only after that exact
grant. Read/status/inspect grants never authorize integration or removal. Do not
route terminal syntax around this check or infer permission from a session ID.
The CLI adapter uses the actual permission engine and refuses unresolved grants.
Lead dispatch must avoid provider startup for this standalone command and add
the command declaration/help/registry entries. C++ consumers include the header
through its public C ABI.

### Authoritative association and concurrency

Every operation first rejects SSH, embedded and non-native execution. The service
then confines itself to `<ctx.tny_dir>/jobs/<validated-run>/job.json`. It requires
private real directories and regular private record/lock files; symlinks,
multiply-linked files, missing owner locks and over-limit records are refused.
It does not create a replacement owner lock or trust a PID.

It acquires the **existing `owner.lock` nonblocking**, then calls the real
`tny_jobs_run(..., TNY_JOBS_OP_STATUS, ...)` and rereads the bounded private record.
Both views must agree on identity, attempt, revision, terminal state and launch
workspace. The owner lock stays held through Git and prevents retry/removal from
reusing the attempt. No state lock is held across Git. All items must be terminal,
root cleanup must be exactly `complete`, and `cleanup_hold` must be boolean false.
A task that exited while its job/siblings are active is refused. Unknown cleanup,
including abandoned supervisor state, never becomes cleanup authority here.
Failed/cancelled jobs with complete cleanup may still be inspected or explicitly
controlled; nothing marks their work accepted.

The authoritative schema is the DAG record introduced by `e351f40`:

```json
{
  "version": 1,
  "kind": "job",
  "id": "0123456789abcdef0123456789abcdef",
  "run_id": "0123456789abcdef0123456789abcdef",
  "job_kind": "ask",
  "dag": true,
  "attempt": 1,
  "revision": 3,
  "state": "succeeded",
  "workspace": "/absolute/launch/cwd",
  "cleanup": "complete",
  "cleanup_hold": false,
  "items": [{
    "index": 0,
    "task_id": 0,
    "attempt": 1,
    "state": "succeeded",
    "log_path": "/private/job/attempt-1-item-0.log",
    "request": {"workspace": {"policy": "isolated", "base": "optional commit-ish"}}
  }]
}
```

The scheduler **must persist** isolated association as either
`item.workspace.policy == "isolated"` or, when `item.workspace` is absent,
`item.request.workspace.policy == "isolated"`. A present non-isolated or malformed
`item.workspace` is not overridden by request fallback. The bare `e351f40` DAG
commit does not yet persist that policy: its records are deliberately refused
until scheduler wiring supplies it. With private request persistence disabled,
the separate `item.workspace` object is required. Do not silently default missing
policy to isolated or accept shared policy. Optional base and helper provenance
remain scheduler/helper data, never caller-supplied control paths.

Root `workspace` is the launch cwd. Both root and selected item attempt must match
the request; carried results retaining an older item attempt are refused by this
interface. The service opens the helper by launch cwd and run/task/attempt only.
For mutation, canonical current caller cwd must equal canonical recorded launch
cwd. An existing helper tree must also belong to that launch Git checkout, not
another linked checkout in the same common repository. Launch subdirectories are
supported. Repeated cleanup of an already absent, owned tree remains idempotent.

Results contain `kind:task_workspace`, identity, operation, status, conflict flag,
and, for inspection/integration, the helper's structured provenance/revision/patch
and worker dirty status. Every result says `verification:unverified` and
`accepted:false`, even after a successful merge. Status can be `inspected`,
`integrated`, `conflict`, `removed`, or `refused`. Return codes are 0 for completed
operations, 1 for validation/refusal, and 2 for Git operation failure/conflict.
No raw job record, prompt, credentials, session data or Git stderr is emitted.
Patches intentionally contain the reviewed workspace content and are not secret
redaction. Conflict state stays in the launch checkout and both worker diffs
remain recoverable. Run verification separately before any acceptance decision.

### Service/CLI-adapter verification

```sh
make release
python3 tests/integration/test_task_workspace_control.py -v
# Same service and CLI-adapter fixtures with ASan/UBSan objects:
make debug
TNY_WORKSPACE_TEST_OBJECTS=build/dbg TNY_WORKSPACE_TEST_SANITIZE=1 \
  python3 tests/integration/test_task_workspace_control.py -v
```

The test builds a temporary driver against the actual release objects. It invokes
the real CLI adapter and typed service, the actual permission engine, the actual
jobs status/retry service, real private record fixtures and actual Git worktrees.
A clearly test-only fixture entry invokes helper preparation in place of the
pending scheduler. It is not linked into shipped command dispatch.

Coverage includes CLI/API result parity, patch provenance, explicit merge and
real conflicts, idempotent cleanup, dirty preservation, wrong attempt and cwd,
shared/missing policy, active siblings, unknown cleanup and cleanup holds,
separate read/mutation grants, SSH/embedded refusal, private record confinement,
foreign trees, and strict grammar with no public prepare. A blocked real Git
subprocess proves the owner lock remains held, a real retry is fenced, and
`state.lock` is available throughout Git. A Git sentinel proves preflight
refusals do not invoke Git. Tests check that raw fixture secrets never escape.

These tests establish service and adapter behavior, **not installed CLI/tool
registry or terminal-interception completion**. After wiring, the lead must run
the same public flows through the shipped CLI/tools and the scheduler-generated
records, register these fixtures in the integration runner/Nix inputs, and run
full job/session, platform and leak gates.
