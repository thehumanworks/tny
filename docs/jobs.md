# Durable jobs

Ask and image work that outlives the process that asked for it
([ADR 0093](adr/0093-durable-native-jobs-and-verified-retry.md)). A job is
submitted, gets a durable id immediately, and is executed by a detached
**supervisor** that runs this same `tny` binary once per item: `tny ask
--events=jsonl --progress=none` for ask work, `tny image generate|edit --json`
for image work. There is no second provider loop, no agent loop and no daemon;
the canonical event contract ([ADR 0090](adr/0090-canonical-foreground-ask-events.md))
is preserved byte for byte in each item's log.

Native execution uses Linux pidfds, Darwin generation-checked process
signalling, or retained MSYS2 Windows Job Objects
([ADR 0099](adr/0099-native-msys-job-ownership.md)). The browser build cannot own a child process:
`submit`, `retry`, `cancel` and `rm` refuse with a documented error *before*
any file or provider side effect, while reading existing records still works.
Image generation itself is unaffected there.

## Opt-in durable DAG (native ask jobs)

[ADR 0143](adr/0143-durable-dag-over-jobs.md) extends the existing supervisor,
not a second controller. Submit a lead and two read-only workers through the
existing batch JSON entry point:

```sh
cat <<'JSON' | tny jobs submit batch --json
{"kind":"ask","dag":true,"concurrency":2,"items":[
  {"prompt":"Read only: identify the review scope. Do not edit files.","label":"lead","role":"lead"},
  {"prompt":"Read only: review reliability. Do not edit files.","label":"reliability","role":"worker","depends_on":[0]},
  {"prompt":"Read only: review security. Do not edit files.","label":"security","role":"worker","depends_on":[0]}
]}
JSON
# Use the returned id. These operations remain bounded / explicit:
tny jobs status JOB_ID --json
tny jobs wait JOB_ID --timeout 30 --json
tny jobs logs JOB_ID --item 1 --json
tny jobs cancel JOB_ID --items 1 --expected-attempt 1 --json
tny jobs retry JOB_ID --failed --json
```

Read-only instructions are not a sandbox. Use the existing permission controls
and tool profile for enforcement. The lead role is descriptive; it does not
confer privileges or automatically collect worker answers. For a
review/implement/verify template, use three items labelled with those names,
with dependencies `[]`, `[0]`, `[1]`. Execution gates do not inject earlier
answers into prompts. Inspect canonical result/session/log references explicitly.
Shared editing invalidates the clean-checkout retry fence. Managed isolated
editing workspaces are now enrolled by this same supervisor; see below.

`dag:true` currently supports ask items only. Dependencies are zero-based item
indices, including forward edges. Cycles, duplicate edges, self-edges, invalid
indices, and DAG metadata without opt-in are refused before execution. Labels
are bounded strings; roles are `lead` or `worker` (the default). DAG definitions
must be persisted; use ordinary batches for `persist_request:false`.

The returned `run_id` equals the existing job `id`. Each item exposes `task_id`,
`attempt`, dependencies, label/role, definition and dependency SHA-256, plus
its existing canonical result references. CLI `parent_session_id` is null.
Trusted runtime adapters can supply their active session with
`tny_jobs_run_context`; supplied parent/run IDs in JSON are refused and never
create membership or authority. Tool schema and runtime lineage wiring remain
lead-owned integration work.

A descendant waits for all dependencies to succeed with intact artifacts.
A failed/cancelled/interrupted dependency makes the descendant `failed` with
`error_code:dependency_blocked`, without a provider request. Explicit retry
reuses successful items and their original attempts after integrity checks;
concurrent retries cannot create two execution owners. Retry never silently
replays successful work or uncertain effects. Unknown cleanup refuses retry.

### Attempt-fenced cancellation and execution scope

DAG cancellation requires `expected_attempt` in typed JSON, or
`--expected-attempt N` in the CLI. Use the attempt returned for the intended
operation. The comparison and cancellation flags are in the **same state-lock
transaction**, including when the job is already terminal. Missing or stale
attempts return `stale_attempt` without changing flags. Do not automatically
refresh and replay a stale cancellation: inspect the new attempt first. Ordinary
batch cancellation retains its existing grammar and semantics without this fence.
The team-control adapter must pass its expected attempt through to jobs.

A DAG also persists `execution_scope_sha256` before launch. This one-way
fingerprint covers the resolved provider endpoint, credential/account identity,
auth routing and extra headers, relevant policy/repository configuration,
configured tool-environment values and extra workspace directories. Raw keys,
tokens and secret-bearing URLs are not stored. Retry compares the current scope
before carrying results or starting work. Changed or missing scope evidence
requires a new explicit run, even if provider/model names remain unchanged.

For a known ChatGPT account, account identity plus credential source fences the
scope, so normal same-account token refresh can survive. Where no stable account
identity is available, credential bytes are conservatively fenced: key rotation
requires a new explicit run. The fingerprint is an integrity check, **not an
authorization token** or a snapshot of arbitrary external files. It does not make
public admission aliases automatically identify all routes to an account.

Every DAG task and run reports **`verification:unverified`**, even on exit zero.
Hashes prove integrity, not acceptance. No worker prose triggers verification
commands, acceptance or integration. This slice has no notification/wait-any
service or run-filtered dashboard.

Retry requires unchanged definitions, dependency bindings, canonical successful
session answers/logs, the same clean Git HEAD/workspace path, and the recorded
provider/model/effort/permission/tool ceilings. Dirty or unknown/non-Git
revision is inspectable as null and cannot be retried. This does not snapshot
ignored files, arbitrary external inputs or concurrent edits. Shared admission
now bounds explicitly enrolled public scopes, not automatically discovered
accounts. Credentials stay private in the launch pipe; they are not persisted
in DAG metadata. Per-item model/effort selection works. Opt-in DAG provider
selection is now available within the bounded native-profile contract below;
ordinary batches retain their existing behavior.

Native CLI execution works through existing children. DAG workspace policies
and enrolled admission require a native-loop provider (including Codex). Host
providers keep ordinary batch behavior but are refused for these enrolled
modes before worktree/admission files or child launches. SSH does not become a
remote durable controller: run the command on
the remote native host explicitly. Embedded SDK workflows do not implicitly
acquire native durability or custom-tool portability. wasm keeps the existing
clean refusal for job execution. See ADR 0143 for workspace and admission call
points and the remaining #153/#155 delivery gaps.

## Explicit native worker providers

A DAG item may name its own native profile, independently of the parent's
current provider overrides:

```json
{"kind":"ask","dag":true,"concurrency":2,"items":[
  {"prompt":"Review reliability; do not edit files.","provider":"profile_a"},
  {"prompt":"Review security; do not edit files.","provider":"profile_b",
   "model":"worker-model","effort":"medium"}
]}
```

Configure those profiles through the existing provider configuration, with each
profile's own `base_url`, `api_key_env`, model and wire API. The
usual resolver's settings/environment precedence applies. A supplied item model
or effort overrides that profile's resolved default. Without `item.provider`,
the item continues to inherit the parent's resolved selection and overrides.
For ordinary batches, select the whole batch with global `--provider`; this does
not add per-item dispatch to their existing contract.

Before creating a job record or worktree, jobs validates **all** selectors and
builds owned private per-item launch snapshots. Explicit selection resolves into
a fresh context: it never modifies the parent context/environment or lends the
parent's retained `--api-key-env`, endpoint, model, effort or wire override to the
new profile. It uses the existing ask child, session runner and provider loop.
There is no additional executor.

Status/provenance and permission detail disclose each DAG item's effective
`provider`, `model` and `effort`. Definitions include the explicit provider, and
each item has a secret-safe `execution_scope_sha256`. Retry resolves and checks
**every** item, including carried successes, before spending. A changed secondary
endpoint, key/account, model, effort or routing scope refuses reuse. The original
root execution-scope fence remains in force too. Legacy homogeneous DAG records
without per-item scopes retain their existing root fence.

Only the selected item's private credential mapping reaches its child. Selectors
are on argv; credentials and secret-bearing URLs use the existing private
payload/environment carriers. Root and sibling credentials are excluded from
operational and declared environment aliases, including credentials belonging to
carried siblings during retry. Snapshots must be complete; allocation failure
must not fall back to the parent's mapping. These protections do not turn the
same-user filesystem or environment into an OS sandbox.

### Supported scope and explicit refusals

- Parent execution remains a native DAG. Worker selectors are 1–63 ASCII letters,
  digits, `_` or `-`: `openai`, configured native OpenAI-compatible profiles, or
  builtin `codex` with `CHATGPT_ACCESS_TOKEN` supplied in the environment.
- Custom names require a settings-backed profile definition. Environment values
  may override its endpoint/key/model as usual, but environment-only names are
  refused because private child environment filtering removes their discovery
  variables. `openai` and builtin `codex` do not need such a definition.
- Explicit profiles require their own nonempty credential and a resolved model
  (or item model), standard `Authorization: Bearer` routing, and no custom header,
  token-field, output-schema or non-default service-tier overrides. The current
  child CLI cannot freeze those additional routing choices safely.
- Explicit builtin Codex selection is subscription/environment-backed only.
  `CHATGPT_ACCOUNT_ID` may accompany the token; known-account token refresh keeps
  the existing account-fingerprint policy. Store-only selection is refused because
  the general Codex resolver may refresh/write the login store before admission.
  Shadowing builtin `codex` with a custom profile is also refused in this slice.
- Explicit selection currently requires a **shared workspace**, default `auto`
  sandbox and no extra directories. Isolated workspaces, custom sandbox and extra
  path configurations are refused rather than silently weakening their ceilings.
  Existing workflows without explicit item provider retain their workspace path.
- Permission mode, tool profile, read-only policy and positive step ceilings stay
  parent-owned. Selecting another provider never grants additional tools or paths.
- Shared admission refuses different selectors or different resolved provider/
  account scopes from the parent's scope. Homogeneous explicit selection with the
  same scope works. Use separate declared jobs for mixed-provider admission.
- Unknown and host profiles are refused before execution state/provider work.
  Other builtin native login profiles that need unsupported routing/refresh are
  not silently approximated. Existing SSH/embedded/wasm refusals remain.

Soft-token totals remain observed best-effort token counts across workers, not
currency estimates. Tool-schema discovery and broader provider/workspace policy
support remain lead-owned integration; this does not claim all of #153.

## Managed workspace enrollment

The scheduler now calls the [task workspace helper](task-workspaces.md), using
the existing job ID, item index and attempt. DAG items accept:

```json
{"prompt":"Implement the change and record checks; do not integrate it.",
 "workspace":{"policy":"isolated","base":"HEAD"}}
```

| Policy | Scheduler behavior |
| --- | --- |
| `shared_read_only` | Explicit read-only override. Launch checkout; child receives `TNY_TEAM_READ_ONLY=1`. |
| `shared_writable` | DAG default. Editing in the launch checkout. No file isolation. Permission mode defaults to `yolo`. |
| `isolated` | Prepare a distinct owned worktree for this item attempt; child uses its returned cwd. |

`base` is optional and only valid with `isolated`. Without it, the helper requires
an initially clean launch checkout. An explicit commit acknowledges exclusion of
launch edits. Ordinary batches without DAG/workspace options retain their old
workspace behavior. Unsupported host, SSH and embedded enrollment is refused;
wasm keeps its native-execution refusal before side effects.

The scheduler commits `workspace_preparation:intent` before any Git operation.
Preparation and inspection run **outside the job state lock**. The supervisor
then revalidates attempt and cancellation, records returned cwd/branch/base/origin
provenance, and commits the launch claim. Git preparation cannot block job status
or cancellation under that lock. A canceled preparation may leave a retained
owned worktree, but never starts its worker. It is not adopted as foreign work.

After owned execution cleanup, bounded helper inspection records the revision,
tracked binary-capable patch, status names and dirty flag. Oversized/unavailable
inspection stays `unverified`, not truncated success. Canonical session answer
verification uses the **worker cwd**, not the launch checkout's session directory.
The supervisor retains its workspace handle until retirement; closing the handle
never merges or removes the tree. Unknown cleanup retains work for explicit
operator inspection. There is no automatic integration, cleanup or acceptance.

Retry refuses selected isolated tasks once preparation was attempted. Carrying
an isolated success requires a clean, unchanged, owned workspace with recorded
inspection; dirty editing work requires explicit integration/new work instead.
This conservative rule prevents hidden replay in a dirty tree. The helper still
does not sandbox arbitrary paths or snapshot arbitrary external inputs.

**Read-only delivery boundary:** the scheduler provisions the trusted ceiling
marker; the integrated native runtime enforces it at its permission boundary.
The real edit-tool denial test runs with `TNY_TEST_TEAM_READ_ONLY_ENFORCED=1`.
Checkpoint retention and propagation remain runtime responsibilities. This is
not an OS sandbox for arbitrary same-user processes. Host modes cannot use these
workspace policies. Mailbox confinement requires canonical trusted state paths;
a symlinked HOME/state-directory prefix is refused, not silently adopted.

## Shared admission enrollment

A native ask job (DAG or ordinary batch) can explicitly enroll:

```json
{"kind":"ask","concurrency":2,
 "admission":{"label":"review_team","provider_scope":"public_account_alias",
              "cap":2,"queue_cap":16,"claim_limit":100},
 "items":[{"prompt":"Review reliability."},{"prompt":"Review security."}]}
```

The immutable scope is under `<tny_dir>/admission`, keyed by the explicit public
`label` and `provider_scope` aliases. Each is 1–63 ASCII letters, digits, `_` or
`-`. Only the five documented admission fields are accepted; known credential
values are rejected as aliases. **Never put an API key or credential in either
alias.** Grammar cannot identify every possible secret. The alias is chosen
by the user; it does not automatically identify every route to an account, and
two aliases for one account do not share a ceiling. `cap` is 1–16; `queue_cap` is
1–128; `claim_limit` is a positive lifetime count of fresh **launch claims**.
It is not a model HTTP-request, token, money or subscription budget.

Submission initializes/verifies the immutable scope once. Retry preserves its
configuration and never resets the ledger. The existing supervisor retains sole
job ownership. It calls admission outside `state.lock`, preserves the local
concurrency ceiling, and launches only from a fresh committed `granted` result.
A repeated `owned` result is not another launch authorization. A grant consumes
one claim even if cancellation wins before spawn; it is never refunded.

Status exposes effective configuration, admission ticket/reason/counts and
exhaustion. Waiting items queue without contacting the provider. Cancellation
cancels tickets. Owner loss converts outstanding grants to cleanup holds rather
than freeing them by stored-PID inference. Capacity is released only under the
existing jobs proof of never-launched or completed owned cleanup. Busy
transactions retry outside job locks; uncertain errors retain capacity. A paused
or unknown owner can therefore block followers indefinitely. See
[the admission contract](admission.md) for FIFO, history and filesystem bounds.

Enrolled children receive `TNY_ADMISSION_ENROLLED=1`. Inherited enrolled jobs
cannot submit or retry nested jobs, before job/admission files or enqueue.
Together with native synchronous-subagent refusal, enrolled execution has a
**hard depth-one launch policy**, not recursive sharing of an arbitrary scope.
This scope covers opt-in top-level job launches, **not arbitrary same-user shell processes**,
all SDK calls or every provider request made inside an admitted turn.

Positive `ctx.max_steps` travels as owned private payload text and a child
`--max-steps` argument. Retry preserves the original ceiling and may narrow it
with a stricter current cap; it cannot widen it. This is a native turn-step
ceiling, not admission's claim counter or a token budget.

Each item records observed canonical cumulative input/output tokens, or explicit
unknown values. The last cumulative usage event is retained, not repeatedly
summed. Job `usage.known_input_tokens` / `known_output_tokens` sum available
attempt evidence, including prior immutable attempts; carried successes are not
counted again. `unknown_items` counts item-attempts without usable evidence,
including unfinished/not-started items. Observed counters do not prove that a
provider reported every billed request. Missing usage and cost are not estimated.
These observations can drive the separate soft run policy below. They do not
provide a hard token, money or billing guarantee.

### Opt-in soft run token policy

A native DAG request may include:

```json
{"budget":{"soft_tokens":1000,"unknown_usage":"stop"}}
```

`soft_tokens` is a positive integer. `unknown_usage` is `stop` (the default) or
an explicit `continue`. Unknown fields are refused. The policy is immutable for
retry; changing it requires a new explicit run. It is separate from admission's
**hard launch-request `claim_limit`**, which is not a model HTTP-call cap.

After collecting terminal item usage, the existing supervisor checks the soft
policy at a safe scheduling boundary. When observed input plus output tokens
reach the limit, it cancels pending work and waiting admissions without starting
those children. With the default unknown policy, an attempted terminal item
without usable usage also stops pending work. Future unstarted tasks do not count
as zero-cost evidence, and do not prevent the first launch. `continue` explicitly
allows progress with incomplete usage; the uncertainty remains visible.

Already-admitted active work is not interrupted by this soft policy and **may
overshoot the limit**, including work that finishes while another result is being
collected. Multiple model requests within one child may also exceed it before
that child's cumulative usage settles. This is not pre-reserved token capacity.

Status exposes `budget_state`, `budget_observed_tokens` and
`budget_usage_unknown`. Counters persist across retries and include previous
attempts without counting carried successes again. An exhausted or default-stop
unknown policy refuses retry before spending; explicit `continue` retains the
unknown marker. Unknown amounts are never invented or charged as known zero.
No hard model-call allowance, reserved-step pool, money cap or absolute run
admission deadline is implemented here. Positive child `max_steps` remains a
separate turn ceiling; HTTP retries and host-owned loops are not model-call
billing guarantees.

## Private member capability provisioning

Every DAG item preparation creates a fresh random 32-byte value encoded as a
64-hex bearer. Before admission, the supervisor persists only its SHA-256 hex
verifier as `item.mailbox_capability_sha256` under `state.lock`. The child gets:

- `TNY_TEAM_RUN`: existing job ID;
- `TNY_TEAM_TASK`: stable item index;
- `TNY_TEAM_ATTEMPT`: current item/job launch attempt;
- `TNY_TEAM_CAPABILITY`: the private bearer.

The bearer is not written to job metadata, status, logs or argv. Its temporary
owned environment and supervisor buffer are wiped after spawn. Ordinary batch
children get no team membership. Ambient parent team/admission fields are not
forwarded as membership; inherited read-only ceilings remain restrictive.
Request-supplied private identity, bearer and verifier fields are refused.
Root `peer_messages` must be boolean when supplied and defaults to false.

Before a sensitive legacy jobs operation, inherited `TNY_TEAM_RUN` is checked
against that run's private verifier and current item/job attempt under its state
lock. Invalid membership is refused. Valid members are also explicitly refused
legacy submit/cancel/retry/remove controls (`member_control_unsupported`), rather
than receiving operator authority over a supplied job ID. This conservative slice
does not expose a member-safe own-task mutation through legacy jobs; use the lead
or a dedicated authorized task adapter. CLI operators outside nested member
contexts retain existing authority. Parent-session lineage comes only from the
trusted `tny_jobs_run_context` adapter argument, never a request sender/session.

These fields match the lead's `team_runtime` verifier contract without adding a
link dependency on that runtime here. Mailbox delivery, trusted parent adapter
wiring and synchronous-subagent membership stripping remain runtime integration,
not a second controller implemented in jobs. The runtime must validate inherited
identity and verifier under the job state lock before granting member operations.

## Surfaces

| Surface | Entry point |
| --- | --- |
| Shell | `tny jobs …` (`tny jobs --help`) |
| Agent tools | `job_submit`, `job_control`, `job_status` |
| Terminal tool | `tny jobs …` typed into `terminal` is intercepted into the tools above ([ADR 0063](adr/0063-tny-verb-interception.md)) |

All three go through one service: the same validation, the same records and
the same permission identities. A `tny jobs` command the interceptor cannot
classify is **refused**, never handed to the shell — that keeps the classifier
from becoming a way around the job permission gate.

```sh
tny jobs submit ask --prompt "summarize this repository"
printf 'audit the Makefile' | tny jobs submit ask
printf 'An orange robot' | tny jobs submit image --output-file robot.png
tny jobs submit batch --request batch.json
tny jobs status <id> --json
tny jobs wait <id> --timeout 120
tny jobs logs <id> --item 2 --max-bytes 4096
tny jobs cancel <id> --items 1,3
tny jobs retry <id> --failed
tny jobs list
tny jobs rm <id>
```

A batch request is one bounded JSON document: one `kind` (`ask` or `image`),
1–64 items of that kind, `concurrency` 1–16 (default 2). Mixed kinds are
rejected before any side effect.

```json
{
  "kind": "ask",
  "concurrency": 4,
  "items": [
    {"prompt": "review src/core/jobs.cpp"},
    {"prompt": "review src/util/process.c", "model": "gpt-5"}
  ]
}
```

Image items add `operation` (`generate`/`edit`), `output_file`, `quality`,
`size`, `strict_size`, `images` (≤5 references) and `overwrite`.

Exit codes: `0` ok, `1` invalid request/unknown job/unsupported runtime,
`2` the job failed or could not be completed, `124` a `wait` deadline passed,
`130` the caller interrupted a wait. A timeout or interrupted wait leaves the
durable job running; use `cancel` to stop the job itself.

## On disk

```text
~/.tny/jobs/<32 hex id>/        # 0700
  job.json                      # 0600, version 1, the current attempt
  owner.lock                    # held by the live supervisor, its whole life
  state.lock                    # short nonblocking transactions over job.json
  attempt-<n>.json              # immutable snapshot of a finished attempt
  attempt-<a>-item-<n>.log      # immutable item stdout, bounded to 4 MiB
~/.tny/jobs/reservations/       # 0700, one lock+record per canonical output
```

`job.json` records schema `version`, `kind:"job"`, id, job kind, workspace,
timestamps, `revision`, `attempt`, `state`, `cancel_requested`, `exit_code`,
`error_code`/`error`, `cleanup` and the ordered `items`. Each item records its
index, state, timing, exit code, safe error, log path, the configuration a
later retry would replay, and — on success — the ask `session_id` plus
`result_sha256`/`log_sha256`, or the image `output_path`, `output_sha256`,
`output_bytes`, plus `manifest_path` and `operation_id` when the image service
supplies them. Both provenance fields are copied literally from the image
CLI's own result object and stay `null` otherwise; this component never
invents an operation id or a manifest path of its own.
Writes are atomic (temp + rename) under `state.lock`, so a reader never locks
and never sees a partial document. An unsupported version, an unknown state or
a malformed record fails closed rather than being guessed.

States are `queued`, `running`, `succeeded`, `failed`, `cancelled`,
`interrupted`.

## Ownership, crashes and cancellation

The submitter creates the job directory, takes `owner.lock`, writes the
queued record, claims its output reservations and only then starts the
supervisor — so a concurrent submitter can never mistake an unacknowledged
job for an abandoned one. The supervisor inherits that **live lock** on
descriptor 3 and validates it (file identity plus a real conflicting lock)
before touching anything. The request itself arrives on an anonymous pipe and
is acknowledged on another before submit reports success.

- **Submitter exits or is killed.** The job keeps running; its state and logs
  stay inspectable. Killing the submitter's entire process group does not
  touch the supervisor, which leads its own group.
- **Handshake uncertainty.** If the acknowledgment does not arrive, submit
  reports `JOB_SUBMISSION_UNCERTAIN` **with the durable job id** and exits 2.
  It never writes a competing terminal record and never rolls back the
  child's reservations: use `tny jobs status <id>` and cancel if unwanted.
- **Supervisor dies.** `owner.lock` becomes free. Any queued or running record
  is then projected as `interrupted` with `cleanup:"unknown"` and exit 2 —
  never `succeeded`, never `cancelled`, never "still running". A paused or
  hung supervisor still holds its lock and is not mistaken for a dead one.
  On MSYS2 an outer Windows Job contains the supervisor and every item from
  creation. Its last handle closes on supervisor loss, terminating even a
  stopped bootstrap that has not yet joined its individual item Job.
- **Item children.** A job item knows the supervisor that started it and
  compares `getppid()` with it at the cancellation probes it already runs
  (`tny ask`'s engine pump and tool loops, `tny image`'s 100 ms read/poll
  callback). A dead supervisor therefore stops the orphan cooperatively.
  This is a kernel-maintained parent relationship, not a stored pid, so pid
  reuse cannot defeat it. If the parent is already gone at startup, no
  request is made at all.
- **Cancel.** `cancel` commits `cancel_requested` under `state.lock` and
  returns. The linearization point is the queued→running launch claim in that
  same lock: a cancel that commits first means the item never reaches a
  provider. A running item's own child is signalled — only actual live child
  handles this supervisor created, never a pid read from a file. Cancellation
  captures generation-safe process references, freezes the tree, then signals
  every captured live node. Linux uses pidfds. Darwin uses the kernel-checked
  audit-token signal API and the atomic BSD-info/PID-generation snapshot
  (Apple xnu-12377.81.4 ABI). An auto-reaping parent is not a reason to skip
  a live child. A PID-generation refresh after exec requires the same 64-bit
  process unique ID; recycled PIDs are never substituted.

  MSYS2 gives each item a retained Job handle. Before any payload work, the
  trusted child joins that Job, closes its temporary handle and acknowledges
  admission. The event loop reads this acknowledgement outside the state lock,
  then rereads cancellation under the lock before sending a nonblocking GO
  byte. Prompt transfer starts only after GO. Before GO, only the retained
  unreaped direct child can be signalled by its POSIX PID; after GO, the
  retained Job is the cancellation authority. Normal root exit also cleans
  any residual descendants, including separate sessions and exec helpers.

  The supervisor reaps its own direct child, then requires `kill(pid,0)` to
  report ESRCH for every captured PID. Zombies do **not** count as complete.
  Host reaping gets a bounded five-second observation window. Missing identity
  APIs, denied process inspection/signalling, or a host PID 1 that does not reap
  adopted children cannot produce `cleanup:"complete"`. There is no waiver for
  containers without a proper reaper. MSYS2 additionally requires empty Job
  accounting and strict absence of each captured MSYS PID or explicit native
  PID alias within a bounded observation window. Query failures and lost
  root-reaping ownership remain unknown. New execution is refused on hosts without
  the required generation-safe signalling seam; read-only metadata remains usable.

Retry snapshots are write-once. Under both owner and state locks, an existing
snapshot is reusable only when its regular-file bytes exactly equal the current
terminal attempt's serialized snapshot. Thus a crash after snapshot publication
or failure to commit the new projection can be retried without deleting history.
Inconsistent history and symlinks remain refused. Selected items receive new
attempt-scoped logs; carried items retain their original paths and bytes.

Removal acquires job ownership, rereads terminal state under the state lock,
and renames the directory to a `.removed` tombstone before unlinking files.
A concurrent retry cannot own an unlinked live namespace. An unlink failure
reports incomplete cleanup and leaves the tombstone for manual recovery.

Submission uses one 15-second deadline for both private payload transfer and
acknowledgement. Both descriptors are nonblocking and waits use `tny_poll`.
SIGINT/SIGTERM (CLI) or the tool cancellation callback can interrupt that wait.
A stopped reader cannot strand submit/retry in a blocking write. After a worker
exists, timeout/interruption returns the durable job ID with uncertainty; it
never invents a competing terminal outcome or prints private payload data.

## Output reservations

Every image destination is resolved to a canonical path and claimed under a
per-output lock. Symlinked, multiply linked, non-regular and duplicate
destinations are rejected before any paid call; an existing file needs an
explicit `--overwrite`/`overwrite`. Two submitters racing for one output leave
exactly one paying. Reclaiming another job's abandoned claim requires that
job's owner lock to be actually free while the reservation lock is held
throughout the probe, state read and rewrite; an active or merely uncertain
owner always denies the reclaim without waiting. Claims are released at
terminal completion with proven cleanup, so later iteration on the same path
stays possible — a changed file is caught by the recorded hashes instead.

A supervisor persists `cleanup_hold:true` before acquiring item children, so a
failed cleanup or final write cannot leave reclaimable claims after owner loss.
Unknown cleanup is latched in each item's result transaction, even while other
items run. Only a committed terminal result with proven complete cleanup clears
the hold. A failed protective write starts no item. While held, claims remain
unavailable after the supervisor exits; same-job retry and removal also refuse
before changing the record
([ADR 0101](adr/0101-uncertain-job-cleanup-reservations.md)). Malformed latch or
cleanup data fails closed. An absent or false latch permits unknown cleanup
reclaim only for the exact canonical owner-loss interruption, after checking
that the actual owner is free under a nonblocking state lock. This preserves
abandoned-owner recovery before the protective hold, without releasing a
supervisor-observed uncertainty or an unfinished protected lifecycle.

## Retry

`tny jobs retry <id> --failed` (or `--items 0,2`) starts attempt *N+1* for the
selected failed, cancelled or interrupted items only. A successful item can
never be selected, by default or explicitly. Before a single new request:

- every carried ask success must still have its stored session, unchanged
  answer hash and unchanged event log;
- every carried image success must still have its recorded output bytes, and
  — once the image service records one — a generation manifest that still
  names exactly those bytes.

A mismatch is `JOB_STALE_SUCCESS` with **zero** provider requests and guidance
to submit a new job. The new attempt resets the selected items' cancellation
flags, timing, errors and exit codes and tags them with the new attempt, so a
cancellation aimed at an old attempt can never apply to the new one. The
finished attempt is snapshotted immutably as `attempt-<n>.json`.

A retry takes the job's ownership lock **before** it changes anything, exactly
as submit does, and then re-reads the record under the state lock and refuses
to commit unless the revision, attempt and terminal state are still the ones
it prepared against. So two retries of the same finished job produce one new
attempt and one supervisor: the contender that loses leaves `job.json` byte
for byte as it found it, resets no live item and writes no `attempt-<n>.json`.
Once the new attempt *is* recorded, any later failure is reported terminally
(`JOB_IO_FAILED` / `JOB_OUTPUT_RESERVED`) rather than left queued with no
supervisor.

Credentials always come from the retrying caller; nothing is stored.

## Privacy and secrets

The job record stores the prompt and settings needed for an explicit retry.
`--no-store-request` (`persist_request:false`) keeps the prompt and reference
paths out of `job.json`, the logs and every other file in the job directory —
the supervisor holds them in memory for that one run, and retrying such an
item is refused with guidance to submit a new one.

Credentials and private base URLs travel only through an anonymous payload
pipe and a private child environment. They never appear in argv, in the job
record, in a log or in an error.

Chat and image jobs are two separate allowances. An ask item child receives
the selected conversation provider's resolved credential and nothing else; an
image item child receives the ChatGPT image allowance and nothing else. The
split covers the *inherited* environment too, not only what the supervisor
sets: children inherit only an explicit operational allowlist (home, path,
temporary/configuration directories, locale and certificate locations). Unknown
names are removed, including custom provider `api_key_env`, derived provider
keys/URLs, and arbitrary `--api-key-env` names. Resolved
credentials are then appended from exactly one kind-owned mapping. Shell tools
inside ask jobs therefore do not inherit arbitrary submitter variables. An
operational variable whose value equals a resolved credential is also removed. When the selected conversation provider *is* the
ChatGPT account (`--provider codex`), that account is the ask item's own chat
credential and travels with it — separating the two kinds never breaks a
legitimate Codex ask job. Child stderr is discarded and replaced with
safe local categories; no provider body reaches durable state. An item's own
stdout is its requested output (canonical events for ask, result metadata for
image) and is private (0600). Prompts and generated content can of course
contain anything the user put there — the guarantee here is about tny's own
configuration and provider diagnostics.

### Declared ask agent/tool environment

User settings can explicitly retain up to 32 environment names for ask items:

```json
{"jobs":{"ask_env":["GEMINI_API_KEY","MCP_AUTH_TOKEN","SSH_AUTH_SOCK"]}}
```

This is the exact mapping for MCP environment-variable
expansions, and shell tools that need inherited authentication. Values resolve in
the submitter and travel only in the private payload and ask child environment,
not in job metadata or argv. Image items never receive this map. Image account
names/values (including aliases) and `TNY_*` harness controls are refused, not
silently borrowed. Missing optional variables stay absent. This is explicit user
settings authority, not an agent-supplied job request field.

MCP configurations that declare literal per-server `env` values retain their
existing MCP-owned mapping. For inherited/expanded values, declare those names
in `jobs.ask_env`. No arbitrary environment is silently passed through;
undeclared tool variables require this mapping. No live MCP account compatibility is claimed by the fake-credential checks.

### Canonical image transaction integration

The temporary CLI staging/publish implementation has been removed. The private
`--job-no-replace` prefix sets the canonical service request's `no_replace`
policy. An isolated worker build without that approved service interface refuses
this prefix before spending; it does not fall back to replacement or duplicate
image generation logic. Integrate the approved canonical service and retain its
newer CLI (including export/replay), rather than replacing it with an older
worker CLI. The canonical CLI already routes this private prefix into the service.

A carried image success must name a generation manifest. Legacy records without
one are stale, not verified success. The mixed success/failure manifest retry
oracle remains in place for the canonical overlay.

## Permissions

Each operation has its own exact identity: `job_submit`, `job_cancel`,
`job_retry`, `job_rm`, and the read-only `job_status` shared by status, wait,
logs and list. None is a "safe" tool, so all of them need an explicit grant
outside yolo mode. Reading status or logs never authorizes submitting,
cancelling, retrying or overwriting. The detail a rule matches names the job
id, item indexes, provider, output and reference paths, overwrite, concurrency
and a digest of the exact request — never a prompt and never a credential.

Inside a tool call, use the typed job tools or `tny jobs …` with job-specific
options. Leading globals other than `--json` are refused for intercepted job
commands, so they cannot bypass the operation's permission identity. The regular
CLI still accepts global options before `jobs`.

Settings example:

```json
{"permission": {"job_status": "allow", "job_submit": "deny"}}
```

## Limits

| Bound | Value |
| --- | --- |
| Items per job | 64 |
| Concurrency | 1–16 (default 2) |
| Prompt | 64 KiB |
| Request document | 1 MiB |
| Private payload | 4 MiB |
| Item log | 4 MiB; crossing it cancels that item with `JOB_OUTPUT_LIMIT` |
| Log read | 256 KiB per `logs` call |
| Image references | 5 per item |

An item that floods its log is stopped and reported, never truncated into a
success.

## Failure codes

`JOB_INVALID_REQUEST`, `JOB_UNSUPPORTED`, `JOB_NOT_FOUND`, `JOB_BUSY`,
`JOB_OUTPUT_RESERVED`, `JOB_OUTPUT_EXISTS`, `JOB_SUBMISSION_UNCERTAIN`,
`JOB_STALE_SUCCESS`, `JOB_NOTHING_TO_RETRY`, `JOB_REQUEST_NOT_STORED`,
`JOB_OUTPUT_LIMIT`, `JOB_INTERRUPTED`, `JOB_WAIT_TIMEOUT`,
`JOB_STATE_UNREADABLE`, `JOB_IO_FAILED`.

## Referencing image jobs

`tny image preview --job ID --item N` selects a successful image item without
spending another image request. `tny image edit --job ID --item N --output-file
NEW < prompt.txt` uploads that item as one final reference. Typed tools and
terminal interception share the same owned selection (ADR 0098).

Selection reads bounded confined metadata without projecting state or reading
image bytes. It pins the producing hash/length and both producing and projection
attempts; the selected item may succeed while other items remain running or fail.
A declared manifest must agree. Permission-time job/manifest changes cannot
substitute inputs; loaded bytes still must match the pinned identity.

Image jobs persist manifests by default. `jobs submit image --no-manifest` or
an image request item's `persist_manifest:false` explicitly opts out. The producer
still supplies a SHA-256, length and operation identity; jobs verify disk bytes
against those values rather than hashing a replacement into success. Such an
item can be selected for preview/edit, but cannot satisfy the stricter
manifest-backed carried-success check of a later paid selective retry.

## Tests

`tests/integration/test_jobs.py` drives real detached children against a
loopback provider under a throwaway HOME: durable ids before completion,
survival of the submitter's process group, mixed batch outcomes, queued and
running cancellation with an unrelated sentinel, supervisor loss, forged pids,
the concurrency bound against an independent counter, selective and stale
retry, simultaneous retries of one finished job against a real lock barrier,
chat/image credential isolation observed from inside an item child, privacy
opt-out, output aliases and collisions, the wait timeout, the log bound,
stdout hygiene, permission identities across tools and interception, and the
wasm refusal. Unit coverage for the descriptor handover, the parent watch, the
argv grammar, alias rejection, the credential split and the carried-manifest
check lives in `tests/test_core.c` and `tests/test_intercept.c`.

`test_jobs_msys.py` runs real native MSYS2 CLI flows, separate-session process
trees, all four stopped admission boundaries under individual cancellation
and supervisor loss, and a compiled root-wait fault. The Windows x64 CI job
runs it against the release executable. `test_jobs_cleanup_hold.py` exercises
post-owner-exit reservation, retry and removal refusal, malformed records and
the allowed abandoned-owner reclaim paths.

### Bounded startup diagnostics

A native DAG child can record a private, write-once category for failure during
initial engine startup. The supervisor reads it only after failed-child
cleanup and exposes `items[].startup_error_code`. Categories are limited to
`MAILBOX_BUSY`, `MAILBOX_IO`, `CONTEXT_PERSISTENCE`, and `PROVIDER_START`; no raw
child stderr, provider body or credential is published. The run/task/attempt and
private capability are validated. Absence or an unreadable sidecar is **unknown**,
not proof of success or permission to replay external effects. Retry clears the
current projection while retaining earlier attempt evidence.
