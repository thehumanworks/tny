# 04b — Observability: live partials, status in `session`/`sessions`

Parallel with 04a. Depends on 02a and 03.

"Read the session" while it runs must show progress, not an empty
transcript.

## Work

- Live partial output: in the background child, periodically checkpoint the
  streamed assistant text with the existing recovery machinery
  (`session_recovery_write`, `src/core/session.c`) — e.g. every N seconds or
  M bytes from `TNY_EV_TEXT_DELTA`. Clear it on successful turn end (the
  final text lands in the transcript). This reuses the exact format
  `--continue-recovery` already reads.
- `tny session <id>` (`src/cli/cmd_sessions.c`): print `status:` including
  `running (pid N)`, `running (stale — pid N is gone)` via the 02a helper,
  and `exit code` when finished. `--json` passes the raw fields through
  (it already dumps the doc — verify the new fields appear).
- `tny sessions` list: mark running sessions (e.g. a `⏵ running` column or
  suffix) in both text and `--json` output.
- Optional, cut if time-boxed: `tny session tail <id>` streaming
  `task.log` + recovery partials. Note in the file if deferred.

## Acceptance

- During a mock background run: `tny session <id>` shows `running` and a
  growing partial; after: `done`, exit code, full transcript.
- Killed child shows the stale marker.
- `docs/features/sessions.md` documents the observable states.
