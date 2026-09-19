# tnyboard

A standalone Python 3.9+ standard-library package for local, Git-friendly ticket
boards. No harness import, database, provider call, scheduler or live inference.
Native POSIX only (`fcntl`, `O_NOFOLLOW`, atomic rename and directory fsync).
Windows and wasm do not support local storage/server execution; clients must use
a supported native host. Use SSH to run the CLI remotely. Do not expose HTTP over
a public interface or assume browser access works.

## Run and install

From this repository root:

```sh
python3 -m tnyboard --root "$PWD" --board demo init
python3 -m tnyboard --root "$PWD" --board demo create 'Implement parser' --id parse
python3 -m tnyboard --root "$PWD" --board demo show
python3 -m tnyboard --root "$PWD" --board demo move parse doing 'Start work' --expected-revision 1
python3 -m tnyboard --root "$PWD" --board demo comment parse 'Review requested' --expected-revision 2
python3 -m tnyboard --root "$PWD" --board demo tui
```

`python3 -m pip install ./tnyboard` installs the package and `tnyboard` console
entry point; runtime dependencies are empty. `make -C tnyboard install` is the
standalone equivalent. Build isolation can download setuptools; offline installs
can use `--no-build-isolation` when setuptools is already installed.

`--root` and `--board` precede the subcommand. Defaults: current directory and
`default`. Root must exist and have no symlink path components. On macOS use
`pwd -P`/the physical path when the project is reached through a symlink.
Ergonomic mutation commands default to actor `user`; RPC requires an explicit
actor. `--lead null` disables the lead; `assign ID null --expected-revision N`
clears a ticket owner override. Column arrays use `--columns 'JSON'`.

TUI commands: `inspect ID`, `move ID STATUS REASON`, `comment ID BODY`, `refresh`,
`quit` (also `exit`). Shell-style quoting works. Mutations read a fresh revision
and submit it once; a concurrent change produces a visible conflict, not an
implicit retry. Both streams must be TTYs to enter the prompt loop. Otherwise
`tui` prints one board and exits. `show` is always noninteractive. Rendering uses
ASCII escapes for all controls, non-ASCII characters, bidi and format characters;
wide boards become vertically stacked columns on narrow terminals. ASCII escaping
is deliberate: terminal-safe, deterministic widths without a `wcwidth` dependency.

## State and Git

```
PROJECT/.tnyboard/boards/NAME/
  .gitignore       # ignores runtime lock and atomic-write remnants
  .lock            # stable inode; never delete while any client is active
  board.json       # configuration and board revision
  tickets/ID.json  # whole ticket, history, comments, claim, dispatch
```

Commit `.gitignore`, `board.json`, and ticket files. JSON is UTF-8, sorted-key,
indented, newline-terminated. One document is replaced per successful mutation.
Ticket and board revisions are independent. Ticket history and comments share the
same atomic file as ticket state. No automatic Git commit occurs. Tokens in claims
are cooperative fencing credentials and appear in Git; they are **not** HTTP
secrets. HTTP bearer tokens are never stored by the component.

Transactions use a process `flock` on one stable per-board lock and a Python
thread lock. Reads also lock and validate the entire snapshot. Writes use a
same-directory exclusive temporary file, file fsync, atomic replacement and parent
directory fsync. Directory creation fsyncs its parent. A crash before replace
leaves the previous document and possibly an ignored temporary file; a crash
before an init writes `board.json` can be recovered by repeating init. Init refuses
orphaned ticket files. A reported storage failure after replace/fsync can mean
that the write occurred: read state before taking another action.

The trust boundary is the local filesystem and shared bearer token. IDs cannot
contain slashes, dots, separators or Unicode. Symlink path components, symlink
files, hardlinked state/lock files and special files are rejected. These checks
are not a sandbox against a hostile local user replacing directories during a
transaction. Only component clients may mutate active board files. Manual edits,
Git checkout/merge/rollback, lock deletion, network filesystems with weak flock or
rename semantics, and hostile concurrent filesystem changes are unsupported.
Stop all clients first; preserve schema/history/revision invariants when editing.
Invalid stored state is rejected, never repaired or silently overwritten.

## Coordination

Columns have nullable owners; ticket owner overrides persist across movement.
Effective owner = non-null ticket owner, otherwise current column owner. Lead
(default `board-lead`, nullable) and `user` are cooperative oversight labels.
`user` is an explicit trusted local operator, **not** an authenticated user.
Any caller with file/token access can supply these labels. This is not multi-user
authorization and confers no tny team, workspace or acceptance authority.

Owners or lead/user may move, assign, claim, release and reserve/resolve dispatch.
A worker with a current claim must also supply that claim's actor and token.
Lead/user can intervene without a token. Moves and assignment clear claims; lead
claim takeover replaces the claim. Comments are open to every valid actor even
while claimed. Create is open to any valid actor. Configure is lead/user only,
and refuses active claims or pending/uncertain dispatches to avoid changing owners
under active coordination. Columns cannot be removed while occupied.

Movement has no direction restriction and requires a nonblank reason. Claims do
not expire, terminate a process, or imply execution. Reads and comments do not
launch anything. Comments are durable pull-based coordination, not notifications.
Dispatch is explicit and described in [api.md](api.md). A confirmed job launch is
not task completion, review, or acceptance. Moving a ticket remains explicit.

## Verification

```sh
make -C tnyboard test
make -C tnyboard lint         # Ruff on PATH, or RUFF='uvx ruff@0.14.0'
make -C tnyboard check
```

Tests cover API, CLI subprocesses, line-oriented TUI, sanitization, malformed
stored/input JSON, ownership, claims, stale revisions, dispatch reconciliation,
thread/process races, authenticated loopback HTTP, malformed requests and durable
state after errors. No live inference. Tests use only the standard library.
See [evidence.md](evidence.md) for the verified implementation run.
