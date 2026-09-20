# Team completion event waits

Date: 2026-09-20. Worktree: `feat/swarm-event-waits`.

`team wait-any` now opens the existing `jobs_host` run-directory watch before
its first completion snapshot. Each notification is only a hint: the operation
drains, reads canonical jobs status, drains status-created events, confirms the
private record still matches, and rechecks authority plus the captured attempt.
Quiet positive waits block in `watch_next`; its 50 ms slices check cancellation
without rereading durable state. A final deadline snapshot preserves abandoned-
owner projection because releasing an advisory owner lock does not notify the
directory. Watch open/loss errors fail explicitly, and positive waits refuse
hosts without directory-watch support; status and zero-time waits stay
nonblocking.

New `tests/integration/test_team_control_wait.py` relinks the real team driver
with test-only counters outside the watched directory. It covers initial/final
snapshot counts, a notification between subscription and the first snapshot,
an update after the post-status drain, completion wakeup, cancellation,
attempt-fence change, self-generated events, watch loss, abandoned-owner
projection at the deadline, unsupported positive waits, and nonblocking
status/zero-time waits.

## Local evidence

- `make release`: exit 0.
- Instrumented wait-driver build: exit 0.
- Handcrafted held-record smoke: exit 124 with trace `ODSDNDSD` (one initial
  and one final status snapshot despite a status-created watch event).
- `python3 -m py_compile tests/integration/test_team_control_wait.py`: exit 0.
- Ruff check/format, `clang-format --dry-run --Werror src/core/team_control.c`,
  and `git diff --check`: exit 0.
- `python3 tests/integration/test_team_control_wait.py -v` and the existing
  `test_team_control.py`: blocked before fixture execution because this managed
  sandbox rejects localhost `bind(2)` with `EPERM`; no live inference ran.
- `make test-unit`: built the sanitizer suite, then failed in existing
  socket-binding tests for the same `EPERM` restriction (net 1 failure, HTTP
  server 7 failures, Grok login 3 failures) and aborted on entering the OpenAI
  suite. Completed non-socket suites, including `team_runtime`, passed.

No runner or Nix wiring edit is needed: `tests/integration/run.sh` already
discovers `test_*.py`, and `nix/source.nix` already includes the complete
`tests/` tree. Outside this assigned file scope, the primary should narrow the
generic native support wording in `docs/team-control.md` and ADR 0148: positive
waits now intentionally require the Darwin/Linux directory-watch seam, while
status and zero-time observation remain available on other native job hosts.
