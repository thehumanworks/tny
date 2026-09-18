# Shared launch admission and observed usage

Native jobs can enroll in an explicit shared admission scope. The scheduler
claims capacity before launching an item attempt and settles it after owned
process cleanup. This is a launch gate for existing jobs, not another scheduler,
provider loop, account meter or billing system. It applies **only to jobs that
declare enrollment**. Other jobs and ordinary turns are not automatically counted.

## Declare a scope

Add this object to a native ask job request (including a team start request):

```json
{"admission":{"label":"review-batch","provider_scope":"work-account","cap":2,"queue_cap":32,"claim_limit":20}}
```

This is a request fragment, not a standalone start request. See
[team control](team-control.md) for the complete `kind`, `dag` and `items` shape.
The same canonical private root and public scope aliases are required to share
limits across supervisors. Use stable public aliases, not keys, tokens, account
secrets, endpoint URLs or credential hashes. Aliases are 1–63 ASCII letters,
digits, underscores or hyphens. The grammar and secret checks cannot prove an
arbitrary alphanumeric label is not a secret: choose it deliberately.

| Field | Public jobs contract |
| --- | --- |
| `label` | Public workload label. |
| `provider_scope` | Public provider/account alias for the intended shared boundary. |
| `cap` | 1–16 simultaneously held launch permits. |
| `queue_cap` | 1–128 waiting tickets. |
| `claim_limit` | Positive lifetime launch-attempt claim limit for this scope. |

Configuration is immutable once initialized. Conflicting configuration refuses;
there is no automatic reset or refund. Distinct aliases do not magically share a
limit, even if they resolve to the same real account. Local job concurrency is an
additional ceiling, not raised by admission. Independent scopes are independent.
A batch uses one resolved provider/account; mixed per-item providers are unsupported.

Nested enrolled jobs and subagents reject. A worker cannot use another enrolled
launch to bypass accounting. This is not an account-wide restriction on unrelated
processes. Native local execution is supported; wasm, SSH and embedded mutation
are unsupported. Host automatic context injection is not an admission capability.

## Queue, cleanup and recovery

Each fresh `(job ID, item index, attempt)` grant consumes one claim **before**
launch. A failed launch still consumes it. A replay of the same grant does not
charge twice and does not authorize a second launch. A claim is not a provider
HTTP request: one admitted attempt can make many model/tool requests.

Tickets use durable FIFO order. Capacity waits and FIFO waits remain queued;
queue-full, exhausted and history-full states do not authorize execution. The
append-only history has a separate 1,024-attempt bound, including cancellation
tombstones. History exhaustion fails closed rather than discarding identities.

Cancellation removes waiting tickets or records a tombstone. A granted permit
stays held until the existing jobs owner proves process cleanup. `cleanup_hold`
is conservative accounting, not evidence of a running model request. Capacity
release does not refund the lifetime claim. There is no wall-clock expiry or
lease-stealing rule. A crash cannot turn a stale PID or a polling timeout into
cleanup proof. Recovery uses the durable job attempt and owned process identity.
Lock contention is retryable outside locks; corrupt/mismatched state refuses.

## Soft token policy is not a spending guarantee

DAG jobs can additionally request:

```json
{"budget":{"soft_tokens":100000,"unknown_usage":"stop"}}
```

This is also a request fragment. `soft_tokens` must be positive.
`unknown_usage` is `stop` (default) or explicit `continue`. The scheduler uses
observed settled usage to decide whether to launch more work. Already admitted
concurrent work can continue and overshoot. It cannot enforce a hard provider
request, token, cost, subscription or account spending ceiling.

Durable status aggregates known token counts and identifies unknown items.
Missing provider usage remains **unknown**, not zero. A zero known total does
not prove zero usage. Cancelled, failed or incomplete attempts can have unknown
usage; do not estimate it from worker prose. Retry and carried-result accounting
retain attempt provenance rather than charging a carried result as newly run work.
The native child step limit is another execution bound, not a token/cost budget.

Reports should separate launch claims, active/queued capacity, known usage,
unknown usage, execution state, manual check evidence and verification state.
Neither successful admission nor reported usage marks work accepted.
See [ADR 0140](adr/0140-shared-admission.md).
