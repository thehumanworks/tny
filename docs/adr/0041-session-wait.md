# 0041 — `tny session <id> --wait`: block on the writer lock until a background turn finishes

Date: 2026-08-28
Status: accepted (extends 0031 background ask)

## Context

[0031](0031-background-ask.md) made `tny ask -B` detach and left readers to
poll `tny session <id>` (or `session.json`) until `status` leaves
`running`. Every script that fans out several `-B` turns rewrote the same
sleep loop, and each loop re-derived the liveness rule (lock probe, not
`kill(pid,0)`) or got it wrong by watching the pid.

## Decision

`tny session <id> --wait` blocks until the session's writer lock is free,
then prints the session exactly as the plain / `--json` inspect does.

1. **Liveness = the 0031 lock probe.** The wait loop is a non-blocking
   shared-flock attempt every 200 ms through `tny_poll` (the one blocking
   seam, [0017](0017-wasm-browser-parity.md)). No IPC into the child, no pid
   watching, immune to PID reuse; a crashed writer frees the lock and the
   wait returns at once with the stale view.
2. **Exit code = the turn's `exit_code`** (0 `done`, 2 `error`, 130
   `interrupted`, same mapping as foreground `tny ask`), 2 for a stale run
   without a stored `exit_code`, **124** when `--timeout SECS` elapses
   (`timeout(1)` convention; `--timeout` implies `--wait`). A session that
   is not running is a plain inspect with the same mapping, so
   `tny session $id --wait` is idempotent.
3. **Output is unchanged.** `--wait` adds no new document shape; `--json`
   still dumps `session.json`, so `--wait --json | jq -r .result.output` is
   the one-liner for reading a background answer.

## Consequences

- `tny session` gains `--wait` and `--timeout SECS`; the parent `-B` exit
  code stays launch-only (0031 decision 2) — turn outcome is read via
  `--wait`.
- Waiting many sessions is a shell loop over ids; a multi-id form is not
  added until a caller needs more than sequential waits give.
