# ADR 0159: Yolo and writable defaults for every agent

Status: accepted. Supersedes the read-only workspace defaults in ADRs 0145,
0157 and the default reviewer guidance in ADR 0158.

## Decision

All user-facing agents default to yolo permissions, including team workers and
file-defined swarm participants and nested coordinators. An omitted DAG workspace
policy resolves to `shared_writable`, not `shared_read_only`. Purposeful swarms
use that same default. Their guidance allows implementation and executable checks;
the root coordinates ownership, integration and verification instead of reserving
all edits to itself.

Read-only access is an explicit override (`workspace.policy: shared_read_only` in
team/job items), never the implicit policy. Preserve explicit ask/auto modes,
inherited read-only ceilings, permission enforcement and capability-aligned tool
filtering for explicitly restricted participants. Existing saved job policies are
not rewritten. File-defined swarm v1 still has no per-participant workspace field;
ordinary teams support explicit read-only or isolated workspaces.

## Consequences and verification

Shared-checkout edits can race. Assign distinct file ownership or explicitly use
isolated worktrees. Neither mode is an OS sandbox. Admission limits, peer control
boundaries and recursive-launch restrictions are unchanged. Platform support is
unchanged: native local teams work; unsupported wasm swarm launches still fail
cleanly before effects.

Mock-provider integration tests check default worker writes, absence of the
read-only child marker, explicit read-only write denial, full writable tool
advertisement, and actual writes by every purposeful swarm participant, including
the nested coordinator. No live inference is needed.

## Local verification

On Darwin, against parent revision `c223bfee944fe878dae0d27c3d2d5f4994e379b1`
plus this change:

- Release build, `make quality`, and `make leaks` passed. The quality gate
  explicitly skips GCC `-fanalyzer` on Darwin; Linux CI owns that check.
- Jobs integration: 144 tests, 141 passed and 3 skipped. Purposeful swarm
  integration: all 7 passed. A focused rerun of default writes, absent read-only
  marker, explicit read-only denial and nested swarm writes passed all 4 tests.
- The three default-access regression tests all fail against a separately built
  pre-change binary, confirming that the tests reject the old read-only default.
- The full `make test` run completed with exit 2. Its 521-test unit suite passed,
  but `test_background_agents`, `test_collective_cap`, `test_collective_swarm`,
  and `test_tui` failed. Each failure was reproduced against the separately built
  parent revision: saved-session inspection times out; two mailbox cases return
  `MAILBOX_DENIED` instead of their expected completion outcomes; the TUI menu
  overlay loses its expected banner. These are not changed here.
- Stripped native release: 1,170,416 bytes. Runtime dependencies are system
  `libc++.1.dylib` and `libSystem.B.dylib`. This is a measurement, not a size or
  performance improvement claim.
