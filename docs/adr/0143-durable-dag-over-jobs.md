# ADR 0143: Durable DAG execution over existing jobs

Status: accepted foundation; broader team delivery remains incomplete

## Decision

Extend the private C++20 jobs owner and its public C ABI. Do not add a
controller, daemon, provider loop or run-to-job binding transaction. A run ID
**is** the existing job ID, a stable task ID is its zero-based item index, and
an execution identity is `(job ID, item index, item attempt)`. The job attempt
is the retry generation; a carried item retains its original item attempt.

Opt-in `dag:true` ask submission adds `depends_on` indices, optional `label`,
and `role:lead|worker` (default worker). Forward dependencies are legal.
Validate every edge, duplicate, index and cycle before output claims or
execution. Reject DAG metadata without opt-in. Existing batch scheduling and
synchronous subagent calls retain their meaning. Image DAGs are refused.

The existing supervisor gates each launch on successful, integrity-checked
dependencies. A terminal failed/cancelled/interrupted dependency produces a
terminal `failed` descendant with `error_code:dependency_blocked`, without a
provider request. There is no second scheduler or autonomous retry. DAG launch
claims and dependency bindings are persisted under the existing state lock
before spawning outside it, including on POSIX without native scope admission.
Cancellation after the claim has the existing running-item cleanup semantics.

## Identity and authority

`tny_jobs_run_context` accepts the active parent session from a trusted runtime
adapter. The ordinary CLI records null parent lineage. Request-supplied parent
or run IDs are refused, not used to discover membership or grant access. Roles
and labels are descriptive, not permission grants. Item session IDs are the
canonical sessions observed in completed child event logs. This is inspectable
provenance, **not** a live dashboard membership or notification implementation.

A DAG records the resolved provider/model/effort and permission/tool ceilings,
never credentials. Items may override model/effort with the existing child
flags. An explicitly supplied item provider must equal the resolved job
provider; different-provider teams currently need separate jobs. Retry requires
the same recorded selection and ceilings (even narrowing currently requires a
new job). Credentials remain in the existing private launch pipe. Account-scope
admission and inherited max_steps wiring belong to the shared admission
integration; this foundation does not claim credential/account identity fencing.

## Recovery and verification

The persisted task-definition SHA-256 covers the canonical stored request,
label, role and dependency list. A launch dependency hash binds each input
index to its original item attempt, definition hash and result SHA-256. Retry
checks definitions, carried dependency bindings and existing canonical session
answer/log integrity before it spends. Missing/deleted/corrupt artifacts fail
closed. These hashes are corruption/staleness checks, not signatures against
an attacker who can replace the private job record and its hashes.

Capture clean Git HEAD through the existing bounded argv-only Git seam, never
under state.lock. Unknown, dirty or non-Git workspaces have a null revision:
initial execution is allowed, but retry is refused. Retry requires the same
clean revision and canonical workspace path. This is a conservative carried
output fence, not a snapshot of arbitrary external files, remote state or
ignored files, and not workspace isolation. Concurrent workspace edits remain
a risk until managed workspace integration. Editing workflows that change the
checkout must submit new work or use the later workspace integration.

Execution exit zero and valid hashes never imply acceptance. Every DAG run and
task has explicit `verification:unverified`, including successful carried work.
No worker prose authorizes checks, integration or accepted completion. This
slice does not run verification commands or add an acceptance operation.
Dependencies gate execution success, not human acceptance. Result references
are inspectable through status/logs; outputs are not silently inserted into
subsequent prompts.

Reuse owner locks, revision fencing, immutable attempt snapshots and cleanup
holds. Concurrent retry has one owner. Repeated retry never re-executes a
successful item. Only explicit retry selects failed, cancelled or interrupted
items; no `resume` alias silently replays uncertain external effects. Owner loss
marks unfinished work interrupted and preserves completed A; unknown cleanup
refuses retry even when a PID looks dead. There is no exactly-once claim.

## Public and platform slice

Use `jobs submit batch --request`, status, bounded wait, logs, cancel and retry.
The shared jobs service is available to existing tool adapters, but the lead
must extend the declared submit schema and wire trusted parent context. No new
global dispatch, tool registration, SDK or shell wrapper is added here.

| Surface | Capability / limitation |
| --- | --- |
| Native CLI | Durable DAG submission, bounded wait/status/logs, cancel, explicit retry |
| Native engine tools / terminal interception | Same service; new schema and trusted lineage wiring required |
| Host providers | Existing CLI children retain host-owned loops; no host notification parity claim |
| Shell profiles | May invoke existing jobs commands; no new shell controller |
| SDK / embedding | No implicit durability for ephemeral SDK workflows; no host-service/custom-tool portability claim |
| SSH | No remote supervisor or remote workspace guarantee; submit on the native remote host explicitly |
| wasm | Existing clean native-execution refusal; no Git or child execution |

## Workspace integration handoff

The workspace owner provides standalone `util/task_workspace` C API. Do not
call it in this commit. The lead should prepare or reopen the managed workspace
**outside state.lock and before committing the launch claim**, then revalidate
attempt/cancel/revision under the lock. Bind its durable provenance to the
existing job/item/attempt, use its path in the private child payload, and retain
it under the existing cleanup hold until retirement is proven. A crash between
workspace preparation and launch must leave an inspectable owned workspace,
not start an independent controller or silently redo integration. Never run
Git inside a jobs transaction. Account admission and max_steps are separate
lead-owned call points before release of the admitted child.

## Evidence and remaining delivery

`JobsDAG` in the existing jobs integration suite exercises forward edges,
validation without spend, A/B/C provider barriers, explicit recovery after
verified cancellation, owner loss with interrupted B and unknown cleanup,
blocked descendants, concurrent/repeated retry, immutable history, corrupt and
deleted logs, changed definitions/dependencies/revisions, and ceiling changes.
The existing jobs suite retains credential isolation and cleanup fault tests.

This does not complete #153 or #155. Remaining public work includes trusted
lineage wiring, run-filtered dashboard/agents views, notifications and wait-any,
accepted verification, managed workspaces, account admission, and broader
cross-platform/integrated evidence. Clean recovery after uncertain owner loss
is deliberately unavailable until cleanup can be proven without stored-PID
inference. The cancellation recovery test is not evidence of exactly-once
recovery from arbitrary controller crashes.

## Integrated delivery addendum

The native delivery now wires the shared CLI/typed/terminal controls, captured
parent lineage, run-filtered dashboard, safe notifications, workspaces and
admission described above. The original handoff list is historical, not a claim
that those public paths are still absent. Tests exercise real parent orchestration
and both tool profiles; verification stays explicit and unverified by default.

DAG `item.provider` now snapshots supported native profiles independently of the
parent's retained overrides. Public status and permission detail expose selectors;
only private payload/environment carries credentials. Every item scope, including
carried successes, is rechecked on retry. Two fixture endpoints prove routing,
credential isolation, unchanged implicit parent selection, ceilings, and no spend
on changed secondary credentials/accounts. Shared workspace, default sandbox,
standard Bearer routing and safely reproducible profiles are required. Mixed
provider/account admission, isolated explicit-provider configurations and store-
refresh-dependent selection are refused rather than weakening ownership or
permission ceilings. The exact support matrix is in docs/jobs.md.
