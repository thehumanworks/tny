# ADR 0040: `--ssh` loads remote `AGENTS.md`, not the launch directory

## Status

Accepted (2026-08-28). Amends [ADR 0022](0022-ssh-execution-boundary.md).

## Context

`--ssh` keeps tny local and runs every workspace tool on the remote host
(ADR 0022). The system preamble already says the local machine is not the
workspace. The `AGENTS.md` / `CLAUDE.md` chain did not follow: it still
walked `$HOME/.tny/`, launch-dir ancestors, and `--cwd`.

That is confusing when the two trees disagree. A typical case is ssh'ing
into another checkout (or a bare VM) from a machine whose current
directory is itself a tny/fx/cursor repo: the model receives *this*
project's `AGENTS.md` ("C11 only", "do not start a TUI framework", …)
while `list_files` / `terminal` operate on the remote tree.

Dropping project instructions entirely over `--ssh` would lose the remote
repo's own `AGENTS.md`, which is the file that actually describes the
workspace the tools will touch.

## Decision

When `ctx->ssh_host` is set:

1. **Still load** `$HOME/.tny/AGENTS.md` (user-global policy; tny stays
   local). Prefix it with an explicit *User instructions* banner that
   names the local path and warns that workspace tools run on the remote
   host — paths in that file are not the remote cwd.
2. **Do not load** launch-dir / ancestor `AGENTS.md` (including `--cwd`).
   Those files describe the local tree, which is not the tool workspace.
3. **Do load** `AGENTS.md`, or `CLAUDE.md` if that is absent, from the
   resolved remote cwd (`ctx->ssh_cwd`), via one `cat` over the existing
   ControlMaster. Prefix it with a *Remote project instructions* banner
   that names `user@host:/dir` and states that tny itself is local.
   Missing is silent; a failed hop does not wipe the rest of the chain.
   Truncated or timed-out cats are dropped rather than half-injected.
4. `instructions_refresh` runs at the end of `ssh_connect` and
   `ssh_disconnect`, so `/ssh` attach/detach and `--ssh` pick up the
   right snapshot before the next turn. `context: false` still disables
   the whole chain. libtny (`library_mode`) is unchanged: it never
   imports home/ancestors and has no `--ssh`.

Narrower-wins still holds: user `~/.tny` first, remote cwd last.

## Consequences

- The native-loop preamble over `--ssh` tells the model both the remote
  cwd *and* which instruction files are in play.
- Remote `AGENTS.md` is untrusted data from the hop, same as any other
  tool output; it is still injected as instructions because that is the
  project's own file. Users who do not want it use `context: false`.
- One extra `cat` at connect (and on `/ssh` attach). Bound 128 KiB.
- wasm: `--ssh` already fails at connect; this path is never reached.
- Coverage: `instructions_follow_the_remote_workspace` in
  `tests/test_ssh.c` (fake `ssh`, local vs remote vs `~/.tny` files,
  disconnect restores the launch-dir chain).
