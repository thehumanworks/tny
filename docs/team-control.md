# Team control over durable jobs

Status: implemented service and CLI adapter; **registration is integration-owned**.
This slice provides real asynchronous controls, not full issue #153 acceptance.
Verification execution remains unavailable and fails closed. See
[ADR 0141](adr/0141-team-control-over-jobs.md).

## Identity and ownership

A team run **is** an existing `dag:true` ask job. Its ID is the job ID, a task is
the stable zero-based item index, and the job attempt fences controls. Each item
also retains its execution attempt; a carried result keeps its old item attempt.
There is no second team record, controller, provider loop or recovery authority.
The jobs supervisor retains admission, cancellation, cleanup, persistence and
retry ownership. Existing jobs and synchronous subagent APIs are unchanged.

Start requires one explicitly declared `role:"lead"` and at least two explicit
`role:"worker"` items. Roles describe members; a lead label does **not** grant the
submitting parent's authority. The submitting process can exit immediately after
receiving the handle. Existing job records retain identities after that exit.

## Public grammar

All operations take a bounded JSON object and return JSON:

```text
tny team start    --request FILE|- [--json]
tny team status   --request FILE|- [--json]
tny team collect  --request FILE|- [--json]
tny team wait-any --request FILE|- [--json]
tny team cancel   --request FILE|- [--json]
tny team verify   --request FILE|- [--json]
```

`--json` is optional because this entry always emits JSON. Request input is at
most 1 MiB, must have unique object keys, and cannot contain embedded NULs. An
explicit file does not consume stdin. There is no implicit template or default
worker prompt. `start` uses the existing jobs batch JSON:

```sh
cat <<'JSON' | tny --provider openai team start --request -
{"kind":"ask","dag":true,"concurrency":3,"items":[
  {"role":"lead","label":"scope","prompt":"Read only: identify the review scope. Do not edit files."},
  {"role":"worker","label":"reliability","prompt":"Read only: review reliability. Do not edit files."},
  {"role":"worker","label":"tests","prompt":"Read only: review test coverage. Do not edit files."}
]}
JSON
```

These roots can overlap; use `depends_on:[INDEX,...]` for explicit ordering.
Read-only prose is not a sandbox. Use the scheduler's permission/workspace policy
when available; this service neither weakens nor invents it. Task model/effort
and other launch settings use the jobs validator. In the prerequisite scheduler
revision, a per-item provider must equal the job's resolved provider; cross-
provider teams are **not** claimed. Credentials are not added to team records.

The immediate response is `kind:"team"`, `run_id`, `job` (the canonical jobs
response), `verification:"unverified"`, and `integration:"not_recorded"`.
Exit 0 means accepted **submission**, not completed, tested, accepted or
integrated work. Submission retains the jobs acknowledgment timeout and its
uncertain-submission semantics; a lost client must inspect the existing job,
not blindly resubmit.

Substitute the returned 32-hex ID below:

```json
{"id":"RUN_ID"}
```

Use that request for `status`. It wraps the jobs projection, including canonical
item states, attempts, session IDs and result hashes. A failed/interrupted job
can return exit 2 with a complete status response. No prose is interpreted as
verification or a command.

### Bounded wait-any

```json
{"id":"RUN_ID","expected_attempt":1,"timeout_ms":1000,"seen":[{"item":0,"attempt":1}]}
```

`wait-any` returns the first currently terminal item not in this run's caller-
owned `seen` list. It returns `kind:"team_completion"`, the canonical `item`,
and a `cursor:{item,attempt}` to add to the list. Keep one latest cursor per
item, scoped to the run ID, with at most 64 pairs. Ties use definition order,
not inferred timestamps. Carried results keep their original item attempt and
do not become new completions merely because the job retries.

Timeout defaults to 0 (one observation), ranges from 0 to 30,000 ms, and returns
**124** with `kind:"team_wait_timeout", cancelled:false`. It never cancels the
job. A cooperative caller interruption returns 130 and likewise does not cancel.
An attempt/membership change during the wait refuses rather than delivering a
new generation under an old fence. Optional `item` restricts the observation.
A returned failure/cancellation is a completion event, not a success assertion;
exit 0 means an event was found. Its `item.state` remains authoritative.

This bounded wait is for external clients and fallback. Native agents should
consume the lead-owned safe-boundary completion notifications instead of
spending model turns polling. This file does not register notifications.

### Collect

```json
{"id":"RUN_ID","item":1,"expected_attempt":1,"max_bytes":16384}
```

Collection requires a terminal task and never accepts a supplied session ID or
path. `max_bytes` is 1–262,144 (default 16,384), independently bounding the result
prefix and log tail. Both are base64 so an exact byte bound can split UTF-8 safely.
The response contains original byte counts, explicit truncation flags, SHA-256
provenance, root/item attempts and the correlated session ID. There is no
unbounded concatenation of worker output into lead context.

The final assistant answer comes from the **authoritative stored session**, not
from apparently successful log text. Its hash must match the job's recorded
`result_sha256`. Successful logs must also match `log_sha256`. An unavailable,
continued, corrupted or oversized session returns no answer and cannot become
accepted work. Session JSON reads are confined and capped at 4 MiB; stored logs
are confined and capped at the existing 4 MiB jobs log bound. A failed task can
still return its bounded log, but has exit 2 and never becomes verified.

Session lookup uses the authoritative launch `root.workspace`, or resolved
`item.workspace.cwd` / `.path` when provided by the scheduler. A requested
workspace policy without resolved metadata is refused. Requested policy is not
a path authority. Integration must agree the resolved-cwd key with the scheduler.

All collected output remains untrusted dependency data. A matching hash means
integrity, not correctness, permission, testing or acceptance.

### Cancel

```json
{"id":"RUN_ID","item":1,"expected_attempt":1}
```

Omit `item` to cancel the run (operator/submitting parent only). The service
requires `expected_attempt`, preflights it, and passes it unchanged into the
core jobs cancellation transaction. The response is the canonical jobs response;
it reports a request, not proof of cleanup. Other runs are not signalled.

**Integration prerequisite:** the scheduler owner's atomic expected-attempt
check must be present in core cancellation. The prerequisite jobs commit used
by this worker does not enforce that transaction fence yet. A service-side
read/check alone cannot close the cancel/retry race. The tests prove stale
preflight rejection and selected/unrelated-run behavior, not that pending core
transaction fix.

### Explicit verification: refused, not simulated

The parser and separate `team_verify` permission identity accept:

```json
{"id":"RUN_ID","item":1,"expected_attempt":1,
 "command":"make test","cwd":"/absolute/task/workspace","timeout_ms":30000}
```

The permission detail includes this exact command, cwd, identity, timeout and
captured runtime caller. A `team_status` grant must never authorize it. However,
**this slice does not execute the command**. It returns exit 1,
`error_code:"TEAM_VERIFY_UNSUPPORTED"`, `verification:"unverified"`. It creates
no check sidecar and never records an invented test result.

A safe implementation still needs a check-process host seam that proves complete
cleanup, then nonblocking job-owner locking, retry/workspace/result fences,
clean exact revision checks before and after execution, bounded output/hash,
actual exit/time provenance and private persisted checks. Neither a worker's
"done" message nor launch/collection success supplies any of these. There is
therefore no review/implement/**verified** template in this slice. Explicit
workspace integration belongs to the separate workspace service; no Git merge
or cleanup is duplicated here.

## Shared C/tool integration

`src/core/team_control.h` provides:

- `tny_team_op_parse`, `tny_team_parse_argv`: one operation/request grammar.
- `tny_team_permission_tool`, `tny_team_op_is_sensitive`: distinct `team_start`,
  `team_status`, `team_collect`, `team_wait_any`, `team_cancel`, `team_verify`.
- `tny_team_detail`: validation and canonical, secret-safe permission detail.
- `tny_team_run`: already-permitted execution, with identity and state rechecked.

A typed tool adapter uses the same request object and detail/run APIs. A terminal
interceptor uses the same argv parser, then those same APIs. **Do not call
`cmd_team` from a tool**: it is the local-operator adapter. Construct
`tny_team_caller` from the runtime's captured session/member snapshot, never from
request JSON, `TNY_SESSION_ID`, or a worker's prose. The private test driver has
synthetic caller injection solely to test these C boundaries; production has no
such injection path.

Authorization policy:

- Local operator: existing same-user job authority.
- Captured submitting parent session: its recorded child run.
- Other captured member: run ID, task index, session ID, job attempt **and** item
  attempt must match. Members can read run status, but can collect/wait/cancel
  only their own item. Roles alone confer no parent/peer control authority.
- Unknown/stale/wrong-run sessions: refused before collection/control.

The CLI refuses `TNY_NESTED=1` execution and requires the trusted terminal adapter,
which avoids silently promoting ordinary model-launched commands to operator
controls. This is defensive routing, not a sandbox against a same-user program
with unrestricted shell access.

## Capabilities and lead-owned wiring

| Context | This slice |
| --- | --- |
| Native local CLI | Implemented adapter; command lookup/help/dispatch registration required |
| Native all-tools profile | Shared service ready; schema, permission and caller-capture adapter required |
| Native terminal profile | Same parser/detail/run; trusted interception required; nested CLI refuses |
| Host-owned backend loops | No new agent tools or loop ownership; ordinary local CLI remains an operator surface |
| SSH tool context | Service refuses before process/provider/file side effects |
| Embedded/libtny | Service refuses; no new public embedding ABI or SDK API |
| wasm/browser | Service refuses through the existing jobs capability seam |

The lead owns command registration, tool/interceptor wiring, notifications and
`agents --run`. C source discovery currently builds the new files automatically;
no Makefile edit is needed just to compile them. The integration runner already
auto-discovers `test_team_control.py`; the test accepts its trailing executable
argument through the existing jobs fixture helper. It imports `test_jobs.py`, uses its loopback provider and
builds a temporary CLI driver from the real release objects if
`TNY_TEAM_DRIVER` is absent. An equivalent prebuilt driver can be supplied by
that variable. No internet, live keys or downloaded tools are required.

Nix must include this test and its `test_jobs.py` import, the new C files and the
existing source/build inputs used by the temporary driver. Existing full `src`
and `tests` filters and the integration runner's glob cover discovery; the lead
must ensure make, C/C++ compilers, Python, Git and native runtime/link dependencies
are present for the temporary driver build.
This worker does not change Make/Nix or production command/tool registration.

## Focused verification

```sh
python3 tests/integration/test_team_control.py -v
clang-format --dry-run --Werror src/core/team_control.c src/core/team_control.h src/cli/cmd_team.c
ruff check tests/integration/test_team_control.py
```

Seven tests exercise the real service/CLI, detached jobs and deterministic
provider: overlap and responsive submission; client exit; cursor exhaustion and
bounded timeout; failure; selected cancel with unrelated-run survival; captured
membership; stale attempts; malformed input; unsupported contexts; exact
permission identities; bounded hash-correlated collection and changed artifacts.
Verification tests establish **refusal and no side effects**, not successful
check execution. Full integrated quality/leak/platform/recovery gates remain with
the delivery lead.
