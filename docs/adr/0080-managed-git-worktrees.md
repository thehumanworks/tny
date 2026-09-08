# ADR 0080 — Managed Git worktrees

Status: accepted, 2026-09-08.

## Context

Agent work needs an independent checkout without changing the invoking
workspace or requiring manual branch and directory setup. Reusing a global
name must preserve its history, and closing the shell must preserve work
unless the user explicitly selects a lifecycle action.

## Decision

1. `--worktree [NAME]` is a leading global flag; `/worktree [NAME]` is an
   idle TUI workspace switch. Both use the same C11 host OS service and the
   installed Git CLI, invoked through `posix_spawnp` with argv, bounded
   output, no stdin, and a timeout. Ambient `GIT_*` process overrides are
   removed so they cannot redirect the selected repository or index.
   Ordinary filesystem Git configuration is still honored.
2. Use `~/.tny/worktrees/NAME`, with a random identifier by default. Validate
   names as single path components. Create `worktree/NAME` from HEAD, or
   reuse that branch without resetting it. Existing directories must resolve
   to a linked worktree in the same common Git directory.
3. Record the original root and original branch ref in
   `tny-worktree.json` inside the private Git directory. Reusing a directory
   reads this metadata rather than selecting a new merge destination. Capture
   the current worktree branch on entry and recheck it before exit actions. A
   private advisory `tny-use.lock` prevents concurrent tny entry; forked
   runners inherit it until shutdown, while exec'd tools do not.
4. A slash switch ends the old runner/session, reloads context at the new
   root, clears workspace completion and queued inputs, and starts a fresh
   session. Previously visited worktrees remain. The CLI performs entry
   before loading config; help/version fast paths do not run Git.
5. Only interactive TUI exit asks for merge/remove/keep, after the backend
   and runner stop. Keep is the default on rejection, EOF or interruption.
   If shutdown cannot be confirmed, keep without running a Git mutation.
6. Merge requires the recorded branches and repository identities to still
   match, plus clean checkouts and no unfinished Git operations. Use ordinary
   `git merge --ff --no-edit --no-autostash --no-overwrite-ignore`; never
   auto-commit or stash, or overwrite ignored files in the original checkout. Keep
   the worktree on both success and conflict. Removal requires a clean
   worktree including ignored files and uses non-force `git worktree remove`.
   Never delete its branch. These checks do not lock out external Git/editor
   processes; users must stop concurrent external edits before acting.
7. Local-only: SSH combinations are rejected. The existing host OS seam
   provides a clean wasm error without adding a new platform boundary or
   linked library. Git is an optional runtime executable, required only
   when worktree mode is requested.

## Verification

`tests/test_worktree.c` checks optional flag parsing and name boundaries.
`tests/integration/test_worktree.py` uses temporary Git repositories, the
shared PTY harness and a loopback mock provider to verify actual branches,
checkout reuse, workspace tool execution, safe default exit, merge outcomes,
removal guards and concurrency. The Nix test environment includes Git.
The wasm OpenAI fixture suite checks the explicit unsupported error.
Targeted mutations exercise name-length and dirty-checkout guards.

## References

- [git-worktree](https://git-scm.com/docs/git-worktree): linked checkout,
  branch reuse, locking and non-force removal.
- [git-merge](https://git-scm.com/docs/git-merge): clean-checkout preconditions,
  fast-forward/divergent histories and conflict recovery.
- [Worktree usage](../worktrees.md).
