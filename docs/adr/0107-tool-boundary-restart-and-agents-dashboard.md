# 0107 — Tool-boundary restart and background-session dashboard

Status: Accepted — 2026-09-13.
Requirements: BG1–BG7, Q1–Q2 in docs/verification/background-search.md.
Amends ADR0053/0081; preserves ADR0104's ownership through quiescence.

## Decision

Left in an active native runner conversation arms an idempotent request. The
control socket accepts that flag while terminal waits pump control traffic;
no renderer callback performs a handoff or enters backend dispatch. Idle Left
and focused permission, question, wizard and modal inputs retain editing.
Arming is visible. Explicit cancellation wins; it never counts as completion.

After complete_tool applies extension transformations and appends the effective
result, run_tools advances its consumed index and parks before any later call or
POST. The runner drains events and atomically saves the full session with a
versioned continuation checkpoint, including the pending batch, index, completed
results, step/failure/usage counters, permission state, queued steering, captured
image bytes/origins and runtime lifecycle counters. Pending calls execute through
a private continuation entry point, never ordinary send or transcript repair.
A turn without a local tool finishes safely and opens the same list at turn end.
Hosted search belongs to its outstanding provider response; it is not a local
checkpoint boundary while that response is still streaming.

Spawn the same executable with descriptor mappings. The original runner remains
responsible until the replacement validates its snapshot. Its existing listener,
writer open description and owner connection transfer continuously: no rebind,
unlink, flock gap or sleep pretending to establish quiescence. The child is a new
process group inside the already detached runner session, with no controlling
terminal. Because the spawn seam sets its process group to its own PID, a second
setsid would fail; a new session is unnecessary for this already detached source.
No arbitrary application C runs in a fork-only child after TLS.

Resolved configuration and credentials travel only through bounded anonymous
IPC. No credential, private base URL, settings document or provider turn affinity
is serialized into the disk checkpoint, argv or diagnostics. The public process
entry is an internal descriptor-only protocol, not a supported user flag. Payload
size and each handshake phase are bounded; the child verifies the inherited
writer descriptor against the session's actual lock file.

The handshake is READY (restoration validated), GO (parent engine/MCP quiesced),
COMMITTED (child PID and background marker saved), RUN (remaining effects may
execute). Before RUN, failure kills/reaps only the newly spawned child and
continues the original foreground snapshot. If foreground restoration itself
fails, retain the honest checkpoint and report failure rather than synthesize
pending-tool results. Once RUN is released the old process performs no more
session mutation or lifecycle finalization. Exactly-once assertions concern this
controlled handoff, not arbitrary crashes between external effects and saves.

A durable background boolean distinguishes these sessions, including explicit
ask -B launches, from ordinary foreground TUI sessions. tny agents enters the
shared dashboard without resolving/prewarming a provider; --json and non-TTY
output list the same workspace rows. Writer-lock probes distinguish live and
stale records; completed/error/interrupted background rows remain selectable.
A selectable live row must accept the unique owner handshake. Another owner's
session is refused without changing its writer or socket. Reattachment uses its
actual permission mode and the same ongoing turn; no prompt is required. The
runner's full accumulated output/reasoning and saved transcript remain available.

Leaving a background view or dashboard detaches. Ctrl-C and session stop remain
explicit bounded cancellation, including PID verification for forced stop. A
background permission request waits for owner reattachment (five-minute bound),
then reports timeout/denial; it is never silently auto-approved or implicitly
downgraded because a dashboard is open. Foreground quit retains ADR0081 behavior.

## Alternatives and limits

Simply closing the existing owner connection would preserve in-memory state but
would not meet the explicit requested restart. Reposting with "continue" can
repeat side effects, lose pending calls and reset step budgets. Reopening an
unlocked session or rebinding its socket conflicts with ADR0104. A new daemon,
database, scheduler or public ABI is unnecessary.

Host-managed loops, wasm, ephemeral sessions and deliberate in-process modes
(including TNY_ISOLATE=0 and macOS post-TLS containment without a runner) reject
handoff explicitly. Existing runner-backed native Codex and Grok turns use this
path. The checkpoint is evidence for interrupted recovery; this decision does
not promise automatic post-reboot execution or reconstruction of external MCP
server/Python interpreter heap state. Such processes reinitialize after exec,
while already consumed tool calls and lifecycle events are not repeated.

## Verification

PTY/loopback tests exercise multi-tool exactly-once effects, PID replacement,
steering and reasoning continuity, pending permission/owner-mode preservation,
no-tool completion, competing owners, dashboard exit survival, completed/stale
rows, missing-executable restart failure and cancellation precedence. Existing
runner roles, isolation, interruption, task and library suites remain gates.
Local commands and actual outcomes belong in the evidence record. The supervisor
owns Grok grok-4.6 live testing and the reserved independent implementation review.
