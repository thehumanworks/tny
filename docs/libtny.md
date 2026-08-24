# libtny embedding API

`libtny` is the experimental ABI-0 headless C interface to the same runtime
used by `tny ask`, the TUI, and `tny acp`. It is not a second agent loop and it
does not expose tny's backend, yyjson, session-store, or pollfd layouts.

The design and compatibility contract are [ADR 0023](adr/0023-libtny-embedding-abi.md).

## Supported artifacts

ABI 0 ships a shared library on:

- macOS arm64: `libtny.0.dylib`;
- Linux glibc x86_64/aarch64: `libtny.so.0`.

The CLI continues to build on MSYS2 and wasm, but ABI 0 does not claim a
public Windows DLL or wasm/JavaScript library. There is no public static
archive yet: a private archive may link the single-file CLI, while a public
archive waits until vendored symbols are namespace-isolated.

## Build and install

```sh
make lib-shared
make install-lib PREFIX="$HOME/.local"
pkg-config --cflags --libs libtny
```

The installed surface is one header, the versioned shared library and its
linker name, and `libtny.pc`. The exact public symbol list lives under `abi/`.

An in-tree example is [`examples/embed.c`](../examples/embed.c). It creates a
runtime and session, starts a turn, pulls copied events, answers permissions,
and releases children before parents.

## Lifecycle

```text
runtime -> session -> event
```

- ABI 0 permits one public runtime per process, one open session, and one
  active turn. A second runtime returns `TNY_STATUS_BUSY`.
- Handles are owner-thread-affine, including cancellation.
- Every successful turn start ends in exactly one `TNY_EVENT_TURN_END`.
- `tny_session_next_event` returns event, timeout, or drained without exposing
  native file descriptors.
- Event string views are owned by the event and remain valid until
  `tny_event_free`.
- Asynchronous `ERROR` events expose the same stable status categories through
  `tny_event_error_code` before the terminal event.
- Inputs retained past a call are copied. ABI 0 accepts UTF-8 without embedded
  NUL bytes.

## Configuration and permissions

Call `tny_runtime_options_init`, then provide an existing workspace and an
explicit state directory. The public API does not read tny settings or choose
a provider from the environment. ABI 0 embeds the native OpenAI-compatible
loop. Cursor, Codex, and ACP remain CLI providers until their authority and
host-state contracts are explicit.

The default permission policy is `TNY_PERMISSION_ASK`, unlike the CLI's
deliberate yolo default. `max_steps` defaults to 0 (unlimited, matching the
CLI; [ADR 0024](adr/0024-unlimited-steps-default.md)); a positive value caps
model calls per turn and ends the turn with `TNY_STOP_REASON_STEP_LIMIT`. A sensitive native or host request emits a permission
event and parks until `tny_session_respond_permission`, cancellation, or close.
MCP is disabled and omitted from the advertised tool schema in ABI 0.
Home/ancestor instructions and home skill catalogs are never imported. Only an
`AGENTS.md`/`CLAUDE.md` in the explicit workspace may enter the prompt.
Process-spawning or ambient-state tools (`subagent`, skills/install, memory,
and interactive clarification) are also omitted from ABI 0.

Credentials are copied, never persisted by libtny, omitted from errors/events,
and wiped from library-owned long-lived storage at teardown. ABI 0 remains
experimental: deep legacy allocation paths may still terminate the process on
allocator exhaustion, as recorded in ADR 0023.

## TLS and platform capability

macOS uses SecureTransport and Linux glibc loads the system OpenSSL
dynamically. ABI 0 publishes only those shared-library platform families;
runtime TLS probing is deferred until it can report more than compile-time
facts.

A static archive linked into an otherwise dynamic Linux application could
still load system OpenSSL; the unsupported case is a **fully static musl final
executable**. ABI 0 does not ship that public artifact. Windows Schannel,
host-supplied transports, and bundled static TLS are separate follow-ups.

## Verification

`tests/integration/test_libtny.py` installs into a clean prefix, compiles C and
C++ consumers, runs a complete strict-mock turn through the dylib/so, exercises
the Python `ctypes` FFI, verifies second-runtime rejection, and proves a native
permission can park and resume without executing a denied write.
