# 04a — Session writer lock + `--resume` on a running session

Parallel with 04b. Depends on 03 (and 02a's staleness helper).

Two processes mutating one `session.json` will corrupt it. Add a writer
lock; make `--resume` fail cleanly while the background turn runs.

## Work

- Lock: pidfile-style `lock` in the session dir (`O_CREAT|O_EXCL` with the
  pid, or `flock` on a lockfile — pick one; `flock` self-releases on crash,
  which removes the stale-lock case, prefer it). Taken by any process about
  to run a turn against a session (background child in 03, and the normal
  foreground `ask --resume` / TUI resume path).
- `tny ask --resume <id>` (and `tny resume`) on a locked session: error
  `tny: session <id> is still running (pid N)`, exit 1. With a stale
  `running` status but dead pid (02a helper), proceed — the lock is the
  source of truth, the status line is advisory.
- No queueing, no steering — 01 decision 5 defers that to a future ADR.

## Acceptance

- Unit test for lock acquire/contend/release; crash of the holder releases
  (flock) or is detected stale (pidfile).
- Integration: start `-B` against a slow mock, `ask --resume <id>` fails
  with the documented message while running and succeeds after completion.
