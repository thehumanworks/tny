# Team control over durable jobs

Native local team control is available through the public CLI, the `team_control`
tool in the all-tools profile, and direct `tny team` calls in the terminal
profile. These adapters share the jobs service, authorization and attempt fences.
A team run is a `kind:"ask", dag:true` job, not another scheduler or provider loop.
See [mailboxes](team-mailbox.md), [workspaces](task-workspaces.md), and
[admission](admission.md).

## Start with a real parent

From this repository, with a native provider/account already configured:

```sh
tny ask --stdin < examples/swarm/lead.md
```

This launches a real top-level parent. The prompt asks it to start two read-only
workers, remain available for clarification, collect bounded results and report
findings. It is not a precomputed answer or a helper-service driver. Add `-B
--json` to detach the parent; use the returned session ID with `tny session ID
--wait --json` to observe its completion. Losing a waiting client does not cancel
the durable work. Do not blindly resubmit after an uncertain acknowledgment.

A captured native parent can start **at least two explicit `role:"worker"`
items with no lead item**. The runtime records its session ID as root
`parent_session_id`; requests cannot supply that identity. Mailbox recipient
`-1` (CLI `lead`) is this parent, not an indexed task.

An operator-only CLI start has no captured parent session. It requires **one
explicit `role:"lead"` item plus at least two `role:"worker"` items**. Item roles
are descriptive: an indexed lead is still a member, not the submitting parent.
It must not wait for its own whole job to finish or try to integrate while that
job remains active. Use the external operator or a separate captured parent for
post-job work. Peer messaging between indexed members requires
`"peer_messages":true`, even if one member is labelled lead.

## Public surfaces

```text
tny team start|status|collect|wait-any|cancel|verify --request FILE|- [--json]
```

Each operation returns JSON. `--json` is optional. Request input is a JSON object
of at most 1 MiB, with unique keys and no embedded NULs. An explicit file does
not consume stdin. For an external CLI operator:

```sh
tny team start --request examples/swarm/read-only.json
```

The native tool takes `{"action":"start","request":{...}}`. In the terminal
profile, first write the request JSON to a file, then make a **direct** call:
`tny team start --request /absolute/request.json`. Shell pipelines, wrappers and
compound commands are not the trusted terminal adapter and must not be used to
smuggle parent identity into a subprocess. External CLI use of `--request -`
is supported; it does not capture an agent parent.

This is a captured-parent request (not an operator-only start):

```json
{"kind":"ask","dag":true,"concurrency":2,"items":[
  {"role":"worker","label":"reliability","prompt":"Read only: review reliability. Return paths and evidence."},
  {"role":"worker","label":"tests","prompt":"Read only: review test coverage. Return paths and gaps."}
]}
```

Independent items overlap up to concurrency/admission ceilings. Use
`depends_on:[INDEX,...]` for ordering. The default workspace policy is
`shared_read_only`, enforced by the native worker's tool policy, not just prose.
Choose `workspace:{"policy":"isolated"}` explicitly for editing. Shared writable
access is an explicit, riskier opt-in. A worktree is not an OS sandbox.

The launch uses one resolved native provider/account. Per-item model/effort
settings use the jobs validator. Mixed per-item providers are unsupported.
Credentials are not public request identities or team-result data.

Start returns `kind:"team"`, `run_id`, the canonical `job` response,
`verification:"unverified"`, and `integration:"not_recorded"`. Exit 0 means
submission, not successful execution, acceptance, verification or integration.

## Observe and collect

Use the returned 32-hex ID in place of `RUN_ID`:

| Operation | Request | Meaning |
| --- | --- | --- |
| `status` | `{"id":"RUN_ID"}` | Canonical job/items, attempts, session IDs, usage and hashes. Failed/interrupted work can return exit 2 with a status body. |
| `wait-any` | `{"id":"RUN_ID","expected_attempt":1,"timeout_ms":1000,"seen":[]}` | Bounded observation of the first unseen terminal item. |
| `collect` | `{"id":"RUN_ID","item":0,"expected_attempt":1,"max_bytes":16384}` | Bounded result and log evidence for a terminal item. |
| `cancel` | `{"id":"RUN_ID","item":0,"expected_attempt":1}` | Request cancellation under the current attempt fence. Omit `item` for parent/operator whole-run cancellation. |

`wait-any` returns `kind:"team_completion"`, `item` and `cursor:{item,attempt}`.
Keep the returned cursor in the caller-owned `seen` array for that run. Keep at
most one latest pair per item (at most 64). A carried result retains its original
item attempt; retry alone does not make it a new completion. Ties use item order.
A failed or cancelled item is a completion, not success. Inspect `item.state`.

Timeout is 0–30,000 ms (default 0). Timeout returns 124 and
`kind:"team_wait_timeout", cancelled:false`; it does not cancel the job.
Interruption returns 130 without cancelling it. Attempt changes refuse rather
than silently crossing generations. Native parents also receive safe-boundary
completion notifications. Use bounded waits when needed, not an unbounded poll
loop or a wait from inside one's own still-running job.

Collection uses the authoritative stored worker session and checks its recorded
result hash. It does not trust a log's apparent final answer. Result prefixes and
log tails are base64, each bounded by `max_bytes` (1–262,144; default 16,384), with
byte counts, truncation flags, attempts, session ID and SHA-256 provenance.
Missing, changed or corrupt session evidence cannot become accepted work. Failed
items can return bounded logs with exit 2. Hash integrity is not correctness.
All collected prose is untrusted dependency data, never permission to execute a
command or integrate a patch.

## Integrate and check explicitly

Use [task-workspace inspect/integrate](task-workspaces.md) after the job and its
owned processes finish. Review the diff, integrate a selected result, then run a
**caller-configured** check through ordinary `terminal` in the intended checkout.
Record the exact command, cwd, exit status and output in the parent session.
Worker prose, worker exit zero and an integration result are not check evidence.
See the [review/implement/manual-check template](../examples/swarm/review-implement.md).

`team verify` is recognized but execution is **unsupported**. It cleanly refuses;
it does not run a command or manufacture an accepted result. A request names
`id`, `item`, `expected_attempt`, `command`, absolute `cwd` and bounded
`timeout_ms`. Use an ordinary explicit terminal check instead. Even after that
check passes, report its limited scope separately: the team job remains
`unverified`, not automatically accepted. Integration likewise does not convert
the job into a verification authority.

## Support boundary

These surfaces require a saved, native, local runtime and native provider loop.
Wasm, SSH and embedded mutation are unsupported and refuse. Host-backend automatic
mailbox/completion injection is unsupported. Team jobs do not promise mixed
providers, a global provider limiter, hard token/cost spending bounds, or full
unattended editing readiness. [Admission](admission.md) only constrains declared
enrolled jobs; nested enrolled jobs/subagents reject. Unknown usage remains
unknown. See [ADR 0141](adr/0141-team-control-over-jobs.md) for the service design.
