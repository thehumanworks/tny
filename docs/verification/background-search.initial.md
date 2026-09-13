# Verification contract: native web search and durable background agents

Status: contract reviewed and clarified before implementation; implementation and evidence pending.
Date: 2026-09-13. Baseline: 948d3b6 (clean main).
Scope: root C11 tny harness only, not tnytty or unrelated open issues.

## Goal and delivery gates

Implement Codex native web search (DuckDuckGo fallback if unavailable), and an
explicit left-arrow handoff that checkpoints after the next completed tool call,
restarts the ongoing turn as a detached background task and permits live
reattachment. Show running background sessions in a dashboard reached by that
handoff and by `tny agents`. Compile, verify locally, commit, push and open a PR.
CI completion is not a prerequisite.

## Invariants and required evidence

| ID | Invariant | Proof required |
| --- | --- | --- |
| WS1 | Confirm actual Codex ChatGPT Responses capability, not obsolete app-server behavior; enable provider-native search only on supported requests. | Release-pinned primary-source research, strict request fixture and a live capability probe. |
| WS2 | Native web search events/results/citations survive streaming and session persistence; hosted tool calls are not executed again as local functions. | SSE unit/integration fixtures (including split input, failed search, repeated events and follow-up turn), live tny search. |
| WS3 | Other providers retain usable search, explicit user search settings retain precedence, no false native support on a custom profile named codex. | Grok search end-to-end and provider-selection fixtures; add DuckDuckGo where native unavailable, with encoded query, bounded I/O and honest errors. |
| BG1 | Left arrow in an active conversation arms backgrounding, does not immediately cancel or replay work, and only hands off after a completed tool result is saved. | PTY test during model streaming and during a tool, marker proving exactly one execution. Protect focused permission/modal input and ordinary idle editing. |
| BG2 | The remaining turn resumes without an extra user prompt, preserving completed results, pending tool batch/order, reasoning items, turn step limits, queued steering and effective provider/model/task/cwd/permissions. | Deterministic multi-tool continuation fixture, interrupted handoff cases, and Grok grok-4.6 live PTY scenario. |
| BG3 | Handoff uses a safe fresh-process boundary; it does not run arbitrary fork-only C after macOS TLS or other threads. | Process architecture ADR, tests after foreground TLS/multiple turns; real child PID/process-lifecycle evidence. |
| BG4 | Exactly one writer owns session files through teardown and handoff. Atomic durable checkpoint, restrictive file/socket permissions, bounded framing; no plaintext credential serialization or secret logs. | Existing runner/session ownership tests plus launch/checkpoint failure, duplicate handoff and concurrent attach cases. |
| BG5 | Background work survives TUI quit/terminal loss, can be reattached during the same turn, and remains resumable after completion. Explicit cancellation remains bounded and truthful; no orphan tools. | Kill original TUI after successful handoff, attach mid-turn, observe completion, resume and stop tests. |
| BG6 | `tny agents` opens the dashboard immediately; handoff opens the same list. It identifies running background sessions accurately, refreshes state, selects/reattaches them, handles empty/stale/completed entries without starting an unrelated provider. | CLI/help and PTY dashboard fixtures; live dashboard/reattach evidence. |
| BG7 | Repeated Left is idempotent; turn-without-tools finishes safely; errors/cancel before boundary do not create background work; unsupported ephemeral/wasm/host cases are explicit and documented rather than false success. | Negative/edge fixtures and wasm compile-compatible stubs where applicable. |
| Q1 | C11, minimal maintainable implementation, existing style and ABI respected; no new heavyweight runtime; existing tests and compilation stay green. | Local release build, `make test`, `make quality`, relevant leak/mutation checks, stripped byte size. New test paths reflected in Nix manifests. |
| Q2 | Architecture decisions and changed CLI/TUI/backend/session behavior are documented consistently. | New monotonically numbered ADRs in docs/adr, docs updates, evidence matrix with actual commands/results. |
| R1 | This contract is reviewed by Claude Code `--model fable --effort high` before implementation; feedback actioned. | Saved reviewer result and disposition. |
| R2 | Exactly one independent implementation review uses fresh Claude Fable high; all actionable findings fixed with regressions. | One review invocation/result, finding dispositions and rerun checks. Contract review is separate. |
| E1 | Direct tny end-to-end testing uses provider grok and model grok-4.6, proves search and mid-turn background/dashboard/reattach. Codex-native capability also independently tested. | Redacted actual transcripts/session IDs/assertions, no fabricated success or substitutions labelled as live Grok. |
| D1 | Only scoped changes staged, committed, pushed; GitHub PR opened after successful local compile. | Commit SHA, clean git status, remote branch and PR URL. No wait for CI. |

## Research questions to resolve before final design

The root docs describe a native Codex subscription profile (ADR0065), although
AGENTS.md still names the old app-server architecture. Tool-end events currently
precede result persistence in complete_tool; handoff must not trigger from that
renderer notification. ADR0104 requires ownership through final save/socket
unlink. A fresh exec boundary is needed after macOS SecureTransport initialization.
Research durable checkpoint-and-exec versus retaining a live worker, exact
pending-batch representation, owner reconnection, and failures during transfer.
Host-managed turns cannot be reconstructed as a native transcript without a
host continuation API; define their behavior explicitly. Prefer reusing existing
session store/runner/control abstractions rather than introducing a daemon.

## Evidence policy

Populate evidence only after observing the command/result. Environment blockers
are recorded explicitly and are not passing checks. Raw logs and credentials
stay outside the repository; commit redacted summaries and deterministic tests.
Do not weaken an invariant merely to match implementation. Keep every delivery
gate visible until final reconciliation.

## Binding clarifications after Fable review

See [review and dispositions](background-search-contract-review.md). The
following clarify, rather than relax, the acceptance invariants:

- BG1/BG2: after the effective (post-extension) tool result is appended, atomically
  save the per-call checkpoint BEFORE the next tool or model request. It records
  completed calls plus remaining calls in original order and the native loop
  state required to continue them. Do not let normal resume synthesize failures
  for those remaining calls. A cancellation result never triggers handoff.
- BG2/BG4: preserve effective effort, tool profile, system prompt, pending steer,
  images, usage/audit and permission/batch state as well as the original fields.
  Inherit the writer lock's open file description across fresh exec, with private
  bounded context transport and child-ready acknowledgement. Retire the old
  engine before the successor mutates state. Never use a save-after-handoff race.
- BG3: compare retention of the live runner with checkpoint-and-exec in the ADR,
  but implement restart because the user requested it. Verify executable/process
  identity changes and no repeated tool side effects. No fork-only TLS hazard.
- BG5/BG6: persist background origin; reattach as owner only when unowned; refresh
  pid identity after restart. Quitting a reattached durable session detaches;
  explicit cancellation still stops. Include a noninteractive agents listing.
- BG7: Left only arms with empty composer and no focused modal; show armed state,
  preserve normal editing. Explicitly define ask/auto and in-process behavior.
- WS1/WS2: pin native declaration and hosted-item/citation representation; avoid
  duplicate function-tool names; capability discovery is research-time only.
- WS3: ADR specifies DuckDuckGo endpoint, bounded parsing/fetch and User-Agent,
  challenge/error handling, settings precedence, native vs wasm/CORS behavior.
- Q1/Q2: measure before/after stripped native size against actual Makefile gate;
  Linux's documented 1 MiB budget must not be silently raised. Register fixtures
  in nix/tests.nix and correct scoped stale Codex architecture documentation.
