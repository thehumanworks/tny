# 02b — `--background` / `-B` flag parsing and preconditions

Parallel with 02a. Depends on 01 (decisions 2, 7).

Pure argv/UX work in `src/cli/cmd_ask.c` — no detach yet. Behind the flag,
run the turn in the foreground for now (03 adds the fork) so this lands
independently testable.

## Work

- Add `--background` and `-B` to the flag loop in `cmd_ask()`. Note: this is
  the first short flag in ask's parser — match it exactly (`-B`), do not
  invent a general short-flag mechanism.
- Precondition errors (message + example, matching the existing style):
  - `-B` + `--ephemeral` → error (decision 7).
  - `-B` with no prompt behaves as today (prompt is read before detach).
- Output shape plumbing (decision 2): in background mode the process that
  faces the user prints either the bare session id or the
  `{"kind":"ask_background","session_id":…,"pid":…}` JSON object; the normal
  result JSON at the bottom of `cmd_ask` must NOT also print in the parent.
  Structure the function so the "print id and stop" exit point is clean for
  03 to fork at.
- `--stdin` + `-B`: stdin is drained fully before the id is printed. The
  connect-overlap thread (`cmd_ask.c:282`) must be skipped in background mode
  — the fork in 03 must precede any `pthread_create`.

## Acceptance

- `tny ask -B "hi"` (pre-03 stub) runs the turn and prints only the session
  id on stdout; `--json` variant prints the object.
- Error cases exit 1 with the documented messages.
- `tny ask --help` untouched here (06 owns help text).
