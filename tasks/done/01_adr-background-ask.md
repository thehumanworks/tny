# 01 — ADR: background one-shots (`tny ask -B`)

Blocks everything else. Write `docs/adr/0031-background-ask.md` recording the
execution model. Core principle, stated up top in the ADR:

> **`-B` runs the identical ask turn and defers its output into the session
> instead of stdout.** The stored `result` is byte-for-byte the object that
> foreground `tny ask --json` would have printed.

Three artifacts with one job each:

| Artifact | Job |
|---|---|
| `flock` on `<dir>/lock` | **truth** — liveness + single-writer, self-releasing |
| `<dir>/pid` | **control** — signal target for `tny session stop` |
| `session.json` | **record** — `status`, `exit_code`, `result`, transcript |

## Decisions to record

1. **ID = session ID, not a separate UUID.** The 16-hex id from `gen_id()`
   exists before any backend work; the whole read/resume surface already
   keys on it.
2. **Output shape.** Plain: bare id on stdout. `--json`:
   `{"kind":"ask_background","session_id":…,"pid":…}`. Parent exit code is
   about the *launch* only (0 launched, 1 precondition failure); turn
   failures surface exclusively via `status`/`result`.
3. **Result persistence (all backends).** `cmd_ask` already accumulates the
   answer from `TNY_EV_TEXT_DELTA` for every backend — that is how `--json`
   works for codex/cursor today. At finalize the child stores that same
   JSON object as top-level `result`. This closes the host-backend gap
   (hosts persist only a resume pointer in the transcript, `runtime.c`
   `finalize_turn`): `tny session $id --json | jq .result` is always the
   answer.
4. **Detach + handshake (the one correct ordering).** Parent: read prompt →
   open/create session → **acquire `flock(LOCK_EX|LOCK_NB)`** →
   `session_save` (`status:"running"`, no pid) → `fflush(NULL)` → `fork()`
   → print id → exit. Child inherits the open file description, so the
   lock survives the parent exiting; child is the **sole writer** from then
   on: `setsid()`, stdio → `/dev/null` + `task.log`, write `<dir>/pid`,
   run the turn, finalize. No second parent save, no pipe handshake.
   Readers seeing `running` with no pid file treat it as "starting"
   (a milliseconds-wide window).
5. **Liveness = lock probe, not `kill(pid,0)`.** A reader that can take a
   non-blocking shared flock while `status:"running"` has found a crashed
   task → report **stale**. Immune to PID reuse; zero pid bookkeeping for
   correctness. `flock` self-release on crash also means no stale-lock
   cleanup, ever.
6. **Cancellation: `tny session stop <id>` (+ `--kill`).** `setsid()` makes
   the child a process-group leader, so `stop` signals the **group**
   (`kill(-pid, SIGTERM)`): the child cancels via the existing probe path
   and finalizes `status:"interrupted"`; spawned hosts die with the group —
   the same mechanism that detached the child also guarantees no orphans.
   `--kill` sends SIGKILL to the group, then acquires the now-free lock and
   writes the terminal status on the child's behalf. `stop` only signals
   while the lock probe confirms a live holder (guards recycled pgids).
7. **The lock is a retrofit to every turn-running writer** — background
   child, `ask --resume`, TUI resume. Two concurrent resumes can corrupt a
   session **today**; `-B` just makes the race probable. Half of task 04a
   is fixing that latent bug. Bare `--resume` on a held lock fails:
   `session <id> is still running (pid N)` — a guardrail, because taking
   over must be explicit (decision 7a), not a silent side effect of a
   badly-timed poll.

7a. **`--resume --steer` (v1): interrupt-and-redirect.** Explicit takeover
   of a running session: run the stop sequence (group-SIGTERM → bounded
   wait for the flock to free; on timeout report and suggest
   `session stop --kill`), then resume with the new prompt, folding in the
   checkpointed partial via the `--continue-recovery` machinery. Composes
   with `-B` (`ask -B --resume $id --steer "…"` = redirect and re-detach).
   Name it honestly in the ADR: this **abandons in-flight tool work** —
   the model re-plans from a transcript ending in the interrupt. A *true*
   mid-turn steer (text injected into the live turn, no lost work — the
   engine already does this for the TUI, ADR-0011) needs an IPC channel
   into the detached child and stays a future ADR. Steer requires
   per-backend proof that interrupt-then-resume preserves the host
   conversation (spawned codex host dies with the group; the resume
   pointer + codex's own on-disk session must carry it) — fixture-tested,
   not assumed.
8. **Codex host registry.** Background children that spawn a host do NOT
   write `~/.tny/codex-host.json` (an invisible process must not become a
   foreground attach target). Attach stays allowed; an attached host dying
   mid-run finalizes `status:"error"` with the reason recorded.
9. **Incompatible flags.** `-B` rejects `--ephemeral` (id would point at
   nothing). `-B --resume` composes (backgrounds a follow-up turn; the
   pre-fork flock serializes it). `--continue-recovery` allowed.
10. **wasm**: clean error (no `fork`), per ADR-0017; CI-enforced.
11. **Known limits (record, don't fix):** flock is advisory and unreliable
    on some network filesystems; `--timeout` is deferred (stop covers the
    hang case manually); recovery-hint display in `tny session` must
    branch on `status` so a live run doesn't advertise
    `--continue-recovery`; steer is interrupt-and-redirect, not a live
    steer (see 7a) — lost in-flight work is documented behavior, not a bug.

## Acceptance

- ADR merged; every downstream task (02–07) points at a numbered decision.
