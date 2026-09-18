# ADR 0148: Thin asynchronous team controls; fail-closed verification gap

Status: accepted control slice; #153 remains partially delivered

## Context and decision

Issue #153 needs a shared public control surface, not another scheduler. Build
`core/team_control.c` over the existing jobs API. `cmd_team.c` only adapts bounded
JSON input and output; tools and terminal interception use the same parser,
permission-detail and run functions with a captured caller identity. Production
registration and notifications belong to the integration lead.

Run ID = job ID. Task ID = item index. The job attempt fences operations; item
attempts bind results and completion cursors, including carried results. Existing
jobs remain the sole persistent lifecycle authority. Start explicitly requires
`dag:true`, ask work, one lead and at least two workers, and returns the existing
durable handle after acknowledgment, without waiting for provider completion.
No role label creates submitting-parent authority. No request session ID creates
membership. Start captures parent lineage from the runtime adapter, separately
from request JSON. Operators keep same-user CLI authority; members must match
captured run/task/session/job-attempt/item-attempt identity. Members can inspect
run metadata but can collect, wait for and cancel only their own task. Parent/
operator controls remain distinct. Nested CLI invocation refuses in favor of
the captured-context terminal adapter. This is not a same-user shell sandbox.

`wait-any` is a bounded external-client fallback (0–30 seconds), returning an
unseen terminal item and its item/attempt cursor. Ties use definition order.
124 means only that the wait ended. 130 interrupts observation, not the durable
job. Native turns should receive the separately implemented safe-boundary
notifications rather than poll on model turns.

`collect` reads bounded, confined job logs and the authoritative final session
answer. It checks stored hashes, retains execution/result identity, and encodes
bounded result-prefix/log-tail bytes as base64 with explicit truncation metadata.
It does not trust log prose, concatenate whole transcripts, accept arbitrary
session arguments, or convert integrity into acceptance. Requested workspace
policy is never a resolved path: use authoritative launch cwd or resolved item
workspace metadata, and refuse ambiguous policy-only metadata.

`cancel` requires `expected_attempt` and forwards it to core jobs. The service
preflight does not replace the scheduler's required atomic transaction check.
The scheduler owner's in-progress fence fix is an integration prerequisite;
this branch cannot claim the cancel/retry race is fixed by a preliminary read.

## Verification is not implemented as a false success

Execution, verification and integration are separate concepts. Team envelopes
explicitly report `unverified` and `not_recorded`. The separate `team_verify`
permission identity includes exact caller-chosen command/cwd/run/item/attempt/
timeout and the captured caller. Read/status grants cannot authorize execution.
No command is inferred from worker prose.

This slice **refuses verification execution** with `TEAM_VERIFY_UNSUPPORTED`.
It has no pretend check records, accepted state or successful verification test.
The existing general shell tool returns text/exit information, not the strict
check-tree cleanup proof required for acceptance. The retained jobs process-scope
API explicitly accepts only the trusted tny executable, not arbitrary check
shells. Reusing either seam as if it proved full check cleanup would be unsafe.
A new check-worker/host seam and its dispatch/cleanup ownership need coordinated
integration, outside this slice's allowed files.

Remaining check service requirements: nonblocking job-owner locking across the
check/retry fence; refusal of active/cleanup-unknown jobs; resolved workspace and
clean, exact before/after revision; authoritative task result hash; explicit
command and timestamps; bounded actual output with hash and observed exit;
private sidecar publication; failure, interruption, client loss and uncertain
cleanup remain failed/unverified. No Git work is done under the jobs state lock.
Separate workspace integration continues to own Git merge/cleanup. No durable
DAG or verification persistence layer is invented here.

## Capabilities and evidence

Native local CLI/service only. SSH, embedded/libtny, ephemeral and wasm contexts
refuse before execution. Host providers keep their own loops. Existing jobs,
SDK workflows and synchronous subagents remain unchanged. The prerequisite
scheduler still requires a per-item provider to match the resolved job provider;
heterogeneous provider teams are not claimed by this slice.

Prerequisites cherry-picked into the context worktree: `e351f40` (DAG jobs) and
`4c7e0d3` (workspace helpers). This slice changes only new team-control service,
CLI adapter, integration test and documentation files.

`python3 tests/integration/test_team_control.py -v` passes seven tests with a
temporary driver linked against the real release main/jobs objects. The fixture
imports the deterministic loopback provider from `test_jobs.py`. Tests prove
responsive launch/client exit, overlapping workers, cursor/timeout semantics,
worker failure, cancellation sparing an unrelated run, captured membership,
stale-attempt refusal, bounded authoritative collection/hash mismatch, context
refusals and distinct permission identities. The verification test proves a
caller-chosen command **does not execute** and work remains unverified.

Earlier failures found and corrected: macOS `/var` state-prefix alias rejected
by confined reads (canonicalize only the trusted root, never a record leaf);
NULL metadata passed to the non-null JSON escape helper (emit explicit null);
and a fixture overlap assertion racing the second worker's admission (wait for
the independent provider counter with a deadline). No failure was called a pass.

Focused C strict warnings and clang static analyzer pass. clang-tidy passes with
the macOS SDK sysroot; the first invocation without that sysroot failed to find
stdio.h and is not evidence of code success. Root full gates, registration,
notifications, atomic cancel fencing, real verification execution and integrated
acceptance/recovery remain lead-owned incomplete work.

## Commands and observed results

Run from the repository root on Darwin arm64:

```sh
# Builds the native release and a temporary CLI driver; seven focused tests.
python3 tests/integration/test_team_control.py -v
# Existing run.sh discovery also passes its executable as a positional argument.
python3 tests/integration/test_team_control.py "$PWD/build/tny" -v
# Existing scheduler API compatibility: two real detached DAG fixtures.
python3 tests/integration/test_jobs.py \
  JobsDAG.test_forward_dependencies_and_unverified_lineage \
  JobsDAG.test_barrier_cancel_and_explicit_retry_carries_a_once -v
clang-format --dry-run --Werror \
  src/core/team_control.c src/core/team_control.h src/cli/cmd_team.c
ruff check tests/integration/test_team_control.py
ruff format --check tests/integration/test_team_control.py
clang-tidy src/core/team_control.c src/cli/cmd_team.c -- \
  -std=c11 -Iinclude -Isrc -Ithird_party/yyjson -Ithird_party \
  -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -isysroot "$(xcrun --show-sdk-path)"
cc -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
  -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wwrite-strings -Wvla \
  -Iinclude -Isrc -Ithird_party -Ithird_party/yyjson \
  -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -fsyntax-only \
  src/core/team_control.c src/cli/cmd_team.c
clang --analyze -std=c11 -Iinclude -Isrc -Ithird_party -Ithird_party/yyjson \
  -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Xanalyzer -analyzer-output=text \
  src/core/team_control.c src/cli/cmd_team.c
```

All final commands above exited 0. clang-tidy suppressed warnings from vendored
headers and reported no first-party findings. No live provider call was made.
The initial integration runs exited 1 for the corrected failures listed above.
Ruff also found an import-order error and a later formatting difference; both
were corrected before the final passing checks. Full `make test`, `make quality`,
`make leaks`, Nix, release-size and platform gates were not claimed in this
focused slice. No successful real check-command execution/provenance test exists:
that capability is explicitly unavailable, not passed or skipped as successful.
