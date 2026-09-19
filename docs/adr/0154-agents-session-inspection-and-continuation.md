# 0154 — Agents session inspection and continuation

Status: Accepted — 2026-09-19. Implemented and independently reviewed.
Native gate results and platform limits are recorded in
[the evidence](../verification/agents-session-continuation-evidence.md).
Requirements: A1–A8 in [the verification contract](../verification/agents-session-continuation.md).
Amends ADR 0107's dashboard selection behavior and ADR 0108's activation of a
saved checkpoint on selection. Preserves ADR 0104's ownership-through-quiescence
barrier and ADR 0058's owner/observer/tool authorization. CLI `tny resume` is
unchanged.

## Context

A terminal turn status is not the end of its conversation. Completed background
sessions must remain readable and continuable after their runner exits. Conversely,
a live runner may retain the writer lock after saving `done`. A failed socket
connection or terminal status is not authority to replace that writer.

Requiring attachment to inspect saved text creates a dead end when another owner
is attached or the runner is unreachable. Resolving credentials or activating a
disk continuation merely to open a row also turns inspection into execution.
The background detach-on-exit flag alone cannot express whether the TUI owns a
writer or only holds a read replica.

## Decision

### 1. Enter attaches when possible; otherwise it inspects

Keep successful live-owner attachment on Enter, including mid-turn and to an idle
completed runner. Use the unique owner handshake, not an observer connection
promoted locally. Attachment preserves the ongoing turn, effective permission
mode, pending decision, model and workspace. It may consume the existing stream;
it never reposts that turn.

If attachment is unavailable, open a clearly labeled saved read-only transcript
with continuation guidance. A rival owner, held lock or unreachable runner cannot
prevent inspection of readable saved text. Completed/unlocked selections also
open read-only. The dashboard and saved view do not resolve providers, refresh
credentials, start runners, activate checkpoints or save session/settings/auth
stores. Unavailable provider configuration does not hide the conversation.

Stored status and liveness remain separate: `running` describes active work;
`live` describes a held writer, not a promise that the owner slot is available.
A completed live runner still reports `done`.

### 2. Continuation requires explicit action and actual ownership

A submitted prompt from a view without a saved checkpoint explicitly
requests a new turn on the selected session. `/continue` in a background view
acquires or attaches that same session, retaining its ID and history; it never
selects `last`. Outside a background view, `/continue` still resumes the latest
workspace session.

For a held writer lock, try the existing runner's owner handshake. A rival owner
or unreachable runner refuses execution without creating a writer, replacing a
socket or persisting a local replica. Failed probes never grant authority. Leave
the saved view readable and retryable after ownership becomes available.

For a new runner, acquire the writer and reload the saved session while retaining
that lock. Use this fresh snapshot for task reconciliation, provider/model/workspace
selection and checkpoint checks before execution; do not execute the possibly
stale inspection copy. Resolve execution configuration only on this explicit path.
A failure leaves inspection available, not an unlocked in-process writer.

Preserve [ADR 0104](0104-runner-quiescence-ownership.md): acquire before binding
or child storage writes; hold ownership through finalization, engine/MCP shutdown,
final save, log flush and socket removal; release immediately before `bye` after
the last mutation. `done` and `turn_end` alone do not establish quiescence. Reload
under the acquired writer without a release/reacquire gap. No failed acquisition
may mutate the existing listener, pid, log or session.

### 3. Inspection and replica mutations have separate guards

A saved read-only view permits only `/help`, `/clear`, `/transcript`, `/copy`,
`/trace`, `/agents`, `/quit`, `/exit` and `/continue`. `/clear` changes the display,
not the saved conversation. `/cancel` requires an attached runner. All other
commands are blocked, including `/new`, `/reset`, `/resume`, `/rename`, `/compact`,
`/undo`, settings/provider changes and auth operations.
Entering the dashboard cancels unfinished provider setup. Submission also discards
stale wizard state in saved/background replicas before routing commands, so setup
answers cannot write settings and `/continue` cannot become a wizard answer.

An attached background view also blocks direct mutations of its local replica
and settings/auth commands. Authorized prompts, steering, permission replies and
cancellation use the owner channel; the runner remains the sole session writer.
Read-only/save authority is distinct from detach-on-exit state. Exit and cleanup
must not save an unowned snapshot or attached replica merely because a background
flag changed. No ownership is inferred from a rendered status or previous attach.

### 4. Configuration follows the selected session, within stored limits

Live attachment uses the existing runner's effective configuration without local
provider resolution. New execution uses the selected session's saved provider,
model and workspace plus current settings and launch flags for remaining options,
including the selected workspace's current configuration. Saved task reconciliation
and existing resume validation still apply.

Legacy missing metadata uses the existing fallbacks: missing provider selects
`openai`, missing model uses the selected provider's current default, and missing
workspace keeps the current workspace. These are fallbacks, not evidence of the
original configuration. Do not claim exact restoration of historical permissions,
effort, credentials, SSH settings or other options never stored.
Present-but-unavailable provider configuration is an execution error, not a reason
to hide saved text or silently switch accounts. This decision does not expand
historical configuration persistence.

### 5. Only explicit recovery activates a saved checkpoint in the dashboard

Opening a row never activates a saved disk continuation. A typed prompt is
refused while any saved `continuation` object is present, including consumed or
invalid checkpoints: **the prompt is not submitted or queued**. The diagnostic
directs the user to `/continue`; it must not imply that the rejected prompt will
run after recovery.

`/continue` explicitly authorizes validation through existing recovery, which
rejects consumed or invalid work. Recovering valid unconsumed work resumes the
retained turn without adding a new user message. It **may release retained tool
calls and other effects**, not merely replay display text. CLI `tny resume ID`
remains an explicit recovery entry point with its existing behavior; the
additional dashboard action does not apply to that CLI command.

Keep [ADR 0108](0108-checkpoint-recovery-and-hosted-tool-boundaries.md)'s endpoint,
settings and repository identity checks, saved effective options, normal credential
resolution and refusal of wider permissions/tool access or unsupported modes.
Before pending effects, the sole writer atomically marks the validated checkpoint
consumed. Already-consumed or invalid checkpoints are refused rather than
replayed through ordinary missing-result repair. Completed turns remove their
checkpoints. Exactly-once guarantees concern the controlled handoff/recovery
protocol, not
unknowable external effects after arbitrary crashes. There is no automatic reboot
execution or reconstructed external MCP/extension heap state.

### 6. Background exit and unsupported modes remain safe

`/agents` and Ctrl-X return to the dashboard without restarting the turn. Leaving
a background view or dashboard detaches work without saving the replica. Explicit
cancellation is available to an attached owner; an unowned saved view has no
cancellation authority. Ordinary foreground quit/interrupt behavior and bounded
cancellation are unchanged.

Starting a new runner from a saved background view is unavailable on wasm and
in-process paths, including `TNY_ISOLATE=0` and macOS post-TLS containment. Refuse
before mutation; never fall back to an unlocked in-process writer. A successful
live attachment can remain usable where the existing runner is supported, even
when a replacement runner cannot be started. Ephemeral mode continues to refuse
saved-session import. No new OS seam or public ABI is introduced.

## Alternatives and scope

- Refusing every failed attachment hides readable saved history.
- Treating every row as disconnected would break working live-owner attachment.
- Using `done`, a failed probe or a delay as ownership would violate ADR 0104.
- Automatically recovering on selection conflates inspection with authorization
  to release effects; treating a typed prompt as recovery would obscure its fate.

Force takeover, a new daemon, live inference and full historical configuration
persistence are out of scope. Existing CLI resume, stop and steer controls are
not redefined by this dashboard decision.

## Verification mapping, not results

The table maps the required semantics to this decision. It is not test evidence.
Implementation checks and independent review belong in the parent-owned
[verification record](../verification/agents-session-continuation.md); no passing
gate, live-provider outcome or acceptance is claimed here.

| Requirement | Contract in this decision |
| --- | --- |
| A1 | Sections 1–2: inspect completed sessions after runner exit; explicitly continue the same ID and history. |
| A2 | Section 1: saved read-only fallback for rival owners, held locks and unreachable runners, with continuation guidance. |
| A3 | Section 1: no provider resolution/refresh, new runner, checkpoint activation or session/settings/auth save on inspection. |
| A4 | Section 2: owner handshake or writer acquisition and fresh-under-lock reload; no stealing or socket replacement; ADR 0104 unchanged. |
| A5 | Section 3: allowlisted read-only commands, attached-replica mutation guards and no exit save; section 2 keeps failed continuation retryable. |
| A6 | Sections 1 and 4: retain live runner configuration; new execution uses selected metadata with current settings/launch flags and honest legacy limits. |
| A7 | Section 5: any saved checkpoint blocks prompts, without submission/queueing; `/continue` validates and rejects consumed/invalid work before recovering eligible retained work. |
| A8 | Section 6: background exit detaches, foreground exit/interrupt is unchanged, unsupported new-runner paths reject before mutation. |
