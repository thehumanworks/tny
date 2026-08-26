# 04c — `tny session stop <id>` (+ `--kill`) and `ask --resume --steer`

Parallel with 04a/04b. Depends on 03 (pid file, group leadership) and 01
(decisions 6, 7a). Stop and steer ship together: steer IS the stop
sequence with a resume appended — they share the wait-on-lock machinery.

## Work — stop

- `src/cli/cmd_sessions.c`: new `stop` subcommand.
  - Probe the lock (04a helper). Not held → "not running" (report status),
    exit 0 as a no-op on `done` sessions.
  - Held → read `<dir>/pid`, send `SIGTERM` to the **process group**
    (`kill(-pid, …)`) — the child is a session/group leader after
    `setsid()`, so spawned hosts receive it too. Signal only while the
    probe confirms a live holder (recycled-pgid guard).
  - Wait bounded (poll the lock, ~5s). Lock released → child finalized
    `interrupted`; report it. Timeout → report, suggest `--kill`.
  - `--kill`: SIGKILL the group, then acquire the now-free flock and write
    the terminal status (`interrupted`, note "killed") on the child's
    behalf — the only case where a non-child writes a terminal status.
- Child side (in 03's scope, verify here): SIGTERM handler feeds the
  existing cancel probe (same path as SIGINT in foreground ask) so the
  engine cancels cleanly and finalize runs.
- `--json` output for scripting: `{"kind":"session_stop","status":…}`.
- Factor the stop sequence (probe → signal → bounded wait → outcome) into
  a helper callable from cmd_ask — steer reuses it verbatim.

## Work — steer (`tny ask --resume <id> --steer "…"`)

Interrupt-and-redirect (ADR decision 7a). Bare `--resume` on a running
session still fails — takeover must be explicit.

- In `cmd_ask`: when `--steer` is set and the lock-acquire fails, run the
  stop helper. On clean release, acquire the lock and proceed as a normal
  resume; fold the checkpointed partial into the transcript via the
  `--continue-recovery` machinery so the model sees what it was doing
  before the interrupt. On stop timeout: error out, suggest
  `session stop --kill` — steer never SIGKILLs on its own.
- `--steer` on an unlocked session is a plain resume (no-op modifier);
  `--steer` composes with `-B` (redirect and re-detach: steer sequence in
  the parent, then the normal 03 detach).
- **Per-backend interrupt→resume fidelity is a test obligation, not an
  assumption**: for a spawned codex host the group-SIGTERM kills the host
  too — resuming rides on the host pointer plus codex's own on-disk
  session. Fixture-test native, codex (spawned and attached), cursor, ACP:
  interrupt mid-turn, steer, assert the follow-up sees prior context.
- Document the semantics honestly (06): pending tool calls are abandoned;
  this is "drop that, do this instead", not a live steer. True mid-turn
  steering (no lost work) remains a future ADR.

## Acceptance

- Integration: stop a mock run mid-stream → `status:"interrupted"`, partial
  output preserved, spawned stub host gone, lock free, `--resume` works.
- `stop --kill` on a SIGTERM-ignoring stub → terminal status written by
  stop itself; no orphan processes.
- `stop` on a finished or nonexistent session: clean no-op / error.
- Steer: `ask --resume $id --steer "new direction"` against a live mock →
  old turn interrupted, new turn runs, transcript shows partial + new
  prompt; `-B --steer` variant re-detaches and prints the same id.
- Steer on a wedged (SIGTERM-ignoring) child: errors with the `--kill`
  suggestion; does not kill on its own.
