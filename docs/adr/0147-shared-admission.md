# ADR 0147: Shared launch admission without new execution authority

Status: accepted for the helper slice; scheduler integration pending.

## Context

Issue #158 needs opt-in coordination across independently submitted batches.
Existing local concurrency remains useful but is not a shared ceiling. Jobs
already own execution, attempt identity, process cleanup and recovery. A second
worker daemon, execution registry, or PID lease would duplicate that authority
and could free capacity while real work is still active.

## Decision

Add a C11/private-state helper, `core/admission.c/.h`, with a public C ABI. Use
existing job ID as run, item index as task, and existing attempt as attempt.
A permit authorizes the existing jobs launch only. Only a fresh committed grant
can precede launch; same-attempt replay never grants another execution.

Use a public user label plus resolved public provider/account alias under a
trusted private root. Never derive scope names from credentials. The cap, queue
bound and lifetime launch-request-claim limit are immutable. Each grant consumes
one claim durably before launch. This limits launches, not all provider requests,
steps, tokens, money, or subscription usage. Unknown usage remains unknown.

Use a bounded persistent FIFO and append-only identity history, including
pre-grant cancellation tombstones and released attempts. Limits are 128 waiting
tickets, cap at most 128 and at most 1,024 recorded identities. Saturation has a
structured result; do not prune identities or reset budgets silently. This favors
bounded memory/storage and replay safety over unbounded scope lifetime.

Serialize claim, cancel, hold and release with the existing nonblocking
`jobs_host` flock. Reuse its 0600 atomic writes. A small `util/admission_host` seam
adds bounded no-follow reads and parent-directory fsync. Keep all OS behavior in
the util seam; do not add a platform fork to core. Acknowledgment follows durable
publication. An error after rename has an uncertain result and cannot authorize
launch. No sleeps, provider/Git operations or callbacks run under state locks.

Jobs ownership spans claim/launch. Do not nest job/admission state locks. Release
requires the caller's existing owned-process cleanup proof, including descendants,
or proof no process was launched. The boolean proof argument is a trusted owner
assertion, never a public user option. The helper cannot establish this proof.
PID absence, stopped owners, lost client, lease expiry and supervisor death are
not reclamation rules. Uncertain launch or cleanup retains capacity indefinitely
and is exposed as a cleanup hold. There is no force-release API.

Minimum safe nesting policy: reject all enrolled submissions from enrolled
workers/descendants before enqueue, even into another scope. The scheduler also
passes trusted ancestry to the helper's EDEADLK guard. No implicit permit handoff
or borrowed parent capacity. Direct nonenrolled paths remain outside the bound;
document that limitation instead of claiming global enforcement.

## Consequences

A paused FIFO head blocks later tickets. A crashed uncertain owner can exhaust
capacity indefinitely. These availability costs are intentional: execution safety
and honest uncertainty take priority. Operators need jobs recovery and process
cleanup evidence, not PID-based reclamation. Corrupt/lost storage fails closed;
explicit initialization must not be misused to reset missing history.

Scope storage is single-host and requires trustworthy private directories and
flock/rename/fsync semantics. No distributed or network-filesystem guarantee.
wasm returns ENOTSUP through the existing execution-capability seam. Native
Windows filesystem semantics need validation before claiming support.

## Evidence and delivery boundary

`tests/integration/test_admission.py` builds the real C helper and OS seams and
runs real fork/flock fixtures, cancellation races, paused/killed owners, concurrent
idempotency, cap 2, FIFO, exhaustion, persistence and publication fault tests.
ASan/UBSan and focused format/static checks cover this slice. These are helper
claims, not proof that actual jobs batches obey the shared cap.

The lead owns jobs integration, Makefile/Nix registration, real independent-batch
provider fixtures, usage aggregation, product-wide gates and packaging. Native
explicit child max_steps forwarding is already lead-owned. Detailed API,
recovery protocol, scheduler sequence, limitations and required acceptance
fixture are in [admission.md](../admission.md). No competing jobs fork is added.

## Integrated delivery addendum

Existing jobs now enroll before owned launches and settle only after their cleanup
proof. Independent real fixture batches share cap two. JSON/dashboard report
limits, pending reasons and available usage; attempt accounting avoids counting
carried artifacts twice. A separate soft-token policy stops pending admissions on
observed exhaustion or unknown usage by default, without claiming a spending cap.
Inherited step limits are tested on actual native create/resume requests. Enrolled
nested asks/jobs/subagents reject; first-party detached terminals reject for owned
jobs. Unenrolled programs and arbitrary shell daemonization remain outside the
shared-service guarantee. Absolute admission deadlines and a reserved model-call
pool are not implemented by these launch permits.
