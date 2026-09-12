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
    {"prompt": "review src/core/jobs.c"},
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

A supervisor that finishes with uncertain cleanup atomically records
`cleanup_hold:true`. Its claims remain unavailable after the supervisor exits;
same-job retry and removal also refuse before changing the record
([ADR 0101](adr/0101-uncertain-job-cleanup-reservations.md)). Malformed latch or
cleanup data fails closed. An absent or false latch permits unknown cleanup
reclaim only for the exact canonical owner-loss interruption, after checking
that the actual owner is free under a nonblocking state lock. This preserves
abandoned-owner recovery without releasing a supervisor-observed uncertainty.

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
keys/URLs, `CURSOR_API_KEY`, and arbitrary `--api-key-env` names. Resolved
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

This is the exact mapping for selected named ACP agents, MCP environment-variable
expansions, and shell tools that need inherited authentication. Values resolve in
the submitter and travel only in the private payload and ask child environment,
not in job metadata or argv. Image items never receive this map. Image account
names/values (including aliases) and `TNY_*` harness controls are refused, not
silently borrowed. Missing optional variables stay absent. This is explicit user
settings authority, not an agent-supplied job request field.

MCP configurations that declare literal per-server `env` values retain their
existing MCP-owned mapping. For inherited/expanded values, declare those names
in `jobs.ask_env`. Named ACP command/model settings still reload through the
selected `acp@NAME` profile. No arbitrary environment is silently passed through;
undeclared external-agent/tool variables require this mapping. No live ACP or
MCP account compatibility is claimed by the fake-credential checks.

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
