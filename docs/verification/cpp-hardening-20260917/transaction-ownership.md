# Durable-job transaction ownership

Scope: the existing private C++ jobs module, its record-loading/serialization
helpers and `jobs_txn`. This continues ADR0118 under ADR0121's maintainability
priority; the C ABI, platform seams, locks, schema, retry and cleanup policies
are unchanged. It does not broaden the migration to unrelated C modules.

The record loader now owns its filename, input bytes and parsed yyjson document
with existing `tny::c_string` and `tny::document` aliases. Early malformed,
missing, oversized or allocation-failed returns clean up automatically. The
parsed document owns its own bytes, so the input buffer is explicitly released
after parsing. A successful mutable document is transferred through the
existing raw-return private C seam, not exposed as a new public C++ ABI.

`jobs_txn` directly owns the directory and mutable document. Member order is
lock, directory, document: C++ destroys the document and directory before the
lock, including unwinding past the owning transaction. The destructor is defaulted and
nonthrowing; copy and move are deleted. Explicit `reset()` follows the same
order and is idempotent. C helpers receive `.get()` borrows. Only the existing
explicit commit writes state; destruction never commits, retries, signals,
waits or infers cleanup success.

The source-bound runner fixture now checks type-level ownership, repeated
uncommitted normal exits, fixture exceptions unwinding past the transaction,
lock contention during actual writes,
allocation-free reset/reuse, every directory/JSON allocation observed during admission,
including the directory copy through `tny_alloc_strdup`, and real
open/write/fsync/rename failures. Failed commits preserve original
record bytes and release the state lock; successful explicit commit advances
the persisted revision. Existing process/descriptor/checkpoint/cleanup-hold
oracles and behavioral mutation checks continue to run on the same fixture.

These changes remove paired manual free paths without introducing a new
allocator, container or dependency. The standard unique_ptr wrappers adopt
existing allocations; they add no allocation or scheduling operation.
Measured artifacts, commands and results are recorded separately in evidence.md.

The fault sweep covers the transaction directory copy and parsed/copied JSON
allocations. It does not claim to exhaust every allocation in the unchanged C
file-slurp/path/timestamp helpers or to alter their historical error categories.
The fixture's C++ exception is raised after successful admission and unwinds
past the transaction object; production jobs helpers still return C-style errors.

The combined finalization review tightened directory-allocation failure: it now
reports ENOMEM with an explicit out-of-memory message, resets all owners, and
returns before reading/parsing the record. The first fault index asserts exactly
one allocation attempt. The integrated mutation inventory also deliberately
omits document reset and lock reset; both must fail the transaction-state oracle.
