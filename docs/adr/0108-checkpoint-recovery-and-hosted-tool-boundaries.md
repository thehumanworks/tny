# 0108 — Checkpoint recovery and hosted tool boundaries

Status: Accepted — 2026-09-13.
Amends ADR0106/0107 following the single implementation review. ADR0104 writer
ownership and the original BG1–BG7 invariants remain unchanged.

## Decision

Only Left with an empty composer arms an active foreground turn. Nonempty drafts
and focused input retain cursor editing. `/agents` and Ctrl-X return immediately
to the list from an attached background view, without another restart. Dashboard
status is the saved terminal status when done/error/interrupted; JSON `running`
means actively working, while `live` means a writer remains available for owner
attachment. Live idle writers keep their lock until actual teardown.

A hosted Codex search completion counts as the selected tool boundary. Its
outstanding Responses stream must first finish, so all output items, annotations
and pending functions are known. Park before the first local function, allowing
consumed index zero only with a saved completed hosted search and a nonempty
pending batch. A hosted-only final answer finishes and opens the completed list.
Never replay the search or attempt to resume a partial provider stream. Raw hosted
items and final annotated messages are retained only in builtin native Codex
search mode; other Responses providers keep their previous follow-up shape.

An unconsumed disk continuation has an explicit reader: `tny resume ID` or
selection from `tny agents` restores its native engine and continues without a
new user message. The checkpoint saves effective noncredential context plus
SHA-256 identity fingerprints for secret-bearing endpoint/header/settings and
repository configuration. Normal provider resolution supplies credentials again;
configuration mismatch, wider permissions/tool access, or unsupported native mode
refuses recovery before pending effects. Last-used provider/model bookkeeping
is excluded from the fingerprint; the saved effective model is restored.
Credentials, raw private URLs/settings and provider turn affinity remain IPC-only.

Before releasing pending effects, the sole writer atomically marks the checkpoint
consumed. An already-consumed checkpoint after a later arbitrary crash is refused,
not replayed through ordinary transcript repair. This intentionally distinguishes
safe handoff recovery from unknowable external effects after activation. Completed
turns remove the checkpoint. There is no automatic reboot daemon. A child failure
before RUN is rolled back in the foreground; a failure after RUN but before
consumption leaves a directly resumable saved continuation. Failure diagnostics
must explain rejection, never silently hide it as quiet prewarm.

A fresh Python extension interpreter receives `session_start` with reason
`background_resume` before resumed hooks. Event/agent sequence counters continue;
user_prompt_submit, agent-start and turn-start effects are not replayed. External
MCP/extension heap state reinitializes rather than being falsely serialized.
Observer socket clients reconnect after exec; the unique owner connection,
listener and writer descriptor transfer continuously. The last owner-wire stderr
drain precedes the snapshot; later teardown diagnostics go only to task.log.

Only handoff-origin pending permissions park for owner reattachment (five-minute
bound). Existing unattended `ask -B` retains prompt-denial behavior. Host-backed
background rows resolve their actual backend for later prompts; the dashboard
itself starts no provider. Native handoff remains unavailable for host loops,
wasm, ephemeral and deliberate in-process turns.

DuckDuckGo redirects are decoded from `uddg` to the source HTTP(S) URL and `rut`
tracking is discarded. Challenge detection uses known challenge form/script
markers rather than ordinary words such as captcha. Strict DDG HTTP/body errors
remain distinct from existing `web_fetch` status/body and truncate-and-bound
semantics. Search overrides retain precedence.

## Verification

The PTY/loopback fixtures cover post-G rollback, post-RUN unconsumed recovery and
configuration rejection, empty/nonempty composer behavior, repeated detach/list
without PID changes, pending permissions, extension initialization, completed live
status, ACP followups and native hosted-search/local-call interaction. Tests also
cover ordinary Responses echo, DDG redirect/challenge parsing and 404/oversized
fetch results. Current commands/results are recorded in the verification evidence;
no live-provider or independent-review outcome is implied by this decision.
