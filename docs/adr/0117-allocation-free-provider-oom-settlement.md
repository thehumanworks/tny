# ADR 0117: Allocation-free provider OOM settlement

Date: 2026-09-16
Status: accepted
Related: issue #138 (P2-I3); ADR 0115, ADR 0116

## Context

ADR 0116 reserves the runtime ERROR/TURN_END pair, but ordinary provider
cancellation still constructs tool results, persists transcript JSON or builds
protocol RPCs. A reserved queue alone cannot settle an active turn without
allocating.

## Decision

The runtime enters a private thread-local emergency settlement scope at the
quiescent provider boundary. It publishes its terminal guard before invoking the
existing cancel callback. Providers inspect the scope without new vtable fields,
public exports, record layouts or event kinds. The scope covers resource release
and reserved event enqueue. Test builds count every allocation attempt after the
provider failure handoff, including the interval before the runtime enters
settlement; public delivery is checked separately for zero attempts.

Providers mark the failure with `tny_alloc_provider_failed()` at the first
quiescent parser/transport boundary and then stop: they construct no further
owner, invoke no tool/control callback and enter no persistence operation once
failure is observed. Request construction failure returns a distinct status,
never a retryable I/O error, and never submits a request.

The native loop invalidates pending custom-tool leases, releases permissions,
parser/response storage and pending images, removes an unsent preview in place,
then returns to idle. It does not manufacture tool results, invoke extension
control hooks or save JSON under OOM. Already committed transcript messages
remain owned; the existing provider-view repair supplies missing tool results
when a later request can allocate. Partial response text is not newly persisted
on this path. Ordinary cancellation is unchanged. Parser OOM after callbacks
have unwound uses the same emergency cancel, skipping usage accounting,
persistence and ordinary finalization even when an earlier step accumulated
usage.

Cursor closes streams and reverse callbacks and stops its owned bridge process
without constructing CancelRun/Shutdown RPCs. It retains the agent identity and
ephemeral store; a later send reconnects and resumes before sending. Cursor
pending completion unlinks the consumed provider lease before result
serialization, and its blocking pump thread transfers failure across
`pthread_join` before owner-thread processing resumes. Store operations stop at
the failed operation; completed writes/deletions are not rolled back. ACP
closes its transport/owned process with a bounded TERM grace, then KILL, and
clears pending permissions/readers; a later send reconnects with the existing
session/load or session/new policy. WebSocket emergency close frees the framing
context without allocating a close frame and disables receive and send once its
reader fails, so the vendored library cannot consume another coalesced frame.
TLS close skips close-notify construction while retaining normal-close SIGPIPE
guards. Closing an attached remote transport is not proof of remote turn
termination. Codex uses the native Responses implementation and shares its
emergency path.

The runtime keeps its pending terminal private until fallible finalization
returns. A finalization allocation failure therefore substitutes the reserved
OOM ERROR/TURN_END pair before any successful terminal is delivered.

## Bounded exceptions

The allocation-free claim starts at the provider failure handoff. It is not a
claim that every legacy helper returns at its first failed inner allocation:

- **E1 — composite request and persistence helpers.** Instruction/skill/MCP
  collection, session provider views, tool schema, `session_add_*`,
  `session_save` and tool preparation/execution retain their existing internal
  allocation sequences; recursive yyjson copy/serialization may attempt another
  allocation before returning to its guarded caller. Their instrumented indices
  remain in the whole-turn sweeps; allocations inside the reserved settlement
  boundary remain forbidden.
- **E2 — vendored WebSocket allocator coverage.** The pinned wslay sources use
  their own malloc calls and are not compiled with the tny allocation override;
  their internal failures are not indices in the tny sweep.
- **E3 — external allocators.** Embedding callbacks, libc and platform TLS are
  outside the tny allocator counters.

## Alternatives and consequences

Reserving arbitrary JSON/RPC/TLS scratch space would couple settlement to
unbounded provider payloads and library allocators. Retrying ordinary cancel
would still depend on memory becoming available. Resource-only cancellation
uses existing ownership and reconnect boundaries, with no new worker or ABI.
User callbacks retain their affinity and non-reentrancy rules. wasm follows the
same native cleanup and ACP transport-close policy; Cursor remains unavailable.

## Verification

The real fault-library Responses fixture covers partial text, permission waits,
retained asynchronous tools and a later response after persisted usage: two
injected OOM turns, zero settlement and delivery allocation attempts, exactly
one OOM ERROR and error TURN_END per turn, late completion rejection, then
success on the same handle. The provider-fault host links the real ACP, Cursor
and OpenAI backends against the fully instrumented object graph and sweeps
every discovered allocation index of complete active mock turns. Behavioral
mutants (allocating settlement, ordinary finalization after parser OOM,
persisting usage after request OOM, allocating after decoder/SDK-error/ACP
message OOM, blocking before KILL escalation) must each fail their oracle.
Series evidence records host gate results and the boundary between fixtures
and live service proof.
