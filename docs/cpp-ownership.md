# Extending the private C++ ownership layer

The goal is fewer manual cleanup paths and clearer lifetime contracts, not
turning every C file into C++. The public ABI remains C; scheduling and OS
operations remain in their existing owners. See ADRs 0121 and 0123 for the
current priorities and reconciled implementation. Binary size is a guardrail
below decimal 6 MB, not a reason to weaken error handling or obscure code.

## Choose the right owner

| Data or authority | Representation | Lifetime rule |
| --- | --- | --- |
| Private heap object | `tny::owned<T>`, created with `tny::make_owned<T>` | One owner; construction failure releases raw storage automatically |
| Growing text/bytes | `tny::string`, `tny::bytes`, `tny::vector<T>` | Uses the fault-injected allocator; views expire on mutation/move |
| yyjson document | `tny::document` / `tny::mutable_document` | Own the document, not its individual borrowed values |
| Retained backend event | `tny_owned_event` handle backed by `owned_event.cpp` | Stable immutable payload addresses until handle release |
| Async host and provider state | Distinct handles sharing the private `call_state` | Invalidation changes permission to complete, not retained memory lifetime |
| Descriptor or advisory lock | `tny::descriptor` / `tny::lock_descriptor` | Move-only; `borrow()` is non-owning; `release()` transfers authority |
| Native process scope | `tny::process_scope` | Explicit cancellation/reaping/retirement proves cleanup; destruction cannot fabricate success |

Keep one ownership family: `util/ownership.hpp`, `json/ownership.hpp` and
`util/resources.hpp`. Do not reintroduce the former `src/cpp` implementation,
replace global `operator new`, mix allocator macros into STL headers, or add a
second reference-counting protocol beside `std::shared_ptr`.

## Add a parser or owned field

Borrow input bytes only during the synchronous parse/callback. Any data that
survives that call must be copied or transferred into an owner first. A pointer
into a yyjson document is valid only while that document remains alive.
Rebuild views after moving a string, especially a small-string value; moving
the string does not promise that its old address remains usable.

The parser facades translate allocation and length failures to the existing
OOM status before returning to C. Malformed input is distinct from OOM.
Preserve framing limits, checked arithmetic and provider-specific malformed
input behavior. Callback cancellation must not destroy a parser/document
while a feed/flush callback still uses it: the OpenAI adapter defers terminal
cleanup until the active parsing boundary returns.

For a retained event field, update the private event declaration and the
single field descriptor list in `owned_event.cpp`. Do not add a parallel free
table: the immutable byte vector owns every copied payload. Extend the runtime
fixture with absent versus present-empty data, embedded NUL for length-bearing
text, source mutation, queue transfer, engine teardown and payload-byte-budget
checks. OOM reserve events are constructed before the active turn; emergency
settlement must use those reserves without formatting or persisting new data.

## Extend an asynchronous tool

The host callback's public release handle and the provider's pending handle
are separate owning leases. Either side may finish first. Registry invalidation,
unregister, cancellation and teardown must reject late or duplicate completion
without freeing memory still retained by the other side.

Keep generation, epoch, active/closing and completion checks under the same
registry lock. Use the allocator-backed shared control blocks only for state
that genuinely crosses those lifetimes. Do not store an unretained pointer to
a call in a worker or use registry invalidation as implicit object destruction.

Exercise the real worker/callback paths, not just smart-pointer helper tests:
complete before/after cancel, unregister while pending, early host release,
late completion after runtime teardown, wrong generation, duplicate completion,
completion allocation failure, and a subsequent successful turn.

## Transfer a process resource

Adopt a newly acquired resource immediately so every ordinary error return
has automatic cleanup. A borrow never closes or unlocks the caller's resource.
A successful transfer must clear the previous owner, not leave two wrappers
holding the same descriptor or process scope. Locks close their descriptor;
an explicit `LOCK_UN` could revoke a fork-inherited open-file description.

Destructors are nonthrowing, bounded cleanup. Explicit operations still own
`cancel -> reap/drain -> persist -> remove socket -> release writer`. Persist
an unknown cleanup state and retain reservations when cleanup cannot be proven.
A stored PID is metadata, not authority to signal another process. SIGKILL
bypasses destructors; crash recovery is a separate tested protocol.

Keep pre-exec and signal-handler operations in the existing C seams. Preserve
secret-buffer wiping and anonymous handoff channels; never replace them with
an ordinary string free or write credentials to logs, checkpoints or argv.

## Verify the change

Use the normal compiler pair, C for C11 and C++ for private C++20. The Makefile
owns source discovery, matching link drivers, sanitizer/fault object sets,
format/analyzer scope and platform capabilities. Add focused tests to the
existing inventory; the build contract must detect missing new sources.

```sh
make quality test-cpp-build
make test-unit test-parser-ownership test-parser-backend-ownership test-search-ownership
make test-runtime-ownership test-runner-ownership
make test-libtny-fault test-libtny-fault-sanitize
make test-abi test-sdks
make test-parser-mutation test-runtime-mutation test-runner-mutation
```

Run the relevant full integration, leak, platform and benchmark gates as well.
Generic mutation testing temporarily edits source: run it alone or in an
isolated checkout, verify the pristine baseline first and confirm restoration.
An empty or all-uncompilable selection is not a successful mutation gate.
Expected negative tests may deliberately emit error diagnostics; the test must
assert the intended failure rather than hide unexpected warnings or crashes.

Measure startup, first prompt, parser/event throughput and memory against the
same-host baseline before claiming a performance result. A C++ type, a passing
compiler, or a review alone does not prove a lifetime protocol correct.
