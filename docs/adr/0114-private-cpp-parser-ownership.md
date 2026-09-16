# ADR 0114: Private C++20 streaming parser ownership

- Status at creation: accepted
- Date: 2026-09-16
- Requirements: P1-I1 through P1-I5 in [phase 1](../verification/cpp-phase-1/contract.md)
- Supersedes the C-only implementation restriction for the area below

## Context

SSE/Connect buffers, event documents and tool-call fragments currently rely on
manual lifetime discipline. Every native provider turn crosses these parsers.
The migration must retain the C embedding ABI and deterministic allocation
failure handling without changing the owner of the agent loop.

## Decision

Authorize C++20 for SSE/Connect accumulation, Chat/Responses event decoding,
tool-call fragment ownership and their small private ownership helpers. C11
remains the language for untouched code, vendored libraries and tnytty. Keep
socket/TLS/WebSocket/HTTP chunk decoding, ACP and MCP in C. Keep scheduling,
tools, retry/checkpoint policy and the event loop in their current C owners.

Use move-only owners and the existing tny allocator through a compatible
allocator. yyjson remains vendored C with its existing allocator and deleter.
Borrowed views may exist only during a documented synchronous decode call;
retained values are copied into owned storage. No global operator new override,
new dependency, public C++ type, iostream or RTTI requirement is introduced.
Catch allocation exceptions at private C entry points. Return explicit OOM,
never malformed JSON or successful completion, on allocation exhaustion.
Destructors do not allocate or throw. Credentials remain in their existing
wiped C owners and are not copied into parser objects.

Build C and C++ separately with explicit source discovery, dependency files,
matching instrumentation, and a C++ link driver. Emscripten uses em++ with
exception catching enabled. Preserve the frozen C ABI0 compatibility build.
The formatter and clang analyzer cover both languages; GCC -fanalyzer stays
C-only with an explicit C++ skip (clang-tidy covers the C++ units).

## Alternatives and consequences

Keeping manual C owners does not establish the requested automatic cleanup.
Converting whole backends would also move scheduling and policy and expand
risk unnecessarily. A second JSON library or process-global allocation hook
would break the existing fault boundary. Private facades keep that boundary
small while adding a C++ runtime dependency that must be measured.

No new universal SSE or tool-argument cap is imposed: those consumers remain
allocation-limited, with checked arithmetic and explicit OOM. The 64 MiB
Connect frame limit and existing C consumer caps remain. Independent size and
startup policy belongs to ADR 0115; this decision makes no speed or size claim.

Split-boundary, lifetime, limit, fault, ABI, sanitizer and mutation results are
recorded in [evidence](../verification/cpp-phase-1/evidence.md). Unavailable
platforms and coordinator review remain explicit unmet gates until verified.
