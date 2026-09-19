# ADR 0155: Optional local branch cleanup after worktree removal

Status: accepted

## Context

Users remove completed or accidental worktrees but may want either to keep
commits for later re-entry or to delete the local branch. Directory removal
and branch deletion are separate decisions. A published PR does not prove
that commits are merged, and Git ancestry cannot recognize all squash merges.

## Decision

Keep the existing non-force directory removal. Only after it succeeds, ask
whether to delete the local branch, with keep as the default. Try `git branch
-d` first. If Git refuses, show the error and require a second explicit yes
before `git branch -D`. Never delete remote branches. Preserve Git's refusal
to delete branches checked out elsewhere. Report partial completion honestly.

Retain the existing creation behavior: reuse `worktree/NAME` when it exists,
without resetting it or ignoring another checkout's ownership. Do not add
GitHub API calls, PR-status detection, or automatic branch deletion. Native
CLI and slash-command worktrees share this exit flow; noninteractive commands
keep worktrees and branches. Wasm keeps its clean unsupported-platform error.

## Verification

`tests/integration/test_worktree.py` uses real Git and PTYs for default keep,
unused branch deletion, unmerged refusal/override, recreation with preserved
commits, and branch ownership refusal. Existing tests cover dirty files,
locks, changed branches, runner shutdown, and noninteractive retention.
