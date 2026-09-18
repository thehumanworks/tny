# Acceptance map

This map distinguishes implemented behavior from unsupported or unverified work.
The chronological evidence and input revisions are in [evidence.md](evidence.md).
It is not an issue-closure checklist until final candidate/CI results are linked.

| Contract | Observable implementation / test | Current limit |
| --- | --- | --- |
| R153 identity and immediate handles | Job ID/run, stable item/task, attempt and captured parent; `JobsDAG`, `test_team_control.py`, real `test_swarm_parent.py` | Roles are descriptive; parent identity/capability, not a role string, grants authority |
| R153 public control | CLI, typed tools and terminal interception; bounded status/collect/wait-any; selected fenced cancellation and unrelated-run sentinel | `team verify` executes no command and returns explicit unsupported/unverified |
| R153 overlap and notifications | Real parent plus two workers in both profiles; busy-tool clarification, safe delivery, completion receipts and run-filtered CLI/PTY tree | Host loops have no automatic native injection |
| R153 worker selectors | Owned per-item native provider/model/effort snapshots; 17 deterministic endpoint/credential/ceiling/retry cases | Explicit selection is limited to safely reproducible shared-workspace, standard-Bearer native configurations; mixed-provider admission is refused |
| R155 durable graph | Versioned opt-in DAG in existing job authority; claim, definition/dependency hashes and immutable attempts | Existing shell/SDK workflows remain explicitly ephemeral; durable mode uses CLI/shared tools |
| R155 recovery | Barrier A/B/C, carried A request count, cancellation/retry race, corrupt input/output, scope/credential changes and supervisor loss (`JobsDAG`, `JobsReview`) | Unknown cleanup remains held; no exactly-once effects or silent replay |
| R156 messaging | 25 service/fault cases plus real parent/member public flow, wrong-member rejection, no tool replay, explicit read/ack/retire | 16 KiB payload, 64 outstanding per recipient, 256 retained run messages; old attempts require explicit retirement |
| R157 file isolation | Two actual workers edit the same relative path in isolated Git trees; launch checkout stays unchanged; explicit conflict preserves both trees | Worktrees are not privilege sandboxes; arbitrary same-user daemonization is not contained |
| R157 ownership and integration | Private provenance, clean/dirty/base/collision cases, owner lock across Git without state lock, stale attempt/caller refusal, repeated cleanup | No automatic merge/removal or acceptance; first-party background terminals are refused in owned jobs |
| R158 admission | Real independent batches and process barrier cap=2; FIFO, cancel/grant races, paused/dead holders, uncertain publication and cleanup holds | Only explicitly enrolled jobs share a declared scope; unrelated programs/SDK loops are not globally bounded |
| R158 ceilings/accounting | Actual inherited step counts, snapshot/checkpoint mutations; attempt totals without carried duplication; soft-token policy and unknown stop/continue cases | Claim limit counts launch attempts, not HTTP calls or money. No reserved model-call pool or absolute admission deadline is claimed |
| R159 context | Python/JS 32-consumer barrier; summaries/finite JSON fields/provenance/base64 slices; exact bounds, mixed edges, cancellation and failure memory | Metadata-only references need application retrieval; no silent truncation or paid summarizer |
| R159 measured behavior | ADR0144 records paired baseline/final heap, RSS, bytes and latency; error-frame retention probes | Memory improvements are measured; no general latency speedup claim |
| RX combined paths | Public captured parent creates workers, exchanges live mail, collects, inspects/integrates and records an explicit terminal check; separate real conflict/client-loss/self-cancel cases | Execution and check transcript evidence never fabricate accepted job state |
| RG native gates | Full test, quality, leaks, SDK/shell and selected ownership/mutation checks, with failures and fixes retained | Recheck effective-base/provider follow-up changes before final publication |
| RG wasm | Actual Emscripten 6.0.8 builds, unsupported-service checks, jobs/OpenAI fixtures and browser smoke | Native assertions skipped by wasm remain native evidence, not wasm passes |
| RG optional/platform | Upstream Linux/macOS CI and SDK platform matrix | Nix unavailable locally; no native Windows or physical power-loss result claimed |
| RD delivery | Independent design, first-slice, recovery, SDK and native risk reviews; PR #165 is published | Native PR and its final hosted checks remain pending |
