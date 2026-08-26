# 03 — Detach: fork, setsid, redirect, finalize status

Depends on 02a (status fields, early save) and 02b (flag + exit point).
The core new code of the feature.

## Work

In `src/cli/cmd_ask.c`, background path only:

1. Read the prompt fully (argv or drained stdin) — stdin does not exist
   after detach.
2. `session_new()` (or `session_open` for `--resume`), set
   `status:"running"`, `session_save()` — the id must be findable on disk
   before the parent exits (01 decision 4).
3. `fork()` **before any pthread_create** (the connect-overlap thread is
   already skipped per 02b; audit for other thread creation, e.g. inside
   backend connect — fork must come first).
   - Parent: print id (or JSON), `_exit(0)`. Single fork is enough — the
     parent exits immediately, so the child reparents to init; no double
     fork needed.
   - Child: `setsid()` (survives terminal close and the TUI's process-group
     kills), reopen stdin from `/dev/null`, redirect stdout and stderr to
     `<session-dir>/task.log` (append, line-buffered enough for tailing).
     Record the child pid into the session (the pid saved pre-fork is the
     parent's — either save after fork in the child, or fork first and save
     in the child before signaling the parent; pick one and keep the
     "id findable before parent exits" guarantee, e.g. parent waits on a
     pipe byte from the child post-save).
4. Child runs the existing turn body unchanged (connect, engine, event
   loop). SIGINT handler is irrelevant in the child (no terminal); SIGHUP
   should be ignored or handled as interrupt → status `interrupted`.
5. On every exit path (success, connect failure, engine error, OOM paths),
   finalize: `session_set_status_finished()` with `done|error|interrupted`
   and the exit code, then `session_save()`. Audit the ~6 early-return
   cleanup blocks in `cmd_ask` — each needs the finalize in background mode.
6. `tny_settings_remember_use()` from the child writes settings
   concurrently with whatever the user does next — verify that write is
   atomic (rename) or skip it in background mode.

## Acceptance

- `tny ask -B "…"` returns in milliseconds with the id; the answer appears
  in the session transcript and `task.log` afterwards.
- `kill -9` the child mid-run: session stays `running` with a dead pid —
  readers (04b) report it stale.
- No orphaned backend hosts after the child exits (existing
  `abort_backend`/destroy paths still run).
