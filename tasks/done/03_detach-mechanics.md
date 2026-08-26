# 03 — Detach: fork, setsid, redirect, finalize status

Depends on 02a (status fields, early save) and 02b (flag + exit point).
The core new code of the feature.

## Work

In `src/cli/cmd_ask.c`, background path only:

The ordering below is the **one correct construction** (ADR decision 4) —
do not reorder it:

1. Read the prompt fully (argv or drained stdin) — stdin does not exist
   after detach.
2. `session_new()` (or `session_open` for `--resume`), then **acquire
   `flock(LOCK_EX|LOCK_NB)`** on `<dir>/lock` (04a helper). Acquire failure
   on `--resume` → `session <id> is still running` error, *before* forking.
3. `session_save()` with `status:"running"` — the single parent save; the
   id must be findable on disk before the parent exits.
4. `fflush(NULL)`, then `fork()` **before any pthread_create** (the
   connect-overlap thread is skipped per 02b; audit for other thread
   creation — fork must come first). Without the flush the child replays
   the parent's buffered stdio into `task.log`.
   - Parent: print id (or JSON), `_exit(0)`. Single fork suffices — the
     parent exits immediately, the child reparents to init. The parent's
     fd copy closes on exit but the lock lives on the **open file
     description the child inherited** — it stays held.
   - Child — the sole writer from here on: `setsid()` (survives terminal
     close and pgroup kills; makes the child a group leader, which is what
     lets 04c's `stop` group-signal spawned hosts), stdin ←
     `/dev/null`, stdout+stderr → `<dir>/task.log`, write `<dir>/pid`.
     Readers seeing `running` with no pid file yet treat it as "starting"
     (milliseconds-wide window).
5. Child runs the existing turn body unchanged (connect, engine, event
   loop). Install SIGTERM feeding the existing cancel probe (same path as
   foreground SIGINT) so 04c's `stop` cancels cleanly.
6. On every exit path (success, connect failure, engine error, OOM paths),
   finalize: `session_set_status_finished()` with `done|error|interrupted`,
   the exit code, and the **result object** — the same JSON `--json` would
   have printed, built from the already-accumulated `st.output` /
   tool-call buffers, so it works for host backends too (ADR decision 3).
   Audit the ~6 early-return cleanup blocks in `cmd_ask` — each needs the
   finalize in background mode. Then release the lock (close the fd).
6. `tny_settings_remember_use()` from the child writes settings
   concurrently with whatever the user does next — verify that write is
   atomic (rename) or skip it in background mode.

## Acceptance

- `tny ask -B "…"` returns in milliseconds with the id; the answer appears
  in the session transcript and `task.log` afterwards.
- `kill -9` the child mid-run: flock self-releases; readers (04b) see
  `running` + free lock and report it stale.
- No orphaned backend hosts after the child exits (existing
  `abort_backend`/destroy paths still run).
