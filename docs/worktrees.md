# Git worktrees

Use a separate checkout for an agent session while preserving the files in
your current checkout:

```sh
tny --worktree                # random name
tny --worktree fix-parser     # create or enter a named worktree
tny --cwd /path/to/repo --worktree fix-parser
tny --worktree fix-parser ask "fix the parser and run its tests"
tny --worktree ask "inspect this checkout"  # random worktree, one-shot request
tny --worktree=status         # a name that is also a tny command
```

Inside the TUI, `/worktree [NAME]` does the same thing and starts a fresh
session in the selected checkout. Finish or interrupt an active turn first.
The provider, model, effort and permission mode are retained. Project
settings, tasks, instructions, tools, file completion and session storage
use the new workspace. Switching again keeps the previous worktree; the
exit choice applies to the currently selected worktree.

## Creation and reuse

- Worktrees live globally at `~/.tny/worktrees/NAME`; names are shared across
  repositories. The default name is a random 16-character identifier.
- Names contain 1–80 ASCII letters, digits, hyphens or underscores, beginning
  with a letter or digit. Paths, spaces and option-like names are rejected.
- The source is `--cwd`, or the current workspace, including a subdirectory
  or an existing linked checkout. Git must be installed, the source must
  belong to a working repository, and HEAD must name a commit. Bare and
  unborn repositories fail with an explanation.
- A new worktree gets branch `worktree/NAME` at the source HEAD. If that
  branch already exists (for example after removing its directory), Git
  checks it out without resetting it. A branch checked out elsewhere fails.
  Source changes are neither copied, committed nor stashed.
- An existing name enters that directory only when it is a linked worktree
  of the same repository on a branch. A directory belonging to another
  repository, an ordinary directory, or a symlink produces an error.
- The original checkout and source branch are recorded in the worktree's
  private Git directory, outside tracked files. Re-entry preserves this
  merge destination. A detached source HEAD can create a worktree, but its
  eventual merge needs a manually chosen destination.
- Concurrent tny use of the same worktree is refused. A detached session
  runner retains the usage lock until it stops. Git and other editors do
  not participate in this lock.

## Exiting the TUI

After stopping the session, tny asks:

```text
On exit: [m]erge, [r]emove directory (keep branch), [K]eep (default):
```

- **Keep:** Enter, `k`, `n`, Esc, Ctrl-C/D, EOF, or any other answer keeps
  the directory and branch. Non-terminal input and abrupt termination also
  keep them. Noninteractive commands, including `ask`, always keep them.
- **Merge:** merge committed changes into the recorded original checkout
  and branch. Both checkouts must be clean, with no unfinished Git operation.
  Uncommitted changes must be committed by the user or agent before merging.
  The worktree and branch remain after a successful merge. Divergent histories
  can create a merge commit. On conflict, the original checkout contains the
  conflict for manual resolution or `git merge --abort`; the worktree remains.
  A moved, detached or switched original checkout is never guessed or reset.
  Ignored files in the original checkout are not overwritten by a merge.
- **Remove:** run non-force `git worktree remove`, preserving the branch
  and its commits. Tracked changes, untracked or ignored files, Git locks,
  unfinished operations, or a changed worktree branch prevent removal.
  A failed requested action keeps the worktree and exits nonzero.

`--ephemeral` still creates a persistent Git worktree when explicitly
combined with `--worktree`; it only disables conversation artifacts.
Worktree mode is local and cannot be combined with `--ssh` or `/ssh TARGET`.
The wasm builds return a clean unsupported-platform error for both spellings.

See [ADR 0080](adr/0080-managed-git-worktrees.md).
