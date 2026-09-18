# Swarm delivery verification contract

Base: `89bcd5918da0e225e1806813a206d007daafac0a` (origin/main at start).
Authority: `SWARM_DELIVERY_PROMPT.md`, issues #152, #153, #155–#159.
#154 is a merged Codex usage PR, not a swarm issue. Preserve that work.
No merge, main push, deployment, release, or repository-setting changes.

## Requirements and acceptance invariants

| ID | Requirement | Required evidence | Initial state |
| --- | --- | --- | --- |
| R153 | Async control, immediate handles; roles/run/task/attempt lineage; CLI/tool parity; notifications; verification state | Overlapping fixture workers; responsive lead; membership; bounded collection/cancel; unrelated-session isolation; capabilities | Not started |
| R155 | Opt-in durable DAG over jobs; identities, claims, fingerprints, integrity; inspect/resume/retry | Barrier A/B/C recovery without replaying A; uncertain B fails closed; concurrent/repeated resume; changed input/dependency/revision; corrupt artifact; permissions and cleanup holds | Not started |
| R156 | Durable bounded addressed mailbox; persist-before-ack; safe-boundary delivery and dedup | Busy recipient; noninterrupted tool; crash replay; ordering/duplicates/backpressure/membership; clarification round trip | Not started |
| R157 | Isolated managed editing workspace; explicit integration/provenance | Two workers same filename; launch checkout unchanged; conflicts preserve diffs; dirty/untracked refusal; collisions/cancel/crash/idempotent cleanup/foreign trees | Not started |
| R158 | Shared fair admission, ceilings; attempt usage and honest budgets | Independent batches cap=2; cancel/death/paused/nested races; unknown usage; no double count; hard exhaustion; inherited steps; secret-safe scope | Not started |
| R159 | Lazy admitted context; selective fields/summary/artifacts; complete-input bound | Python/JS blocked 32x256KiB regression; order/no-context/bounds/cancel/failure; original artifacts; baseline/candidate memory, bytes, latency | Not started |
| RX | Integrated collaboration and compatibility | Public lead + 2 workers; busy message; client loss/resume; isolated edit/integration/checks; lineage/usage; failure/cancel; synchronous APIs retained | Not started |
| RG | Quality, ownership, packaging, platforms | make test/quality/leaks, SDK/shell, mutation, Nix/wasm/CI; stripped size <6,000,000 bytes and runtime dependencies | Not started |
| RD | Reviewable delivery with truthful evidence | Docs/ADRs/help/declarations; independent design/code/recovery review; committed branches/PRs and current CI; criterion mapping | Not started |

## Architecture and ownership

Extend sessions/jobs and ownership seams, not a second provider loop or daemon.
The durable controller reuses jobs for execution and retry integrity. New features
are opt-in. Hash integrity is not acceptance; prose never authorizes verification
or integration. Keep C11/public C ABI and authorized private C++ boundaries.
ADR allocation: 0136 control/DAG, 0137 context, 0138 workspace, 0139 mailbox,
0140 admission. Declare native/tools/terminal/host/SDK/SSH/wasm capabilities.
No exactly-once effects, hard monetary guarantees, or worktree sandbox claims.

Lead owns integration, build/Nix registration, shared wiring, final checks,
publication and evidence. Initial worker scopes: control/DAG, SDK context,
workspace safety. Separate branches/worktrees; no overlapping writes or nested
teams. Agree interfaces in task briefs before integrating consumers.

## Quality process

Use existing pinned mise tools and project gates. Never weaken tests or quality
rules. Independent review before implementation and at first meaningful slice
and risky recovery/permission boundaries. Observable public behavior tests and
mutation/fault injection, not source-string assertions alone. Keep failed checks
and fixes. Reuse evidence only with unchanged inputs. Logs are bounded and
secret-free, with no whole sessions. Substantive decisions use docs/adr (the
repository convention overrides the generic skill path).

## Status

This contract does not assert completion. evidence.md tracks candidate SHAs,
commands/exits, incomplete criteria and PR/CI state. Only current passing evidence
makes a criterion verified. Tool/time limitations are blockers, not exemptions.

## Design review amendment 1

Independent reviewer session `4c7e75fd87034639` completed successfully before
implementation. Critical findings: avoid a lost run-to-job binding; trust caller
context rather than supplied membership; retain permission/account ceilings on
retry; distinguish execution/integrity/acceptance; fence consumers by attempt.
Resolution: use the existing durable job ID as run ID, item index as stable task
ID and existing job/item attempt as attempt identity. Extend the jobs DAG schema
and scheduler in place, with no separate controller/job submission transaction.
Thus the jobs record/owner remains the single execution authority. New metadata
must never turn arbitrary session IDs into membership. SDK workflows remain an
explicitly ephemeral mode; no silent claim that they acquire native durability.
The first slice must not claim full #153 or #155 until their remaining public
control/notification/verification and recovery criteria are independently proved.
Additional required checks: lost-ack/concurrent ownership, permission/account
changes on retry, direct-control bypass, referenced-artifact deletion and
controller/client loss. Consumer bindings use run ID + task index + attempt;
Git operations must not run under the job state lock.

## Integration amendment 2

Configured `role:lead` is retained as a member identity, never translated into
the submitting parent's task -1 authority. Its intended mailbox routing policy
is lead-to-worker and worker-to-lead within the same capability-authenticated run;
peer-to-peer requires explicit opt-in. Only the submitting parent/operator may
retire old-attempt queues. This is not authority over arbitrary sessions/jobs.

Shared read-only task policy is an inherited native permission ceiling before
mode/rules/grants, including checkpoint recovery and child launch snapshots.
Worktrees are not sandboxes. Native read commands remain available to terminal
profiles; arbitrary shell scripts, writes and new child launches are denied.
Host-owned loops must refuse this policy when they cannot enforce it.

## Integration amendment 3

The implementation preserves the mailbox helper's stricter authority model:
submitting parent/operator is task -1; all indexed roles, including `role:lead`,
are descriptive. Explicit `peer_messages:true` enables communication between
indexed members. This supersedes amendment 2's proposed role-based routing;
it does not weaken membership or give labels administrative authority. The
one-lead/two-worker fixture opts into peers explicitly and tests addressed delivery.

## Main reconciliation amendment 4

While implementation ran, main advanced to
`ab74e2a442b950dfcbc76963813bcedf093da00b`. The lead preserves its terminal task
completion/observer service, repository-wide agent discovery, Responses optional
argument fix and Linux/macOS CI policy. Nix is now a developer-only upstream
check, not a hosted CI promise. Required native gates are repeated on the merged
candidate; physical power-loss survival is not claimed.

Upstream allocated 0136/0137 concurrently. Our unpublished decisions are renumbered
without changing upstream decisions: 0136→0143 (DAG), 0137→0144 (context),
0138→0145 (workspace), 0139→0146 (mailbox), 0140→0147 (admission),
0141→0148 (control), 0142→0149 (boundaries). Initial contract/brief allocations
remain historical evidence, not current ADR identifiers.

A focused reviewer established a P1 lifecycle gap: first-party detached terminals
could outlive writable job workers while POSIX jobs reported complete cleanup.
Owned job descendants now refuse that launch mode before sandbox/log/fork side
effects, using the inherited job-parent restriction rather than member identity.
Ordinary non-job background terminal completion remains unchanged. This does not
promise containment of arbitrary shell daemonization.
