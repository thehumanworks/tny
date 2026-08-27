# libtny embedding API

`libtny` contains the ABI-1 headless C candidate for the same runtime
used by `tny ask`, the TUI, and `tny acp`. It is not a second agent loop and it
does not expose tny's backend, yyjson, session-store, or pollfd layouts.

The lifecycle design is [ADR 0023](adr/0023-libtny-embedding-abi.md), amended
for concurrency by [ADR 0033](adr/0033-libtny-multi-runtime-cancel.md) and
frozen for compatibility by [ADR 0037](adr/0037-libtny-abi-1.md).

## Supported artifacts

The ABI1 candidate builds a shared library on:

- macOS arm64: `libtny.1.dylib`;
- Linux glibc x86_64/aarch64: `libtny.so.1`.

Final ABI0.8 major-0 artifacts install side-by-side from exact commit
`510a95c`. Windows DLL, static, musl-shared and wasm embedding artifacts are
unsupported and advertise neither shared nor static linkage capability.

## Build and install

```sh
make lib-shared
make install-lib PREFIX="$HOME/.local"
pkg-config --cflags --libs libtny
```

Every ABI1 initializer returns a status and takes explicit capacity, for
example `tny_runtime_options_init(&options, sizeof options)`. Runtime create,
capability queries, and event reads likewise take the matching caller
capacity. Prefix capacities must end on a documented field boundary; unknown
tails and nonzero reserved bytes are ignored.

ABI1 uses `include/tny` and `libtny.pc`. ABI0 uses the separate
`include/tny-0` root and `libtny-0.pc`; mixing either header with the other
major's artifact is rejected by the consumer matrix.

An in-tree example is [`examples/embed.c`](../examples/embed.c). It creates a
runtime and session, starts a turn, pulls copied events, answers permissions,
and releases children before parents.

## Lifecycle

```text
runtime -> session -> event
```

- ABI 1 permits multiple independent public runtimes per process. Each
  runtime still permits one open session and one active turn. Workspaces,
  credentials, state roots, backend state, event queues and children belong
  to their runtime; public runtimes continue to disable process-global MCP.
- A runtime and its session have the thread that created the runtime as their
  owner. Every operation except `tny_session_cancel` remains owner-thread
  affine. Status-returning calls made on another thread fail with
  `TNY_STATUS_BAD_STATE`; wrong-thread void teardown calls do nothing.
- `tny_session_cancel` is idempotent and may be called by a scheduler thread.
  It atomically records the request and signals a nonblocking self-pipe in the
  owner thread's `tny_poll` set. A blocked `tny_session_next_event` wakes
  promptly, applies backend cancellation on the owner thread, and must still
  be drained through exactly one interrupted `TNY_EVENT_TURN_END`.
- The owner must keep the session alive until every concurrent cancel call has
  returned. Freeing a runtime/session concurrently with cancel, or making any
  other concurrent call, is unsupported. Join scheduler tasks first, then
  release event, session and runtime handles on the owner thread.
- Pointer-slot `tny_session_destroy(&session)` and
  `tny_runtime_destroy(&runtime)` authoritative teardown. They null the
  caller's slot before releasing storage and return OK when repeated on that
  same null slot. Legacy raw `*_free(handle)` remains one-shot compatibility;
  a stale copied raw pointer is invalid C storage and is not made safe.
- Every successful turn start ends in exactly one `TNY_EVENT_TURN_END`.
- `tny_session_next_event` returns event, timeout, or drained without exposing
  native file descriptors.
- Event string views are owned by the event and remain valid until
  `tny_event_free`.
- The frozen-layout `tny_event_view_v0` snapshot uses
  `tny_event_view_init(&view, sizeof view)`, then
  `tny_event_read(event, &view, sizeof view)`; the view exposes the canonical
  event schema version, sequence, monotonic timestamp, provider/session/turn
  identity, message id, complete usage/context/cost values, and the existing
  event-specific fields in one FFI-friendly read. Existing getters remain
  supported.
- ABI 0.4 adds the frozen-layout `tny_capabilities_v0` snapshot. Its borrowed
  string views remain valid until the runtime is freed.
- ABI 0.5 enables `TNY_CAP_FEATURE_CROSS_THREAD_CANCEL` and reports
  `TNY_CANCEL_CROSS_THREAD_ASYNC_WAKE`. No public symbols or existing struct
  fields changed.
- Asynchronous `ERROR` events expose the same stable status categories through
  `tny_event_error_code` before the terminal event.
- Inputs retained past a call are copied. ABI 0 accepts UTF-8 without embedded
  NUL bytes.

### Forks

Runtime and session handles belong to the process that created them. After
`fork()`, status-returning operations on an inherited handle return
`TNY_STATUS_BAD_STATE`; inherited void teardown calls are ignored. The child
must not attempt to continue or free the inherited runtime. In a
multi-threaded host it should use only async-signal-safe operations until
`exec()`, as required by POSIX, then create a fresh runtime in the new process.
The parent runtime is unaffected.

## Canonical event schema

`sdk/schema/events.json` is the source of truth for the public runtime event
vocabulary. `python3 sdk/schema/check.py` verifies the C numeric constants and
regenerates committed TypeScript/Python type fixtures deterministically. The
compatibility contract is [ADR 0030](adr/0030-public-event-schema.md): new
optional fields are additive, consumers ignore unknown fields, and language
SDKs preserve unknown future event kinds instead of failing the stream.

The public schema currently covers text/thinking, tool lifecycle, permission,
plan, usage, turn-end, error, status, steer rejection, custom/user messages,
and tool progress. Structured tool arguments/results are intentionally a
separate bounded-data extension.

## Configuration and permissions

Call `tny_runtime_options_init`, then provide an existing workspace. An
explicit state directory is required when `persistence == 1`. When
`persistence == 0`, `state_dir` may be empty: the process-local session keeps
its generated id and event metadata in memory and libtny creates no settings,
sessions, history, or other state path. Existing ephemeral callers may still
pass a state directory; it is not materialized. `tny_session_open` remains
unavailable for an ephemeral runtime because there is no durable session to
open.

The public API does not read tny settings or choose a provider from the
environment. ABI 0 embeds the native OpenAI-compatible loop. Cursor, Codex,
and ACP remain CLI providers until their authority and host-state contracts
are explicit.

The default permission policy is `TNY_PERMISSION_ASK`, unlike the CLI's
deliberate yolo default. `max_steps` defaults to 0 (unlimited, matching the
CLI; [ADR 0024](adr/0024-unlimited-steps-default.md)); a positive value through
`INT32_MAX` caps
model calls per turn and ends the turn with `TNY_STOP_REASON_STEP_LIMIT`. A sensitive native or host request emits a permission
event and parks until `tny_session_respond_permission`, cancellation, or close.
MCP is disabled and omitted from the advertised tool schema in ABI 0.
Home/ancestor instructions and home skill catalogs are never imported. Only an
`AGENTS.md`/`CLAUDE.md` in the explicit workspace may enter the prompt.
Process-spawning or ambient-state tools (`subagent`, skills/install, memory,
and interactive clarification) are also omitted from ABI 0.

Credentials are copied, never persisted by libtny, omitted from errors/events,
and wiped from library-owned long-lived storage at teardown. Every allocation
path reachable through the public runtime is contained by the ABI fault scope:
pre-turn exhaustion returns `TNY_STATUS_OOM`, while an active turn settles with
the reserved OOM error and exactly one terminal without terminating the host.

## Structured capabilities

Initialize `tny_capabilities_v0` with `tny_capabilities_init`, then call
`tny_runtime_get_capabilities`. The call is owner-thread-affine and
side-effect-free: it never opens an endpoint, initializes a provider, reads
configuration, or writes state.

The snapshot separates:

- `provider_available_mask`: providers compiled and supported by this public
  library (OpenAI only in ABI 0.5);
- `provider_selected`: the runtime's selected provider;
- `provider_initialized`: whether its local backend has completed
  initialization;
- `endpoint_reachability`: `UNKNOWN` until normal turn traffic observes a
  result, then the last known `REACHABLE` or `UNREACHABLE` state. The query
  itself is not a probe.

`feature_available_mask` reports compiled/library support, while
`feature_enabled_mask` reports selection for this runtime. ABI 0.5 advertises
shared-library packaging, optional session persistence, the platform TLS
implementation, and cross-thread cancellation. It deliberately leaves the
bits for static packaging, MCP, custom in-process tools, terminal embedding,
Windows, wasm, and fully static TLS clear. Cursor, Codex, and ACP provider bits
are also clear. Built-in native tools are not “custom tools.”

ABI 0.6 additionally advertises `TNY_CAP_FEATURE_HOST_SERVICES` as available.
It is enabled only for a runtime created through the v1 entry point with a
copied host-services table.

The snapshot also names the platform, architecture, HTTP transport, TLS
implementation, and linkage, and reports owner-thread/cross-thread-wake cancel
semantics plus the event queue and payload budgets. TLS can be available but
not enabled when this runtime selects an `http://` endpoint.

Capability structs are sized with a frozen v0 layout. Callers must initialize
`struct_size` and ignore unknown mask bits and scalar values. Every ABI-0
`*_v0` layout is frozen, including its reserved tail: its initializer writes
the complete v0 size, so adding fields would overflow binaries built with an
older header. Future growth therefore uses a distinct v1 struct plus new
initializer and query/create symbols. A manually initialized smaller supported
prefix is filled only through its declared size; a prefix smaller than the
documented scalar core is rejected. Borrowed byte views are scoped to the
runtime.

## Host services (ABI 0.6)

Embedders that need host callbacks use `tny_runtime_options_v1_init`, initialize
a `tny_host_services_v1` with `tny_host_services_v1_init`, and call
`tny_runtime_create_v1`. The v1 options embed the complete frozen v0 options;
the old initializer, layout, and `tny_runtime_create` remain binary compatible.
The service table begins with ABI version, declared size, and `user_data`.
libtny copies its known prefix during creation, so the table storage may be
released immediately; `user_data` itself remains host-owned and must outlive
the runtime.

Callbacks are synchronous, owner-thread-only, and non-reentrant. Calling back
into the same runtime from a callback returns `TNY_STATUS_BAD_STATE`.
`tny_session_cancel` remains ADR 0033's only cross-thread operation. Callback
input views and storage buffers are borrowed for the duration of the call only.
C++ callbacks are `noexcept`; Python and other FFI trampolines must catch all
language exceptions and return a stable negative `TNY_STATUS_*` value. libtny
normalizes an invalid callback result to `TNY_STATUS_INTERNAL`, returns typed
errors with static redacted text, and never includes caller URLs, keys,
credentials, prompts, or provider bodies in diagnostics.

`tny_runtime_host_monotonic_ms` and
`tny_runtime_host_secure_random` use safe native defaults when callbacks are
absent. Host time also supplies event timestamps and runtime deadlines; a bad
host clock cannot stall internal progress because libtny falls back to its
native monotonic clock. Native randomness reads the OS CSPRNG and fails closed.
The storage load/store calls carry only an opaque key, byte buffer, and
monotonically advancing revision—never the private session schema. URL-open and
explicit scheduler notification are similarly typed requests. Storage,
URL-open, and notification return `TNY_STATUS_UNSUPPORTED` when absent.

Diagnostics contain redacted libtny lifecycle/service messages and are never
required for correctness. Event enqueue invokes scheduler notification when
present, but pull delivery stays authoritative. The final `runtime destroying`
diagnostic is synchronous; once `tny_runtime_free` returns, the copied table is
zeroed and no stale callback can run. See
[ADR 0036](adr/0036-libtny-host-services.md).

## Custom tools (ABI 0.7)

Initialize `tny_tool_spec_v1`, fill a unique name, description, object JSON
schema, sensitivity, limits, callback and `user_data`, then call
`tny_runtime_register_tool` before creating a session. Registration copies all
descriptor bytes. Built-in/alias/`mcp_` names, duplicates, invalid UTF-8 or
schemas, excessive limits, and post-session registration are rejected. Active
custom schemas are appended to the native OpenAI tool list; built-ins are not
rewritten.
The executable schema subset is object root, single-`type` property schemas,
`required`, and boolean `additionalProperties`; unsupported or nested keywords
are rejected.

The native permission engine classifies sensitivity before invocation, so a
denied tool cannot enter host code. The callback runs on the owner thread under
the same non-reentrant guard as host services. It either returns a synchronous
borrowed UTF-8 result or parks an async call. Async workers complete only through
`tny_tool_call_complete`, passing the exact generation and a bounded
`tny_tool_result_v1`. Completion copies bytes, is thread-safe, wakes a blocked
`next_event`, and rejects wrong-generation, double, cancelled, or unregistered
calls with `TNY_STATUS_BAD_STATE`.

Arguments are valid only during invocation and completion result bytes only
during `tny_tool_call_complete`; recipients copy anything retained. NUL or
malformed UTF-8, oversized arguments/results, invalid positive callback values,
and malformed result structs fail closed. Session cancel/close invalidates
pending calls. Join host completion workers before runtime destruction, then
call `tny_tool_call_release` exactly once per async handle and unregister after
closing the session. Registry and host references are separate: close detaches
and invalidates its reference, so a late completion safely returns
`TNY_STATUS_BAD_STATE`; memory is reclaimed when the host releases. Unregister
guarantees no later invocation callback. `tny_capabilities_v1` reports all
registry and byte limits. See
[ADR 0038](adr/0038-libtny-custom-tools.md).
At most 64 registration objects are admitted during one runtime lifetime,
including inactive tombstones, bounding memory while preserving deterministic
repeated-handle rejection. The wasm CLI does not expose this registration API
and claims no async custom-tool parity.

## TLS and platform capability

macOS reports `macos` / `securetransport`; Linux glibc reports
`linux-glibc` / `openssl-dynamic`. `tls_implementation` describes compiled
support and the TLS feature's enabled bit says whether the selected endpoint
uses it. Reachability is learned only from normal runtime traffic.

A static archive linked into an otherwise dynamic Linux application could
still load system OpenSSL; the unsupported case is a **fully static musl final
executable**. ABI 0 does not ship that public artifact. Windows Schannel,
host-supplied transports, and bundled static TLS are separate follow-ups.

## Verification

`tests/integration/test_libtny.py` installs into a clean prefix, compiles C and
C++ consumers, runs complete strict-mock turns through the dylib/so, exercises
the Python `ctypes` FFI, drives simultaneous isolated runtimes, repeats
out-of-order teardown, wakes a blocked event pull by cancelling from another
thread, checks one interrupted terminal, and proves a native permission can
park and resume without executing a denied write.

`make test-libtny-tsan` is the Linux x86_64 race gate. It builds libtny and a
native C host with GCC ThreadSanitizer from process startup, then stresses four
independent owner-thread runtimes and concurrent scheduler-thread cancellation
across repeated simultaneous strict-mock turns. Exact-one interrupted terminal,
drain, a successful post-cancel turn, isolation, wrong-thread rejection and
owner-ordered teardown are checked inside the native host. Python only owns the
mock-server lifecycle; it never
loads the TSan library. The target is not advertised on macOS, arm64, musl,
Windows, or wasm; their existing functional and sanitizer jobs remain separate
and must not be described as TSan evidence.

The native `libtny-sanitizer-host` starts with ASan/UBSan already linked. It
repeats pointer-to-pointer destruction, parent-owned active-session cleanup,
partial-constructor cleanup, and a hostile provider request for an unadvertised
process tool; that tool cannot spawn and the library writes no host stdio.
Linux keeps LeakSanitizer enabled. Darwin's ASan runtime reports leak detection
unsupported, so macOS runs the same native host with ASan/UBSan while Linux is
the leak-sensitive gate.
