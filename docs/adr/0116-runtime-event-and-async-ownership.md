# ADR 0116: Private runtime event and async tool ownership

Date: 2026-09-16
Status: accepted
Related: issue #138; ADR 0114, ADR 0115

## Decision

Extend ADR 0114's private C++20 area to owned runtime event records and to
custom tool registration/call ownership. Scheduling, session state, queue
policy, provider loops, host services, public libtny exports and the SDKs stay
with their existing C owners. The public C ABI, record layouts, capability
flags and release APIs do not change. Native and wasm share the event
implementation; wasm still exposes no custom-tool embedding API or
worker-completion capability.

An event has a stable heap address and a single allocator-backed byte vector.
A local field descriptor list copies every retained payload once; no parallel
free table exists. The vector is sized before views are built and never grows
afterwards. Unique handle transfers through reserve, pending terminal, queue
and pop never move the record. Published payload pointers are immutable and
valid until event release, independently of engine/session/runtime lifetime.
Explicit text lengths preserve embedded NUL and distinguish present-empty from
absent fields.

Logical event-count and payload-byte budgets are unchanged. Actual record,
container and allocator overhead is measured separately. OOM reserve pairs are
built transactionally before a turn. Only unpublished reserves have a mutable,
preallocated turn-id slot. Settlement marks the engine terminal before backend
cancellation so cancellation callbacks cannot allocate another event. The
provider's own emergency cleanup is governed by ADR 0117.

Registrations and public/private handle wrappers have unique ownership.
Allocator-backed shared control blocks are limited to the registry state and
the call state actually shared with an async worker. A provider owns a private
pending handle; the callback host owns a distinct public handle and retains its
existing exactly-once release obligation. Either handle may be released first.
Registry invalidation changes state, not the provider handle's lifetime; C
adapters consume their pending handle on take or invalidate and clear it, and
generic tool-call cleanup invalidates a still-pending call. This removes the
earlier risk of registry invalidation freeing a provider borrower before
backend teardown.

The registry's intrusive active-call list borrows call states from pending
owners and never owns them. Call states retain registry state, and a weak
registry backlink supports invocation without a reference cycle. Registration
addresses remain stable across vector growth. Closing marks all calls inactive
and closes wake descriptors under the same lock; retained workers keep only the
storage needed for safe stale-completion rejection. A single RAII pthread mutex
protects active/completed/epoch/closing/result state, replacing ADR 0038's
two-lock implementation while preserving its linearization and rejection
semantics. Generation and epoch checks remain explicit. Concurrent session
destruction remains unsupported.

All introduced object, string, vector and shared-control-block allocations use
`tny::allocator`/`make_owned` from `src/util/ownership.hpp` and the existing
allocation scope, so allocation-index fault sweeps reach them. Partial objects
are destroyed automatically; allocation exceptions become NULL/OOM at private C
entry points. Destructors and lock cleanup neither throw nor allocate.
Callbacks remain noexcept and non-reentrant under the existing public contract.
No global allocation override or new library is added.

## Verification and limits

`docs/verification/cpp-phase-2/ownership.md` records owners, borrowers, thread
rules and transfers. `docs/verification/cpp-phase-2/evidence.md` and the series
evidence record the real runtime/library/SDK tests, exhaustive allocation
sweeps including C++ owner and control-block allocations, behavioral mutations
(borrowed view, missing generation check, early async release, missing byte
accounting, duplicate terminal, allocating settlement), allocation measurements
and any unmet platform/performance gates. No throughput, startup or
cross-platform improvement follows from this ADR.
