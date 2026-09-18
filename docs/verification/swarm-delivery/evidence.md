# Swarm delivery evidence

## Reconciliation

- Base and fetched origin/main: `89bcd5918da0e225e1806813a206d007daafac0a`.
- Initial checkout clean. Renamed supplied branch to `feat/swarm-delivery`;
  retained harness-managed checkout path to preserve session association.
- GitHub authenticated. No open PRs at initial inspection.
- #153, #155–#159 open; no implementation PRs linked in issue comments.
  #154 is an already merged Codex weekly-usage PR, preserved unchanged.
- `tny skill show` unavailable. Loaded installed skill files for tny,
  worktrees, verification-contract and mutation testing instead.
- `mise install`: exit 0. Baseline `mise exec -- make -j6`: exit 0.
  Binary under test is this checkout's `build/tny`, not the installed CLI.
  Baseline stripped Darwin arm64 release: 1,121,424 bytes.

## Ownership and sessions

| Owner | Branch/worktree suffix | Session | Scope |
| --- | --- | --- | --- |
| Lead | feat/swarm-delivery / supplied checkout | current | Integration, registration, ceilings, evidence, publication |
| Design reviewer | shared read-only checkout | 4c7e75fd87034639 | Pre-implementation recovery/identity design |
| Context | feat/swarm-context / tny-swarm-context | 393b519d8a4771a6 | R159 and SDK usage retention |
| Control | feat/swarm-control / tny-swarm-control | 61c0a8c58790e646 | R153/R155 jobs DAG foundation |
| Workspace | feat/swarm-workspaces / tny-swarm-workspaces | 05649b8e7bff2cd5 | R157 managed workspace helper |
| Mailbox | feat/swarm-mailbox / tny-swarm-mailbox | 5f571e4a434865af | R156 durable bounded mailbox helper |
| Admission | feat/swarm-admission / tny-swarm-admission | ecf7e03a5e79e566 | R158 shared permit/queue helper |
| Slice reviewer | shared read-only checkout | 539ba23526d1bb30 | Step inheritance implementation |

The first three editing workers started together. Mailbox and admission workers
were added only after their disjoint helper ownership and existing job identity
interface were specified. All use separate worktrees. Lead alone integrates
scheduler and public-surface glue. A launched session is not accepted work.

## Design review

Session 4c7e75fd87034639: done, exit 0. Found launch-intent transaction risk,
identity-vs-authority ambiguity, retry ceiling/account continuity, separate
acceptance states, attempt fencing and SDK execution-mode gaps. Contract amendment
1 records the response: extend the existing job record/scheduler in place, avoiding
an extra run-to-job binding transaction. All other checks remain required.

## Step inheritance slice (R158 subset)

Input: base plus five files in the initial step-inheritance diff. The launch
snapshot forwards positive effective `max_steps`; zero retains existing semantics.

- `mise exec -- make -j4 test-subagent-ownership test-subagent-mutation`: exit 0.
  All 12 behavioral mutants killed, including omitted step ceiling. Ownership
  suite covers caps 1, 7 and INT_MAX, source mutation and allocation cleanup.
- `mise exec -- make -j4`: exit 0.
- `TNY=$PWD/build/tny mise exec -- python3 tests/integration/test_subagent.py`:
  exit 0. New fixture emits more tool work than the cap and observes exactly
  three actual child provider requests, plus a failed child outcome. Existing
  create/message/inspect/lifecycle, credentials and permission cases pass.
- This does not establish shared admission, run budgets or all of R158.

## Current invariant verdicts

R153/R155/R156/R157/R158/R159/RX/RG/RD remain **in progress**, not verified.
No issue may be closed from helper tests or a successful worker report alone.

## First integration observation (not a release candidate)

- Initial `make quality`: exit 2. clang-tidy read `cli.h` during an in-place
  edit and reported an unterminated comment. The completed header is valid.
  This check is invalidated, not a baseline failure or a passed gate.
- Initial `make leaks`: exit 0 on its then-built subset. Later helper/runtime
  additions invalidate reuse for final acceptance.
- Initial `make test`: exit 2. It ran while source/HEAD changed: missing not-yet-
  integrated mailbox header, stale binary vs new DAG tests, version mismatch,
  and a publication fixture missing native object prerequisites. All must be
  rerun on a frozen assembled candidate; none is relabeled success.
- Review `539ba23526d1bb30`: step inheritance implementation had no production
  defect but lacked resumed-turn enforcement. Added successful creation then
  capped follow-up. The actual new child requests are exactly three; targeted
  subagent integration rerun exit 0.
- Review `de590ef173142905`: four production integration concerns: cancel must
  fence expected attempt; retry needs secret-safe account/endpoint continuity;
  durable acknowledgment needs parent-directory sync; retired attempts must not
  permanently consume mailbox outstanding capacity. Directory sync and mailbox
  retirement fixes are assigned; cancel/account corrections remain a required
  scheduler follow-up after its current integration commit. Review did not approve
  production integration.

## Integrated foundation commits

- f50d83d: worker e351f40, opt-in jobs DAG (80 jobs tests, 3 platform skips).
- afe3cd4: worker 4c7e0d3, managed workspace helper (7 tests, 109 assertions).
- ec75d2b: worker b491083, durable shared admission helper and real-process tests.
- 4f6ce66: worker 08a7108, bounded mailbox helper (15 native/sanitized tests).
- e3496eb: worker 3105192, lazy selective SDK context and available usage.
  SDK/shell/conformance and quality passed in the worker tree. R159 baseline
  regression fails with 32 renders; candidate renders 1. ADR0137 records five-run
  baseline/candidate memory, composed bytes and latency, including latency cost.
These are source integration milestones, not final combined verification.

## Further focused evidence and review

- `make debug test-subagent-ownership test-checkpoint-ownership`: first exit 2
  from an incorrect greatest TEST declaration; fixed. Second run exit 0.
- `build/tny-test -s perm_suite -v`: exit 0, 19 tests / 227 assertions. New
  read-only ceiling precedes all modes, broad rules and session grants.
- `test_swarm_agents.py build/tny -v`: exit 0, three real-job/PTY tests. A first
  invocation put `-v` where test_tui expects the binary; it failed at import,
  then the documented runner argument form passed. Later rerun also passed.
- Updated subagent integration: exit 0 for create and resumed step ceilings.
- Existing subagent/checkpoint ownership mutation gates: exit 0. New read-only
  mutants were added afterward and require another run on final inputs.
- Scoped runtime tidy first failed on implicit strcmp truth checks; explicit
  comparisons fixed. It must be rerun; no blanket suppression was added.
- SDK reviewer `b739d967df11861c` independently reproduced usage lost after
  observed events/cleanup failure, Python non-finite omitted-field acceptance,
  and exception traceback retention of 32 composed prompts. A separate SDK
  hardening worker owns fixes/tests. Nix SDK inputs/dependencies were added by
  the lead; native Nix/wasm tools are unavailable locally, pending supported CI.

## Native adapter checkpoint

The lead's mailbox/notification adapters, run-filtered dashboard and inherited
read-only ceilings now compile. Scoped tidy passes after the explicit strcmp
fix. `make format-check lint-py lint-sh lint-js`: exit 0. Focused interception
permission test: exit 0, 14 assertions. The full busy-tool delivery fixture is
written but intentionally awaits the scheduler's private capability integration;
this checkpoint does not claim it has passed.
