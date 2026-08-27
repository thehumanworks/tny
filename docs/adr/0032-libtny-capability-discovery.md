# 0032 — libtny capability discovery is a sized side-effect-free snapshot

Date: 2026-08-27
Status: accepted

## Context

ABI 0 previously documented its platform and TLS matrix in prose. SDKs need a
machine-readable answer, but a boolean such as “supports OpenAI” cannot say
whether support was compiled, selected, initialized, or found reachable. A
query that probes the network would also make inspection surprising and could
leak authority.

Ephemeral runtimes separately required a caller-owned state directory even
though `persistence == 0` forbids durable session state.

## Decision

ABI 0.4 adds `tny_capabilities_v0`, initialized by
`tny_capabilities_init` and read with `tny_runtime_get_capabilities`.
The struct is fixed-width, sized, and append-only. Byte views borrow immutable
library storage or runtime-owned configuration and remain valid until runtime
free. Consumers ignore unknown mask bits, scalar values, and appended fields.

Provider support, selection, local initialization, and endpoint reachability
are distinct fields. Reachability starts unknown and changes only as a result
of ordinary turn traffic; the query never performs I/O. Available and enabled
feature masks similarly distinguish compiled support from runtime selection.

ABI 0.4 advertises only the OpenAI provider, the native HTTP/1 transport, the
published shared-library packaging, optional persistence, and platform TLS:
SecureTransport on macOS and dynamically loaded system OpenSSL on Linux glibc.
It does not advertise Cursor, Codex, ACP, MCP, custom tools, terminal
embedding, static packaging, cross-thread cancellation, Windows, wasm, or
fully static TLS. Threading, cancellation, and bounded event-queue semantics
are explicit scalar values.

An ephemeral runtime may omit `state_dir`. Its internal session remains usable
in memory, including its id and event identity, but no settings/session/history
path is created. Persistent runtimes continue requiring an explicit state
directory, and explicit state directories supplied by existing ephemeral
callers remain accepted without being materialized.

## Consequences

Language SDKs can inspect the exact public runtime they wrap without parsing
prose or triggering a provider connection. Future providers and packaging
forms extend masks and tail fields without changing the ABI-0 prefix. A future
explicit probe API, if added, must be a separately named authority-bearing
operation.

## Verification

The libtny integration test compiles clean-prefix C and C++ consumers, checks
the exact export lists, reads the snapshot through Python `ctypes`, verifies
prefix sizing and owner-thread affinity, covers initialized/reachable and
unreachable transitions, validates the macOS/Linux TLS matrix, and proves an
omitted ephemeral `state_dir` creates no filesystem state while persistent
omission is rejected.
