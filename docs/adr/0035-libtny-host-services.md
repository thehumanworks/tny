# 0035 — libtny host services use a copied, non-reentrant v1 table

Date: 2026-08-27
Status: accepted

## Context

ABI 0.5 made public runtimes independent, but an embedding host still had no
explicit place to supply clocks, entropy, diagnostics, storage, URL handling,
or scheduler wakeups. Reaching process globals from language bindings would
undo runtime ownership, while appending fields to `tny_runtime_options_v0`
would overwrite storage allocated by older callers.

## Decision

ABI 0.6 adds `tny_runtime_options_v1`, `tny_host_services_v1`, their dedicated
initializers, and `tny_runtime_create_v1`. The v1 options contain the frozen v0
options by value and a borrowed table pointer. Creation validates the v1 ABI
and declared sizes, copies the known host-services prefix, and never retains
the table address. `user_data` is copied as an opaque value; the host owns the
object it identifies and keeps it alive until `tny_runtime_free` returns.
Existing `tny_runtime_create` callers and every v0 byte remain unchanged.

The table begins with `abi_version`, `struct_size`, and `user_data`. Its
callbacks are diagnostics, monotonic milliseconds, secure random bytes,
opaque revisioned storage load/store, URL opening, and scheduler notification.
All are synchronous on the runtime owner thread. Callback inputs are borrowed
only until return; callbacks must copy anything retained. The storage API is a
caller-buffer interface, so private session layouts never cross the boundary.
A successful store must advance its opaque revision.

Callbacks are non-reentrant. While one is active, every owner-thread public
operation on that runtime fails with `TNY_STATUS_BAD_STATE`; cross-thread
cancellation retains the sole exception established by ADR 0033. C++ callback
types are `noexcept`. Other language trampolines catch exceptions internally
and translate them to a stable `TNY_STATUS_*` value. Positive, unknown, or
otherwise invalid callback results become `TNY_STATUS_INTERNAL`; no foreign
unwind may cross libtny.

Monotonic time and secure randomness have native defaults. Host time drives
public event timestamps and runtime deadlines; a regressing or failed clock
falls back to the native monotonic source for internal progress, while an
explicit clock request returns the typed failure. Native entropy reads the OS
CSPRNG and fails closed rather than substituting predictable bytes.

Diagnostics are optional, redacted lifecycle/service messages. They receive
no provider body, URL, key, credential, prompt, or caller-supplied failure
text, and their result never controls correctness. Scheduler notification is
also advisory because pull delivery remains authoritative; event enqueue calls
it when present. Explicit scheduler, storage, and URL requests return
`TNY_STATUS_UNSUPPORTED` when absent. Operational callback failures return a
typed error with a static redacted message; failed random output is zeroed.

Runtime destruction sends its final lifecycle diagnostic before invalidating
the copied table. It then releases children and context, clears the context
reference, zeroes the table, and frees it. No worker or deferred callback is
created, so no callback can occur after `tny_runtime_free` returns.

## Consequences

Language SDKs can build a trampoline table without exposing C internals or
process globals. Hosts pay one small per-runtime copy only when using v1.
Host-managed storage is available as an opaque revisioned service, but this
decision does not replace libtny's existing session persistence automatically;
that migration requires a separate schema-neutral policy decision. Network
transport remains out of this interface.

The public libtny artifact is native-only. The wasm CLI does not advertise or
instantiate this embedding table, URL integration, or host-managed storage;
there is no claimed wasm host-services parity.

## Verification

`tests/integration/test_libtny_host_services.py` builds strict C and C++
consumers, runs a real strict-mock turn through host time and scheduler
callbacks, verifies table copying, revisions, buffer lifetimes, redacted typed
failures, missing-service defaults, reentrancy rejection, owner-thread checks,
and the final teardown callback. The same fixture installs Python `ctypes`
trampolines, proves a reentrant call is rejected, and checks that a failed
random callback is typed and its partial output is erased. Export snapshots
contain every ABI 0.6 symbol, while the fixture asserts the original 200-byte
v0 options size.
