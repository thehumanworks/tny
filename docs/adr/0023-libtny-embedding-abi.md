# 0023 — libtny is one headless runtime with an experimental pull-driven C ABI

Date: 2026-08-24
Status: accepted (adds a native embedding surface; the JS embedding non-goal
in `product.md` remains in force)

## Context

The CLI, TUI, and ACP server currently orchestrate the same backend lifecycle
independently: create a backend, connect, create or resume a provider session,
bind the native loop, send a turn, poll and dispatch, answer permissions, save
the host pointer, and tear everything down. That duplication makes an
embedding API unsafe to add as another wrapper: it would create a fourth turn
loop with subtly different cancellation, permission, and error behavior.

tny already has the correct semantic seams. Every provider maps onto the
normalized event vocabulary in `src/core/events.h`, and the private backend
contract in `src/core/backend.h` exposes connection, turn, steering,
permission, poll, and shutdown operations. Those headers are not a public ABI:
they expose mutable layouts, borrowed pointers, C enum layout, yyjson-owned
state, POSIX `pollfd`, and provider implementation details.

The public library must be usable from C and ordinary FFI generators without
turning tny into a second implementation or weakening the CLI's size, startup,
prewarm, wasm, permission, and session behavior.

## Decision

### One engine, several adapters

Introduce one private runtime engine that owns the backend lifecycle. `tny
ask`, the TUI, the ACP server, and the public library are adapters over that
engine.

```text
  ask / TUI / ACP server / C embedder
                    |
            private runtime engine
        (session + turn + event queue)
                    |
          private backend contract
    openai / cursor / codex / acp client
```

The public ABI never exposes the private backend vtable. External agents keep
using ACP and external tools keep using MCP. A binary backend/plugin ABI,
custom in-process tools, host storage, host HTTP/WebSocket transports, and a
terminal-embedding surface are later decisions.

### ABI-0 scope

The first external artifact is an **experimental headless shared library**:

- macOS arm64: versioned `libtny.0.dylib`;
- Linux glibc x86_64/aarch64: versioned `libtny.so.0`;
- one standalone public header and pkg-config metadata.

The shipped `tny` executable links a private archive so it remains a single
file and dead stripping keeps the existing size/startup budgets. A public
static archive is deferred until vendored yyjson/picohttpparser/wslay symbols
are isolated from copies an embedding process may already link.

MSYS2 and wasm continue building and testing the CLI/runtime sources, but ABI
0 does not claim a public DLL or wasm library. Native Win32, Windows Schannel,
bundled TLS for fully static musl, a JS SDK, and new platform targets require a
separate decision. In particular, adding vendored cryptography is not part of
this ADR.

ABI 0 supports one runtime per process, one open session in that runtime, and
one active turn. A second runtime fails with `TNY_STATUS_BUSY`. This is a
temporary implementation limit, not a permanent ABI promise: a later release
may allow more runtimes without breaking callers.

### Public object and ownership model

The public header declares incomplete types only:

```c
typedef struct tny_runtime tny_runtime;
typedef struct tny_session tny_session;
typedef struct tny_event   tny_event;
typedef struct tny_error   tny_error;
```

The runtime owns its copied configuration and process/transport resources. A
session belongs to exactly one runtime; a turn belongs to its session. Event
and error objects contain library-owned copies and are released only with
`tny_event_free` / `tny_error_free`. A child handle must be closed before its
parent. Closing a parent closes its children in the documented order.

Inputs retained after a call are copied. ABI 0 accepts UTF-8 text without
embedded NUL bytes; arbitrary byte payloads are a later extension. Public
constants have fixed `uint32_t` values rather than public C enums or `bool`.
Options structs start with `struct_size`, include zeroed reserved fields, and
have initializer functions. Adding fields is append-only within ABI major 0.

The header contains C++ `extern "C"` guards plus `TNY_API` and `TNY_CALL`.
Product SemVer and the runtime ABI major/minor are separate queries. Internal
symbols have hidden visibility; the shared-library export list is exact.

### Configuration and authority

The library does not implicitly adopt CLI authority. Zero-initialized public
options mean **ask mode**:

- safe native operations proceed according to the existing permission rules;
- a sensitive native or host request emits a permission event and parks;
- the embedder must answer it explicitly or cancel/close the turn.

The CLI adapter explicitly retains the accepted ADR 0001 behavior and opts
into yolo by default. It also retains provider auto-detection, settings/env
loading, session persistence, and the existing prewarm/overlapped-connect
optimizations.

Public runtime creation receives explicit workspace, provider, model,
state-directory, persistence, and settings/environment opt-in fields. Caller
memory is borrowed only during creation. Credential copies owned by the
library are wiped before release; the library cannot wipe caller or process
environment storage.

Public ABI 0 disables MCP. MCP tools are not advertised and no public runtime
path may reach the current process-global server table. The CLI and ACP server
retain current MCP behavior behind the one-runtime process rule until MCP
ownership moves into the runtime. Capability discovery reports this
difference rather than advertising tools which cannot run.

### Runtime and turn state machines

Runtime/session lifecycle and turn lifecycle are separate.

```text
runtime/session:  NEW -> READY -> CLOSED
turn:             ACTIVE -> WAIT_PERMISSION -> ACTIVE
                     |             |
                     +-> CANCELLING+
                                  |
                    ACTIVE ------> TERMINAL -> DRAINED
```

`turn_start` may validate, spawn, connect, and handshake. A failure before it
returns success is synchronous and creates no event stream. Once it succeeds,
every later failure is represented by an optional `ERROR` event followed by
exactly one `TURN_END`. The runtime synthesizes that pair when a transport dies
without a backend terminal and suppresses duplicate backend terminals.

`next_event` drives the same private poll/dispatch engine used by the CLI
adapters. Its result distinguishes event, monotonic-deadline timeout, terminal
stream drained, and synchronous API error. The TUI and ACP server use a
private prepare-poll/dispatch interface so they can multiplex keyboard/client
input with backend fds without periodic timer polling. POSIX `pollfd` never
crosses the public ABI.

All ABI-0 runtime/session operations, including cancel, are owner-thread
affine. `next_event` timeouts let language wrappers return to their scheduler.
Cross-thread cancellation will require an atomic request plus a private wake
fd (or platform equivalent) before it can be promised. Cancellation is
otherwise asynchronous: the caller continues draining through `TURN_END`.

`close` while active requests cancellation, drains for a configured bounded
shutdown timeout, then performs transport/child forced shutdown. No public
callbacks or events occur after close returns. Post-fork child `_exit(127)` is
part of child failure isolation and is not an exit of the embedding host.

The TUI's provider/model/tier/effort/workspace changes remain owner-thread
operations. Its private adapter either reconfigures a quiescent runtime or
destroys/recreates it while reattaching the open tny session. A prewarmed
backend can be adopted only through a private runtime operation that preserves
the frozen resume-pointer/provider-owner check. Prewarm is an acceleration
layer over the engine, not another lifecycle implementation.

### Asynchronous permissions

Host backends already emit permission events and accept a later response. The
native OpenAI loop currently calls its prompt hook synchronously inside tool
execution; that must change before the public API is usable.

The native loop separates permission classification from tool execution. For
an unresolved sensitive call it copies the pending tool id/name/arguments,
enters `WAIT_PERMISSION`, emits one `PERMISSION` event, and returns to the
driver. `respond_permission` resumes the parked call. A stale id, wrong-turn
id, duplicate response, or response outside `WAIT_PERMISSION` returns
`TNY_STATUS_BAD_STATE`. The same correlation rule applies to host permissions.
Cancel, close, and transport failure while parked deterministically resolve
the pending call and still produce one terminal event.

### Owned bounded events

Backend callbacks currently carry borrowed pointers, sometimes into temporary
buffers. The runtime copies every event payload before backend dispatch
returns. The queue has explicit event-count and payload-byte budgets, with
reserved capacity for one bounded error and one terminal event. Permission,
error, and terminal events are never silently dropped. Overflow cancels the
backend, emits a `BACKPRESSURE` error if possible, and terminates the turn once.

The exact initial limits live in one internal header and capability/status
output; tests exercise a dispatch burst larger than the normal tool-call
burst. Changing the limits does not change the ABI.

Steering keeps ADR 0013's ownership rule: a successful `steer` transfers the
copied text to the backend. A later refusal returns that text in ordered
`STEER_REJECTED` events before the turn's terminal event.

### Blocking and deadlines

Initialized runtime options contain bounded connection and shutdown timeouts.
Creation may read explicitly enabled local settings/state files. `turn_start`
may spawn/connect/handshake up to the connection deadline. `next_event` blocks
only to its caller-provided monotonic deadline. Close waits only to the bounded
shutdown deadline before forcing child cleanup. Defaults and maximum accepted
values are documented in the public header; invalid values fail before side
effects.

### Errors, capabilities, and diagnostics

Stable numeric error categories cover invalid argument, bad state, busy, OOM,
configuration, authentication, I/O, timeout, unsupported, protocol,
backpressure, cancellation, and internal failure. Synchronous failures return
an owned `tny_error`; failures after a turn starts use the same category in an
error event before the terminal event.

ABI 0 documents its TLS/platform matrix instead of freezing a placeholder
runtime probe. A later capability API must distinguish compile support,
selected transport source, local initialization, and endpoint reachability.

ABI-0 embeds the native OpenAI-compatible backend. Its transports are
SecureTransport on macOS and dynamically loaded system OpenSSL on Linux
glibc. Fully static musl, Windows/MSYS, Cursor, Codex, ACP, and wasm remain
internal CLI capabilities rather than public ABI-0 library claims.

The library never prints to stdout/stderr, installs process signal handlers,
or exits/aborts the embedding host for ordinary failures. Diagnostics go to a
non-reentrant owner-thread logger sink. Child stderr is captured/drained, never
left inherited in a way that can corrupt host output or block a child.

The shared-library build contains an allocation boundary covering the public
adapter, runtime, OpenAI-native loop, transport buffers, and yyjson documents.
Allocation exhaustion before a turn starts returns `TNY_STATUS_OOM`; after a
turn starts the engine uses a constructor-time reserved `ERROR` (`OOM`) plus
`TURN_END` pair, so settlement itself does not need to allocate. The CLI keeps
ordinary libc allocation calls and does not pay for the test injector.

The library path does not print to host stdout/stderr or install fatal handlers.
Its OpenAI transport has no provider child process. Native tools which spawn a
process (`terminal`, `open_file`, and `subagent`) are neither advertised nor
accepted in library mode, so public teardown has no child process which can
outlive it. An active HTTP stream is cancelled by closing its transport during
session/runtime destruction. Connection establishment remains bounded by the
native 15-second connection deadline, and `next_event` remains bounded by the
caller's validated deadline.

Every library-owned API-key copy is released through `secure_free`; the
temporary HTTP authorization header is explicitly zeroed before its buffer is
released on success, cancellation, and failure paths. Caller memory and process
environment storage remain outside the library's ownership.

## Verification gates

Implementation is accepted only with:

- existing `make test` after each adapter migration;
- mutation coverage for the permission, event, and terminal-state changes;
- exact event-order tests for success, transport death, cancellation,
  permission pause/resume, stale permission response, steering rejection, and
  queue overflow;
- repeated create/close and second-runtime-busy tests under ASan/UBSan;
- `make test-libtny-fault`, which builds a test-only fault-injection library and
  sweeps named public-call allocation scopes in isolated host processes;
- bounded child shutdown with no leaked host/MCP process;
- standalone C11 and C++ header compilation;
- a clean-prefix shared-library C consumer and one Python `ctypes` consumer;
- an exact `nm` export allowlist and ABI snapshot;
- macOS arm64 and Linux glibc shared-library build/package lanes;
- local trusted/untrusted TLS fixtures on each published library platform;
- `make size-check` and the existing startup/TTFT gates with no regression;
- the existing wasm CLI parity suite, without adding a public JS library.

The current issue-24 fault lane is deliberately bounded: allocation indices
1–48 for runtime/session construction, 1–96 for turn start, 1–48 for event
pull and permission response, plus active-turn teardown. Each case has a
20-second process timeout and asserts empty host stdout/stderr. The ordinary C
suite runs under ASan/UBSan; the test-only fault shared library is not itself
sanitizer-instrumented. Exhaustive allocation-count discovery, a sanitizer-
instrumented fault host, and a completed focused mutation run remain required
before treating issue 24 as fully closed. The native connection setup bound is
15 seconds; the tested active-stream teardown bound is one second.

## Consequences

- The CLI, TUI, ACP server, and embedders share one lifecycle and event-order
  implementation.
- The first public surface is deliberately smaller than tny's CLI surface.
  MCP, multi-runtime operation, static embedding, host transports, native
  plugins, broad language SDKs, and terminal embedding can be added without
  exposing today's internals.
- The CLI keeps its current default authority, prewarm, wasm, and single-file
  release behavior. The embedding API starts safer and more explicit.
- A real public C ABI adds work which libfx's JS/N-API package does not solve:
  symbol visibility, calling convention, object ownership, error/status
  compatibility, install names/SONAMEs, and FFI misuse tests.
