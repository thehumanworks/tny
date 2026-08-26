# 02a — Session status fields + early save

Parallel with 02b. Depends on 01 (decisions 1, 4).

Give sessions a runtime status so a background task is observable from the
moment the parent prints its id.

## Work

- `src/core/session.c` / `session.h`:
  - New top-level fields in `session.json`: `status`
    (`running|done|error|interrupted`), `pid` (int, present only while
    running), `exit_code` (int, present once finished). Old sessions simply
    lack them — readers must treat absence as "not a background task".
  - Setters: `session_set_status_running(s, pid)`,
    `session_set_status_finished(s, status, exit_code)` (or one setter — keep
    it minimal). Finishing clears `pid`.
  - Ensure `session_save()` on a fresh `session_new()` creates the directory
    and writes a valid `session.json` before any turn ran (today nothing
    touches disk until the first save; verify the create path handles an
    empty transcript).
- Staleness helper for readers: given a session with `status:"running"` and a
  `pid`, `kill(pid, 0)` failing with ESRCH ⇒ report as crashed/stale (used by
  04b and 04a).
- Update `docs/features/sessions.md` and the session JSON schema (see the
  recent "schemas" commit) with the new fields.

## Acceptance

- Unit tests: new session saved-before-turn is openable; status transitions
  running→done and running→error round-trip through save/open; absent fields
  on legacy sessions don't crash listing or `tny session`.
