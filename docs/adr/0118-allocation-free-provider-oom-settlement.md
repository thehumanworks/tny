# ADR 0118: Allocation-free provider OOM settlement

Date: 2026-09-16
Status: accepted

## Context

ADR 0116 reserves the runtime ERROR/TURN_END pair, but ordinary provider cancel
still constructs tool results, persists transcript JSON, or builds protocol RPCs.
A reserved queue alone therefore cannot settle an active turn without allocating.

## Decision

The runtime enters a private thread-local emergency settlement scope at the
quiescent provider boundary. It publishes its terminal guard before invoking the
existing cancel callback. Providers inspect the scope without new vtable fields,
public exports, record layouts or event kinds. The scope includes resource
release and reserved event enqueue. Test builds count every allocation attempt
inside it; public delivery is checked separately for zero allocation attempts.

The native loop invalidates pending custom-tool leases, releases permissions,
parser/response storage and pending images, removes an unsent preview in place,
then returns to idle. It does not manufacture tool results, invoke extension
control hooks, or save JSON under OOM. Already committed transcript messages
remain owned. The existing session_provider_view repair supplies missing tool
results when a later request can allocate. Partial response text is not newly
persisted on this emergency path. Ordinary cancellation is unchanged.

Cursor closes streams and reverse callbacks and stops its owned bridge process
without constructing CancelRun/Shutdown RPCs. It retains the agent identity and
ephemeral store; a later send reconnects and resumes before sending. ACP closes
its transport/owned process and clears pending permissions/readers; a later send
reconnects and uses the existing session/load or session/new policy. WebSocket
emergency close frees the framing context without allocating a close frame; TLS
close skips close-notify construction while retaining normal-close SIGPIPE guards.
Closing an attached remote transport is not proof of remote turn termination.
Codex uses the native Responses implementation and shares its emergency path.

## Alternatives and consequences

Reserving arbitrary JSON/RPC/TLS scratch space would couple settlement to
unbounded provider payloads and library allocators. Retrying ordinary cancel
would still depend on memory becoming available. Resource-only cancellation
uses existing ownership and reconnect boundaries, with no new worker or ABI.

User callbacks retain their existing affinity and non-reentrancy rules. The
allocation guarantee covers tny-owned allocations, not allocations an embedding
application or platform TLS implementation makes internally. wasm follows the
same native cleanup and ACP transport-close policy; Cursor remains unavailable.

## Verification

The real fault-library Responses fixture covers partial text, permission waits,
and retained asynchronous tools: two injected OOM turns, zero settlement and
delivery allocation attempts, exactly one OOM ERROR and error TURN_END per turn,
late completion rejection, then success on the same handle. The provider
allocation mutant must fail the counter assertion. Phase-2 evidence records
host gate results and the boundary between fixtures and live service proof.
