# 07 — Integration tests + mutation pass

Final gate. Depends on everything prior. Individual tasks carry their unit
tests; this task is the cross-cutting suite and the mutation run.

## Work

- Integration tests (`tests/integration/`, mock provider):
  - **Happy path**: `tny ask -B` exits fast (assert < ~1s wall) printing a
    valid 16-hex id; poll `session.json` until `status:"done"`; assert the
    answer is in the transcript and `task.log` has the tool trace.
  - **JSON shape**: `-B --json` object has `kind`, `session_id`, `pid`.
  - **Result parity**: for a host-backend (stub) run, the stored `result`
    object equals what foreground `--json` prints for the same fixture —
    byte-for-byte modulo timing fields. This is the core contract.
  - **Stale detection**: `kill -9` the child mid-run; lock self-releases;
    `tny session <id>` reports running-but-stale; `--resume` works.
  - **Lock contention**: `--resume` during a live run fails with the
    documented message and exit 1.
  - **Stop**: `tny session stop` mid-stream → `interrupted`, partials
    preserved, spawned stub host dead (group signal), lock free.
    `stop --kill` on a SIGTERM-ignoring stub → terminal status written by
    stop; no orphans.
  - **Steer fidelity per backend** (the 04c obligation): interrupt a live
    turn with `--resume --steer` against native, codex-spawned,
    codex-attached, cursor, and ACP stubs — assert the steered follow-up
    sees prior conversation context and the transcript shows
    partial + interrupt + new prompt. Steer on a wedged child errors with
    the `--kill` suggestion without killing.
  - **Detach hygiene**: child survives parent's terminal/pgroup teardown
    (spawn under a setsid wrapper, kill the group, child completes).
  - **No orphans**: after `done`, no spawned host processes remain
    (pattern from the tny-live-testing checks).
  - Mock transports must split frames at arbitrary boundaries per the
    repo rule if any new parsing was added (none expected).
- Mutation testing (`tests/mutation/mutate.py`) targeted at the new code:
  status transitions in `session.c`, the finalize-on-every-exit-path blocks
  in `cmd_ask.c`, and the lock logic — exactly the code a unit suite covers
  only nominally. Record surviving mutants and either kill them with tests
  or justify them here.
- `make test` green; re-measure stripped binary size (`wc -c`) — the
  feature must not push past the 2.0 MiB invariant; note the number.

## Acceptance

- All new tests in CI; mutation report attached to the PR/ADR; size number
  recorded.
