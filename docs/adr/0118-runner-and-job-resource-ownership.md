# ADR 0118: Runner and durable job resource ownership

Date: 2026-09-16
Status: accepted
Related: issue #139 (P3-I1 through P3-I6); ADR 0114, ADR 0116, ADR 0117;
behavioral contracts ADR 0053, 0081, 0093, 0099, 0104, 0107, 0108

## Decision

Extend the private C++20 ownership area of ADR 0114 to the runner and
durable-job resource aggregates in `src/core/runner.cpp` and
`src/core/jobs.cpp`. Their private entry points retain C linkage, and the
scheduler, checkpoint protocol, job/session schema, permissions, CLI/TUI and
public ABI retain their existing contracts. OS-specific spawn, mapped-fd
staging, process-tree identity, Windows Job operations and pre-exec paths stay
in the existing C host seams (`src/util/process.c`, `src/util/process_scope.c`,
`src/util/jobs_host.c`, `src/core/session.c`). wasm retains its unsupported
process operations, and the macOS TLS fork-safety fallback is unchanged.

`src/util/resources.hpp` provides move-only close-only descriptor and lock
owners, a pipe pair, a spawn-writer guard and a process-scope owner. Adopt takes
sole ownership, borrow provides a synchronous integer view, release transfers
responsibility. Destructors preserve errno, never retry close, allocate, throw,
wait, signal, write status or explicitly unlock an open-file description.
Closing a parent copy must not unlock the inherited child description.
Session-spawn ownership refers to the existing session lock slot; it never
creates a second owning fd. A lock already owned by the caller remains borrowed
by spawn.

Runner/client and job transaction/slot aggregates are properly constructed C++
objects. They cannot be calloc'd or memset. Heap-owned objects use
`tny::make_owned` from `src/util/ownership.hpp`, so their allocations enter the
existing fault sweeps, and `bad_alloc` is contained at private C boundaries.
Existing C byte/JSON helpers retain their failure contracts. Credentials retain
their wiped storage and anonymous IPC handoff; they do not enter persistent
owners.

Lifecycle completion is explicit: cancel, poll/reap, drain, persist final
state/snapshot, remove socket, release authority, send bye. RAII closes storage
resources; it does not establish quiescence or success. Process scopes require
explicit fallible retirement. Failed retirement retains authority and unknown
cleanup state; it never clears a reservation or manufactures success. A durable
job latches its cleanup hold as soon as any item's cleanup becomes unknown, and
the hold and the checkpoint resumable flag are updated in place without
allocating.

Restart maps borrowed listener/writer/client descriptions into the new process
while the old process retains its copies. Checkpoint consumption and RUN remain
explicit protocol transitions. Failed exec/handshake rolls back only pre-RUN
work; successful RUN exits without session mutation or explicit unlock.

## Verification and limits

`docs/verification/cpp-phase-3/ownership.md` records the inventory and the
behavioral oracle mapping; the series evidence records the actual conversion
checks, real descriptor/lock/process fixtures (`tests/fixtures/runner_ownership.cpp`
with `tests/fixtures/resource_host_faults.c`), mutations
(`tests/mutation/runner_critical.py`) and unavailable platform gates. This ADR
makes no performance or cross-platform runtime claim.
