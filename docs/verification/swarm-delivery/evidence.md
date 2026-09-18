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

## Assembled native flow and further fixes

- Public busy-tool all-tools/terminal fixture first failed `MAILBOX_IO`: the
  adapter had not canonicalized Darwin's trusted `/var` state-root prefix.
  Fixed the prefix, while retaining no-follow checks on job leaves.
- A real race then exposed posting past a contended inbox. Safe-boundary delivery
  now pumps control only for at most 250 ms and fails rather than silently posting
  without queued context. Five repeated two-profile runs passed afterward.
- Live-member `team status` initially rechecked the redacted public projection,
  which intentionally lacks capability verifiers. It now revalidates the confined
  current private record and attempt. Public two-profile tests pass.
- Added actual isolated same-file edits, completed-worker preservation across
  client loss, explicit inspect/integration and real conflict preservation to the
  public fixture. Both profiles pass; usage is 120 known input / 24 output tokens
  with no duplicate completed-worker requests. Verification remains unverified.
- Team control's collected workspace path initially did not match the scheduler
  field. `workspace_cwd` now supplies the execution location. Seven control tests
  pass. The workspace driver now captures a parent identity and cleans inherited
  harness metadata; 13 service/CLI tests pass, including unrelated caller refusal.
- Dedicated self-cancel validates only the current authenticated own item and
  attempt, while legacy member mutations remain refused. Its public fixture also
  proves unrelated live work survives. Initial fixture wiring failed before the
  scenario handler was added; the corrected four-case suite passes (wasm case
  is an explicit native skip).
- `team_runtime_suite`: two tests, 28 assertions, exit 0. It rejects ambient
  session hints, wrong capabilities/run/task/attempt and ambiguous JSON identity.
- Read-only enforcement is now mandatory in the ordinary jobs suite; the temporary
  integration opt-in skip was removed.
- SDK review follow-up found Python callback-close ordering and nested exception
  group retention. Commit 100e01c fixes both with real AsyncSession-wrapper tests,
  Python 3.10/3.14 tests, typing and native SDK conformance. No billing guarantee
  or arbitrary exception-attribute cleanup is claimed.

## Captured-parent integration

Worker session 7879ac88c97e8716 created its own isolated
`test/captured-parent-flow` checkout at `/tmp/tny-captured-parent-flow` rather than
editing the assigned empty `feat/swarm-parent-flow` checkout. Its actual commit
31273bc was inspected and integrated as 43f6a4c; an accidental empty cherry-pick
of the unused checkout's HEAD was skipped without changing work.

The new real public parent fixture passed three times in all-tools and terminal
profiles against a frozen copy of the lead binary (SHA256
995b64eefd30b0433d7d32495766efbfd85d36f7f60c45c7348e064b6e502d54).
A top-level native parent launches two worker-only items, exchanges busy-tool
clarification as mailbox parent -1, waits/collects, inspects/integrates, and runs
an explicit ordinary-terminal check. The saved parent session records the real
check command/cwd/result. `team verify` still refuses and jobs stay unverified.
This is focused evidence; the final combined suite must rerun the new fixture.

The complete native `make -j4 quality` gate passed on the pre-doc-integration
working source. GCC analysis was explicitly skipped on Darwin. Added docs/tests
and subsequent input changes still require final lint/test reconciliation.

## Native gates before main reconciliation

- `make -j4 quality`: exit 0; Darwin explicitly skips GCC analyzer.
- `make -j4 leaks`: exit 0, including the nonfork workspace and team runtime suites.
- `make -j4 test-sdks`: exit 0; Python 103 tests (one installed-wheel skip), native
  Node suite and both conformance adapters passed.
- `make -j4 test-shell-workflows`: exit 0, Bash and Zsh.
- `make -j4 test`: exit 2, only three new fixture entry points failed because
  run.sh appends the binary path: swarm_parent, task_workspace_control and
  team_mailbox. Their argument handling is corrected rather than exempted.
- Isolated `mutate.py --focus swarm-auth --test team_ --fast`: exit 0; one valid
  auth mutant killed; two boolean-mixing mutants did not compile and are not
  counted as kills. A separate compilable removal of the read-only ceiling was
  killed by the real permission assertion (build 0, test 1); source restored.
- Isolated `make test-subagent-mutation test-checkpoint-mutation`: exit 0, including
  step/read-only inheritance and read-only checkpoint recovery mutants.
- Stripped pre-reconciliation candidate: 1,220,784 bytes. Darwin runtime:
  libc++.1.dylib and libSystem.B.dylib. Baseline: 1,121,424 bytes.
- Existing bench_ttft.py, ask-stdin, 5 runs, 400ms fixture delay: baseline median
  886.0ms (849.5ms min), candidate 876.8ms (867.0ms min). These noisy short runs
  do not establish a speedup. Baseline built from 89bcd59 in a separate worktree.

## Independent final risk review and upstream changes

Reviewer d482350e7811142b first found no concrete P0/P1 in the frozen native
adapter snapshot. A targeted follow-up then established the first-party background
terminal lifetime issue; the original verdict is qualified, not silently reused.
The guard now refuses detachment in owned jobs and its synchronous descendants.
The unit test checks both profiles and no terminal-state directory creation.
Public fixtures check ordinary and admitted job workers. Upstream's non-job
terminal completion behavior is retained and will be rerun.

The reviewer inspected pinned MSYS2 3.6.10 source: directory fsync returns success
as a no-op. Our writer promises syscall ordering, not physical storage durability;
no Windows power-loss result is claimed. Native Windows was not tested here.

Fetched main introduced terminal completion (#162), repository agent discovery
(#164), a Responses optional-argument fix, and developer-only Nix/CI changes.
A named stash preserves the pre-merge working changes. The local merge and stash
application preserve both sides; it is not a PR merge or a push to main.
The assembled input revision changed, so prior gates are historical only.
Docker is not running locally and Nix is unavailable. A bounded isolated wasm QA
worker is attempting the CI-pinned emsdk without system-wide installation.

## Merged-main native candidate gates

On the resolved merge of feature HEAD43f6a4c and upstream ab74e2a, with the working
source manifest in the private check log directory:

- `make -j4 quality`: exit 0; GCC analyzer explicitly skipped on Darwin.
- `make -j4 leaks`: exit 0. Upstream terminal-task fork exclusion and our separate
  workspace-process exclusion are retained; nonfork workspace/auth suites run.
- `make -j4 test-sdks`: exit 0, Python 103 tests and native JS/conformance passed.
- `make -j4 test-shell-workflows`: exit 0, Bash and Zsh.
- `make -j4 test`: exit 0, complete unit/protocol/integration suite, including the
  corrected binary-argument fixture entry points and upstream terminal tests.
- Focused public swarm, captured-parent, workspace-control, mailbox and ordinary
  terminal-background suites: all exit 0. Owned jobs reject detached terminal
  starts without canary/log effects; ordinary non-job behavior remains intact.

Isolated wasm QA session c0fc4f568f2afbd1 installed CI-pinned Emscripten 6.0.8 in a
private prefix and built both wasm targets (exit 0). Actual wasm swarm rejection,
jobs read/refusal, OpenAI fixtures and the real browser smoke executed and passed.
Browser initially skipped without Playwright; private installation and rerun
executed all assertions. Wasm size: 1,593,379 bytes. These results are scoped to
the worker's recorded source manifest, not unperformed native Windows tests.

SDK review b739d967df11861c independently closed the callback-drain and exception-
group findings at 100e01c: 31 Python tests and all prior probes passed. SDK-only
branch a3bb3f9 was published as PR #165 after its fresh in-tree SDK, quality,
shell and typing gates. Its hosted checks are separate and were pending at launch.

## Hosted CI correction

PR #166's first musl runs failed six new workspace unit tests at Git fixture
initialization. The Alpine unit-test image did not install Git. This is a test
closure defect, not an allowed baseline failure: Git is added to that image,
the toolchain contract checks the dependency, and fixture failures now print an
actionable init/config diagnostic. No test is skipped. Local policy/actionlint
checks and all seven workspace tests pass; the hosted musl rerun is required.
PR #165's independent SDK/Linux checks were green or still running at observation.
