# 01 — ADR: background one-shots (`tny ask -B`)

Blocks everything else. Write `docs/adr/0031-background-ask.md` recording the
execution model and the three open design decisions, so the implementation
tasks have a settled contract.

## Decisions to record

1. **ID = session ID, not a separate UUID.** The 16-hex session id from
   `gen_id()` exists before the backend connects and the whole read/resume
   surface (`valid_session_id`, `~/.tny/sessions/<ws-hash>/<id>`, `--resume`,
   `tny session`) already keys on it. `-B` prints that id.
2. **Output shape.** Plain mode: bare id + newline on stdout, nothing else.
   `--json`: `{"kind":"ask_background","session_id":"…","pid":N}`. The turn
   result is not available at print time by definition.
3. **Detach model.** Single `fork()` before any pthread is created, child
   `setsid()`, stdin → `/dev/null`, stdout+stderr → `<session-dir>/task.log`.
   Parent exits 0 immediately. Child updates session status on exit
   (`done|error|interrupted`, `exit_code`); readers detect a crashed child via
   `kill(pid, 0)` staleness.
4. **Session lifecycle.** Session is saved to disk with `status:"running"` and
   the child pid *before* the parent prints the id, so `tny session <id>`
   never says "not found" for a live background task.
5. **Messaging semantics (v1).** Follow-ups are `tny ask --resume <id>` after
   completion. While running, the session is write-locked and `--resume`
   fails with `session <id> is still running (pid N)` — no queueing. Live
   steering via a control socket is explicitly deferred to a future ADR
   (relates to ADR-0011).
6. **Codex host registry policy.** Decide and record: background children
   that spawn a codex host do NOT write `~/.tny/codex-host.json` (an
   invisible process should not become the attach target for foreground
   TUIs). Attach (to someone else's host) stays allowed; if the host dies
   mid-run the error lands in the session status, not silently.
7. **Incompatible flags.** `-B` rejects `--ephemeral` (id would point at
   nothing) and `--stdin`-less empty prompts as usual. `--continue-recovery`
   stays allowed.
8. **wasm behavior**: clean error (no `fork` in wasm), per ADR-0017 parity
   contract.

## Acceptance

- ADR merged in `docs/adr/`, linked from `docs/adr/README.md` if that file
  indexes ADRs.
- Every downstream task (02–07) can point at a numbered decision here.
