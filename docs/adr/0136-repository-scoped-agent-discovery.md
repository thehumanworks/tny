# 0136 — Repository-scoped background-agent discovery

Status: Accepted — 2026-09-18.
Amends ADR 0107's workspace-local dashboard scope.

## Root cause

Sessions are stored under a hash of the canonical workspace path. A session
started with `--worktree` belongs to a different bucket than the original
checkout. The dashboard scanned only the caller's bucket, hiding those agents.
It also required a durable background marker, so a normal foreground
`tny --worktree` session was hidden even while its runner was live.

## Decision

Keep session storage checkout-local. For `agents` only, query the local Git
worktree registry once per refresh with `worktree list --porcelain -z`. Scan the
current workspace and each other registered checkout's session bucket. Skip the
current bucket when Git lists it again. Include live foreground sessions as well
as saved background sessions, using one writer-lock probe per entry. Finished
foreground sessions remain excluded; an existing attached owner still prevents
takeover. Preserve timestamp ordering. Do not scan unrelated repositories or
change `sessions` and `resume last` semantics. No GitHub API or remote is needed.

NUL-delimited records support paths with whitespace, quotes and newlines; ignore
incomplete records. Use the existing bounded argv-only Git host seam. Non-Git,
missing/failed Git, SSH and wasm retain the current-workspace fallback.

Interactive selection loads a fresh context for a different checkout before
opening its session or connecting to its runner. Project settings, permissions,
instructions and subsequent turns must not inherit the dashboard origin's
workspace. Discovery does not acquire the managed-worktree usage lock or take
ownership of merge/remove-on-exit actions.

## Verification

`tests/integration/test_worktree.py` covers managed and manual worktrees,
repository subdirectories, special-character paths, exclusion of unrelated and
finished foreground sessions, live foreground discovery, no duplicate rows,
ordering, non-Git fallback, plain/JSON
output, completed-session selection, and a live detached ACP fixture runner
launched with `--worktree` and attached from the original checkout.
