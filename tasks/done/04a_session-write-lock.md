# 04a — Session writer lock + `--resume` on a running session

Parallel with 04b. Depends on 03 (and 02a's staleness helper).

Be honest about scope: this is a **retrofit, not an add** (ADR decision 7).
No flock exists anywhere in the codebase today — TUI resume, `ask
--resume`, and in-turn compaction all write `session.json` unlocked, so two
concurrent resumes can corrupt a session *today*. `-B` just makes the race
probable. This task introduces locking to every turn-running writer.

## Work

- Lock: `flock(LOCK_EX|LOCK_NB)` on `<dir>/lock` (self-releases on crash —
  no stale-lock cleanup exists, ever). Held for the duration of a turn by:
  the background child (acquired pre-fork, 03), foreground `ask --resume`,
  and TUI resume. Readers never lock — atomic-rename saves make reads safe.
- Liveness probe helper (shared with 02a/04b/04c): non-blocking **shared**
  flock attempt; success while `status:"running"` ⇒ stale. The lock is the
  source of truth; the status field is the durable record.
- `tny ask --resume <id>` (and `tny resume`) on a held lock: error
  `tny: session <id> is still running (pid N)` (pid from `<dir>/pid`),
  exit 1. Stale (`running` + free lock): proceed normally.
- No queueing, no steering — deferred to a future ADR.
- Known limit to document: flock is advisory and unreliable on some
  network filesystems (ADR decision 11).

## Acceptance

- Unit test for lock acquire/contend/release; crash of the holder releases
  (flock) or is detected stale (pidfile).
- Integration: start `-B` against a slow mock, `ask --resume <id>` fails
  with the documented message while running and succeeds after completion.
