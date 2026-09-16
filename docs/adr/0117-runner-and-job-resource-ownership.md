# ADR 0117: Runner and durable job resource ownership

Date: 2026-09-16. Status: implemented; full contract verification incomplete.
Scope: issue #139, P3-I1 through P3-I6.

Extend the private C++20 ownership area to runner and durable-job resource
aggregates. Their private entry points retain C linkage, and the scheduler,
checkpoint protocol, job/session schema, permissions, CLI/TUI and public ABI
retain their existing contracts. OS-specific spawn, mapped-fd staging,
process-tree identity, Windows Job operations and pre-exec paths stay in the
existing C host seams. wasm retains its unsupported process operations, and
the macOS TLS fork-safety fallback remains unchanged.

Use move-only close-only descriptor and lock owners. Adopt takes sole ownership,
borrow provides a synchronous integer view, release transfers responsibility.
Destructors preserve errno, never retry close, allocate, throw, wait, signal,
write status or explicitly unlock an open-file description. Closing a parent
copy must not unlock the inherited child description. Session-spawn ownership
refers to the existing session lock slot; it never creates a second owning fd.
A lock already owned by the caller remains borrowed by spawn.

Runner/client and job transaction/slot aggregates are properly constructed
C++ objects. They cannot be calloc'd or memset. The tny allocator constructs
heap-owned objects and bad_alloc is contained at private C boundaries. Existing
C byte/JSON helpers retain their failure contracts. Credentials retain their
wiped storage and anonymous IPC handoff; they do not enter persistent owners.

Lifecycle completion is explicit: cancel, poll/reap, drain, persist final
state/snapshot, remove socket, release authority, send bye. RAII closes storage
resources; it does not establish quiescence or success. Process scopes require
explicit fallible retirement. Failed retirement must retain authority and
unknown-cleanup state; it must never clear a reservation or manufacture success.

Restart maps borrowed listener/writer/client descriptions into the new process
while the old process retains its copies. Checkpoint consumption and RUN remain
explicit protocol transitions. Failed exec/handshake rolls back only pre-RUN
work; successful RUN exits without session mutation or explicit unlock.

[Inventory](../verification/cpp-phase-3/ownership.md) and
[evidence](../verification/cpp-phase-3/evidence.md) record the actual conversion,
behavioral tests, mutations and unavailable gates. This ADR makes no performance
or cross-platform runtime claim. Independent reviews belong to the coordinator.
