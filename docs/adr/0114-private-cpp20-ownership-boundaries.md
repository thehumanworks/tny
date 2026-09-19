# ADR 0114: Private C++20 ownership boundaries

Date: 2026-09-16
Status: accepted for implementation of issues #137, #138 and #139

## Context

Parser fragments, retained events, asynchronous calls, and runner/job
resources need explicit owners and reliable cleanup through ordinary
failure. The C-only policy conflicts with this scoped user-requested
migration. Historical ADRs remain unchanged. Language choice alone is
neither a performance claim nor proof of protocol correctness.

## Decision

Keep C11 for existing application, scheduling, transport, OS-seam, SDK ABI,
vendored and tnytty sources. Permit C++20 only in private ownership and
decoding modules for the three issues. Preserve public C headers, exported
symbols, record layouts, capability flags, callback/affinity rules and
allocator ownership. Do not expose a C++ ABI or add a framework.

Use small move-only resource owners and synchronous borrowed views.
Retained payloads must be copied into owned storage; rebuild views after
moves rather than retaining pointers into moved small-string buffers.
Reuse the tny allocator for every new object/container/control block so
allocation-index fault sweeps reach them. Never replace global operator
new, use longjmp across destructors, or abort an embedding host on OOM.
C adapters catch exceptions before returning or calling C callbacks and
return explicit error status. Destructors are nonthrowing and do not
allocate, publish terminal status, or perform unbounded waits.

Queue settlement, generation checks, cancellation, process quiescence,
reaping and persistence remain explicit protocols. RAII does not replace
proof of those operations. Pre-exec/signal-only paths remain in C.

## Build and quality

Compile untouched C with the C compiler and C11 flags; compile private
C++ with the matching C++ compiler and C++20 flags. Link mixed objects
with that toolchain's C++ driver, including shared, fault, sanitizer, fuzz
and wasm outputs. Leave the frozen ABI0 source/build unchanged. Discover
C++ sources and headers in formatting, enabled analysis, strict warnings,
source packaging, CI and Nix. Emscripten must enable exception catching
at compile and link time. No blanket warning suppression or disabled
quality gate is an acceptable migration strategy.

## Measurement and size policy

Retain all then-current per-platform size ceilings initially. Measure stripped
artifacts and loaded C++ runtime dependencies separately and report both.
Any size-budget revision required a separately documented, measured
justification; [ADR 0150](0150-agent-first-harness-and-measured-footprint.md)
later removes numeric ceilings. Do not cite historical fx sizes as current
evidence.

Compare a clean pre-series baseline and each integrated candidate using
identical host/toolchain/flags/corpora. Shared startup thresholds are
those in the issue contracts, including cumulative comparison against
the pre-series baseline. Add reproducible PTY-first-prompt and parser/
event workload tooling where absent; TTFT is not first-paint evidence.

## Verification and rollout

Implement parsers/builds (#137), then events/async tools (#138), then
runner/jobs (#139), integrating each working boundary before extending
the pattern. Run all applicable original gates and behavioral mutants.
The current user requires one independent Claude Fable medium review;
record its findings and each disposition. Unavailable runtime platforms
remain unmet gates, not inferred passes from cross-compilation.
