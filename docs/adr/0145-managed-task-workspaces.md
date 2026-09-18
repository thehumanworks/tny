# ADR 0145: Managed task workspaces with explicit integration

Status: Accepted helper design; job/CLI integration pending

## Context

Issue #157 requires file isolation for editing workers without a competing job
controller. The shared identity is the existing job ID (32 lowercase hex), stable
item index and positive attempt. Generic named worktrees can reuse existing
branches and paths; that behavior is useful interactively but insufficient proof
of ownership for task cleanup.

The independent delivery design review was completed before this implementation
(verification contract amendment 1, reviewer `4c7e75fd87034639`). It requires
consumer fencing by attempt and Git outside the job state lock.

## Decision

Add a C11 `task_workspace` OS-seam helper. Reuse `git_run`, the bounded argv-only
Git subprocess helper also used by `worktree.c`. Do not call `worktree_enter`:
it deliberately adopts existing named trees/branches. Do not change its existing
interactive behavior.

Derive a private repository-local reservation and branch from job/task/attempt.
Never adopt existing branches, directories, uncertain reservations or foreign
linked trees. Record base commit, launch path/branch, linked Git directory and
identity in private metadata. Check exact tree, branch and repository identity
before destructive operations. Metadata establishes local file ownership, not
job membership or permission. The job service authorizes every call.

Prepare only from a clean launch checkout unless the caller supplies an explicit
base commit choice. Never silently stash, snapshot or exclude dirty/untracked
launch edits. The default editing policy at job integration is isolated Git;
shared writable requires opt-in, and shared read-only requires existing tool and
permission enforcement. Worktrees are not a privilege sandbox.

Inspection returns and saves a structured revision, tracked binary patch and
status snapshot. Untracked/ignored file contents are not patch artifacts; commit
intended additions. Refuse artifacts exceeding the existing bounded Git capture
rather than reporting truncated output as complete.

Integration is explicit, to the recorded clean launch branch only, with both
trees quiescent and clean. Snapshot the worker revision, then merge it without
automatic commit. Preserve conflicts and both worker branches. Never infer
acceptance from exit status, artifact hashes or prose. Serialize integrations
with a repository lock. Existing Git merge behavior retains its config semantics.

Closing and cancellation never delete files. Cleanup verifies ownership and a
clean tree, including ignored files, uses Git removal without force and retains
branches and metadata. Repeated cleanup, including a crash after Git removal,
is safe. An interrupted preparation without final metadata stays quarantined;
retry with a new attempt, not guessed adoption. Atomic metadata replacement is
not a power-loss durability guarantee.

Non-Git and remote SSH provisioning fail before creation. Wasm returns an explicit
unsupported error. Remote native use needs the remote job authority to invoke
this same local helper. See [task workspaces](../task-workspaces.md) for API,
wiring, recovery and tests.

## Consequences

This slice is a real tested helper, not a public swarm command or job controller.
The lead must wire policy, membership, attempt fencing, cwd propagation, result
publication, validation and explicit integration/cleanup authorization. It must
register the test suite and verify packaging/platform checks. Existing job state
remains the only execution authority.

Conservative refusal leaves recoverable residue instead of risking user work.
This costs disk space and requires manual review of uncertain reservations and
large patches. Git/file locks do not stop arbitrary same-user writers; callers
must stop workers and concurrent human edits before final operations. Private
metadata does not protect against malicious same-user filesystem mutation.

## Integrated delivery addendum

The scheduler and public `task-workspace` CLI/typed/terminal adapters are wired.
Real provider workers edit identical relative paths in separate trees while the
launch checkout stays unchanged. Explicit integration records a preserved real
conflict; captured-parent tests collect, inspect/integrate and run a caller-
configured ordinary terminal check. The first-party detached-terminal API is
refused in owned job descendants, so it cannot silently outlive workspace
inspection or permit release. This is not containment of arbitrary shell daemons.
