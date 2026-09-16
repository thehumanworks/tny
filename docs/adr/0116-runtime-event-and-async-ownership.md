# ADR 0116: Private runtime event and async tool ownership

Date: 2026-09-16. Status: implemented, verification pending.
Scope: issue #138, P2-I1 through P2-I6.

## Decision

Extend ADR 0114's private C++20 area to owned runtime event records and custom
tool registration/call ownership. Scheduling, session state, queue policy,
provider loops, host services, public libtny exports and SDKs stay with their
existing owners. The public C ABI, layouts, capabilities and release APIs do
not change. Native/wasm share the event implementation; wasm still exposes no
custom-tool embedding API or worker-completion capability.

An event has a stable heap address and a single allocator-backed byte vector.
A local field descriptor list copies every retained payload once; no parallel
free table exists. The vector is sized before views are built and never grows
thereafter. Unique handle transfers through reserve/pending/queue/pop do not
move records. Published payload pointers are immutable and valid until event
release, independently of engine/session/runtime lifetime. Explicit text
lengths preserve embedded NUL and present-empty versus absent fields.

Logical event-count and payload-byte budgets remain unchanged. Actual record,
container and allocator overhead is measured separately. OOM reserve pairs
are built transactionally before a turn. Only unpublished reserves have a
mutable, preallocated turn-ID slot. Settlement marks terminal before backend
cancellation so cancellation callbacks cannot allocate an additional event.
This does not make the provider's ordinary cancel operation allocation-free:
protocol requests and transcript updates there remain an unresolved P2-I3
obligation, explicitly recorded in the evidence.

Registrations and public/private handle wrappers have unique ownership.
Allocator-backed shared control blocks are limited to the registry state and
call state actually shared with an async worker. A provider owns a private
pending handle; the callback host owns a distinct public handle and retains
its existing exactly-once release obligation. Either handle may be released
first. Registry invalidation changes state, not the provider handle's lifetime;
C adapters consume their pending handle on take or invalidate and clear it.
This removes the old risk of registry invalidation freeing a provider borrower
before backend teardown.

The registry's intrusive active-call list borrows call states from pending
owners and never owns them. Call states retain registry state, and a weak
registry backlink supports invocation without a reference cycle. Unique
registration addresses remain stable across vector growth. Closing marks all
calls inactive and closes wake descriptors under the same lock; retained
workers keep only the storage needed for safe stale-completion rejection.
A single RAII pthread mutex now protects active/completed/epoch/closing/result
state, replacing ADR 0038's two-lock implementation while preserving its
linearization and rejection semantics. Generation and epoch checks remain
explicit. Concurrent session destruction remains unsupported.

All introduced object, string, vector and shared-control-block allocations use
`tny::allocator`/`make_owner` and the existing allocation scope. Partial objects
are destroyed automatically; allocation exceptions become NULL/OOM at private
C entry points. Destructors and lock cleanup neither throw nor allocate.
Callbacks remain noexcept/non-reentrant under the existing public contract.
No global allocation override or new library is added.

## Verification and limits

[Ownership inventory](../verification/cpp-phase-2/ownership.md) records owners,
borrowers, thread rules and transfers. [Evidence](../verification/cpp-phase-2/evidence.md)
records real runtime/library/SDK tests, exhaustive allocation sweeps, six
behavioral mutations, allocation measurements and unmet platform/performance
gates. Self-review does not substitute for coordinator independent review.
No throughput, startup or cross-platform improvement follows from this ADR.
