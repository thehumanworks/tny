# 0033 — libtny runtimes are independent and cancellation has one cross-thread wake path

Date: 2026-08-27
Status: accepted

## Context

ADR 0023 deliberately shipped ABI 0 with one public runtime per process and
made every handle operation owner-thread-affine. That let the first embedding
surface reuse the private runtime safely, but it prevented language schedulers
from hosting independent agents or interrupting a session whose owner was
blocked in `tny_session_next_event`.

Calling a backend's cancel vtable directly from another thread is not safe.
Backend transports, parsers, event queues and session persistence are all
single-driver state. Merely adding an atomic flag is also insufficient because
the owner may be sleeping in the process-wide `tny_poll` seam until a long
deadline.

## Decision

ABI 0.5 removes the public-runtime singleton. Every explicit libtny context,
permission engine, backend, session, event queue, credential copy and state
root is owned by its runtime. Each runtime continues to allow one open session
and one active turn. MCP remains disabled for public runtimes, so they cannot
reach the current process-global MCP server table; multi-runtime MCP is not
advertised by this decision.

The runtime creator is the owner thread. All runtime/session operations remain
owner-thread-affine except `tny_session_cancel`. A wrong-thread status-returning
operation returns `TNY_STATUS_BAD_STATE`; void teardown from the wrong thread is
ignored. Callers must not race teardown with any operation.

`tny_session_cancel` only publishes an atomic, idempotent request and signals
an engine-owned nonblocking close-on-exec self-pipe. The read end is appended
to the backend descriptors passed to `tny_poll`. When it wakes, the owner
drains the pipe, consumes the request, calls the backend cancel operation, and
continues the ordinary event drain. Repeated requests coalesce. Cancellation
on an inactive turn is a no-op and cannot poison the next turn. Provider and
runtime duplicate-terminal suppression still guarantees exactly one terminal,
with `TNY_STOP_REASON_INTERRUPTED` for cancellation.

The caller keeps the session alive until all cross-thread cancel calls return,
then joins scheduler work before freeing children and parents on the owner.
No other concurrent mutation is supported.

Runtime handles also record the creating process id. After `fork()`, inherited
status-returning operations fail with `TNY_STATUS_BAD_STATE`, and inherited
void teardown is ignored. The child does not continue or free inherited
handles; a multi-threaded child follows POSIX async-signal-safety until `exec()`
and creates new runtimes afterward. Parent state is untouched.

Capability discovery advertises `TNY_CAP_FEATURE_CROSS_THREAD_CANCEL` in both
available and enabled masks and reports
`TNY_CANCEL_CROSS_THREAD_ASYNC_WAKE`. The threading model remains
`TNY_THREADING_OWNER_THREAD`: cancellation is the sole exception. ABI 0.5 adds
no exported symbol and changes no existing struct layout.

All ABI-0 `*_v0` layouts are now explicitly frozen, including reserved tails.
Their initializers necessarily write `sizeof(v0)`, so appending to v0 would
overflow storage allocated by an older SDK. Any future layout growth uses a
new v1 type and new initializer plus read/query/create symbols. Sized prefix
reads remain supported where already documented, but are not permission to
grow a v0 initializer target.

Endpoint reachability remains last-known state. Starting a new turn does not
reset it to unknown; only ordinary traffic updates reachable or unreachable.

## Consequences

Language SDKs may host multiple independent runtimes and map scheduler
cancellation to one safe ABI call without polling short deadlines. Backends
and event queues remain simpler single-driver code. The wake pipe costs two
descriptors per open libtny session and is closed with its engine.

The immutable platform TLS loader remains process-wide, as do ordinary C
runtime facilities. Credentials, provider selections, events, persistence and
child lifecycles do not use shared public-runtime state. MCP must become
runtime-owned before a later ABI can enable it for multiple runtimes.

The wake implementation remains inside the existing `tny_poll` platform seam:
native `tny_poll.c` owns the self-pipe operations, while `net_wasm.c` exposes a
clean unavailable stub because public libtny is not a wasm artifact. Browser
CLI/runtime builds therefore do not compile or emulate POSIX pipe behavior.

## Verification

The libtny integration suite creates simultaneous runtimes with distinct
workspaces, state directories, credentials and mock endpoints; repeats
out-of-order runtime/session teardown; cancels a five-second blocked pull from
a scheduler thread; repeats that cancellation; measures a sub-second wake; and
asserts exactly one interrupted terminal. Existing exact-export, C/C++ header,
permission, error-order, CLI/TUI/ACP, size and sanitizer gates remain required.

The race-specific gate is `make test-libtny-tsan` on Linux x86_64 with GCC.
It builds every libtny object and a native C embedding host with
`-fsanitize=thread`; Python starts only the strict loopback providers before
executing that host and never loads the instrumented library into an existing
Python process. Four owner threads create independent runtimes with distinct
workspaces, state roots, credentials and endpoints, begin simultaneous turns,
and block in event delivery. Four scheduler threads then issue repeated
idempotent cancels against every session while also proving a non-cancel
wrong-thread operation returns `TNY_STATUS_BAD_STATE`. Each owner observes one
and only one interrupted terminal, drains, waits for every scheduler call to
return, then completes a fresh turn on the same isolated session before owner-
ordered teardown. The CI stress repeats the lifecycle twenty times and treats
any TSan diagnostic as a hard failure.

This TSan evidence is intentionally scoped to the supported Ubuntu glibc
x86_64 GCC toolchain. It does not claim TSan coverage for macOS, Linux arm64,
musl, Windows, or wasm. macOS continues to run its ASan/UBSan and functional
multi-runtime coverage; those sanitizers are valuable memory/undefined-behavior
checks but are not a substitute for a race detector.
