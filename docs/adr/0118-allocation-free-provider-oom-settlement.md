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

## Review 4 clarification and bounded exceptions

Amended 2026-09-16 under explicit repair authorization. The allocation-free
claim starts at the provider failure handoff / reserved settlement boundary.
It is not a claim that every legacy helper returns at its first failed inner
allocation. The following exceptions delimit that stronger claim:

- **E1 — composite request and persistence helpers.** Instruction/skill/MCP
  collection, `session_provider_view`, `tools_schema_json`, `session_add_*`,
  `session_save`, and tool preparation/execution retain their existing internal
  allocation sequences. Recursive yyjson copy/serialization and mutable JSON
  construction can also attempt another allocation before returning to their
  guarded caller. Provider callers stop before constructing another owner,
  invoking another tool/control callback, or entering another persistence
  operation once failure is observed. Rewriting those shared helper internals
  is outside this provider repair. Their instrumented indices in the fixed
  mock workloads remain included in the whole-turn sweeps; allocations inside
  the reserved settlement boundary remain forbidden.
- **E2 — vendored WebSocket allocator coverage.** The pinned wslay sources use
  their own malloc calls and are not compiled with the tny allocation override.
  Their internal allocation failures are not indices in the tny sweep. The
  tny WebSocket callback now disables both receive and send immediately when
  its reader fails, so wslay cannot consume another coalesced frame after that
  callback failure. This closes the continuation path without modifying the
  pinned library. It does not establish exhaustive fault injection of wslay's
  own chunk/queue/flatten allocations.
- **E3 — external allocators.** Allocations inside embedding callbacks, libc,
  and platform TLS are outside the tny allocator counters. This is the existing
  external-allocation boundary, not proof of those implementations' OOM paths.

The runtime keeps its pending terminal private until fallible finalization
returns. A finalization allocation failure therefore substitutes the reserved
OOM ERROR/TURN_END pair before any successful terminal is delivered. Cursor
pending completion unlinks the consumed provider lease before result
serialization; its pump thread transfers failure across `pthread_join` before
owner-thread processing resumes. These ownership and lifetime rules are not
exceptions. Store operations stop at the failed operation; already completed
writes/deletions are not rolled back.

The Review 4 evidence records discovered allocation counts, sanitizer coverage,
per-inventory dispositions and the exact tested source. Those finite mock
workloads do not establish every possible provider payload or external service
behavior, nor do host sanitizers establish unavailable platform/leak gates.
