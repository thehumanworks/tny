# ADR 0125: Durable job transaction value owners

Date: 2026-09-17. Status: accepted for the ownership continuation.

## Decision

Use the existing `tny::c_string` and `tny::mutable_document` owners for a
job transaction's directory and document. The lock is declared first, so
reverse member destruction releases document, directory and finally lock.
Reset follows that same order and is idempotent. Transactions are neither
copyable nor movable; committing remains an explicit fallible operation.

The record loader/store use scoped string/document owners rather than paired
manual frees. Parsed immutable documents copy their input; a mutable deep
copy can outlive the loader's local owners. Directory copying uses the
existing injected allocator, making its admission failure observable in the
same sweep as JSON failures. No new allocation family or public ABI is added.

Cancellation, revisions, atomic persistence, cleanup holds, reservation
ownership, reaping and C OS seams retain their existing protocols. Destruction
does not persist edits or report successful cleanup. Exceptions in a caller's
scope unwind the transaction owner; jobs code does not acquire new throwing
operations from these null-returning C allocation functions.

## Verification

A real-descriptor fixture checks 32 abandonment/unwinding cycles, idempotent
reset/reuse, all discovered directory/JSON admission allocation failures,
open/write/fsync/rename commit failures, unchanged disk bytes and an explicit
successful commit. A fresh open-file-description probe must find the lock busy
inside the actual write operation. Compile-time traits prohibit copying/moving.
Two reset mutants must be killed. Linux memory checking covers the raw yyjson
and directory allocations not represented in C++ allocation counters.

The transaction implementation originated in a concurrently active isolated
worktree and was preserved and integrated with its tests after reconciliation.
Final combined evidence and independent-review dispositions are recorded under
`docs/verification/cpp-finalization/`. The 6 MB guardrail and original
performance gates remain unchanged.
