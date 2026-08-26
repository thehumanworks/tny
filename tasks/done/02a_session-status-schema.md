# 02a — Session status fields + early save

Parallel with 02b. Depends on 01 (decisions 1, 4).

Give sessions a runtime status so a background task is observable from the
moment the parent prints its id.

## Work

- `src/core/session.c` / `session.h`:
  - New top-level fields in `session.json`: `status`
    (`running|done|error|interrupted`), `exit_code` (int, present once
    finished), and `result` (object — the exact JSON foreground
    `ask --json` would have printed; ADR decision 3, closes the
    host-backend gap where only a resume pointer persists). The pid lives
    in a separate `<dir>/pid` file (control channel for `stop`), NOT in
    the doc — keeps the child the sole doc writer. Old sessions simply
    lack all of this — readers treat absence as "not a background task".
  - Setters: `session_set_status_running(s)`,
    `session_set_status_finished(s, status, exit_code, result_json)`.
  - Ensure `session_save()` on a fresh `session_new()` creates the directory
    and writes a valid `session.json` before any turn ran (today nothing
    touches disk until the first save; verify the create path handles an
    empty transcript).
- Staleness helper for readers (ADR decision 5): probe with a non-blocking
  **shared flock** on `<dir>/lock`. Probe succeeds while `status:"running"`
  ⇒ crashed/stale. Immune to PID reuse; no `kill(pid,0)`. Used by 04a, 04b,
  04c.
- Update `docs/features/sessions.md` and the session JSON schema (see the
  recent "schemas" commit) with the new fields.

## Acceptance

- Unit tests: new session saved-before-turn is openable; status transitions
  running→done and running→error round-trip through save/open; absent fields
  on legacy sessions don't crash listing or `tny session`.
