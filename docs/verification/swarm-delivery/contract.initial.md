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
