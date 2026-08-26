# 07 — Integration tests + mutation pass

Final gate. Depends on everything prior. Individual tasks carry their unit
tests; this task is the cross-cutting suite and the mutation run.

## Work

- Integration tests (`tests/integration/`, mock provider):
  - **Happy path**: `tny ask -B` exits fast (assert < ~1s wall) printing a
    valid 16-hex id; poll `session.json` until `status:"done"`; assert the
    answer is in the transcript and `task.log` has the tool trace.
  - **JSON shape**: `-B --json` object has `kind`, `session_id`, `pid`.
  - **Stale pid**: kill the child mid-run; `tny session <id>` reports
    running-but-stale; lock is released (flock) so `--resume` works.
  - **Lock contention**: `--resume` during a live run fails with the
    documented message and exit 1.
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
