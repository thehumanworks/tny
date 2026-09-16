# ADR 0121: Maintainable C++ with a six-megabyte artifact ceiling

Date: 2026-09-16. Status: accepted from explicit user direction.
Supersedes the current executable-size policy in ADR 0120 and earlier
platform-specific caps, not their historical measurements.

## Decision

Prioritize readable, extensible C++20 ownership, reliable failure handling and
measured performance. Artifact size is a guardrail, not a minimization target:
the shipped tny executable must be strictly below **6,000,000 bytes** (decimal
MB). Apply the same guardrail to the wasm-plus-glue artifact. The Makefile's
inclusive maximum is 5,999,999; CI, release and Nix/install checks reuse it
instead of carrying separate platform-specific magic numbers. Explicit
SIZE_MAX overrides remain available for deliberately stricter downstream
builds and test fixtures; shipped workflows use the product default.

C++ runtime dependencies are reported separately from the executable. This
ceiling does not include optional external agent binaries, SDK wheel archives,
or debug-symbol/test executables. It does not authorize silent dependency
bloat or eliminate artifact measurements.

Prefer standard RAII and typed ownership rather than manual free tables,
raw owning pointers, clever layout tricks or source compression. Preserve
private C++ implementation boundaries and public C ABI. New C++ modules must
supply build, fault, cancellation and ownership tests; renaming C files without
improving ownership is not a migration objective. Operating-system/pre-exec
seams remain in C where their contracts require it. No library, framework or
whole-repository rewrite is introduced by this decision.

Startup/first-prompt thresholds, parser and event throughput/memory checks,
ABI compatibility, OOM recovery, exactly-once terminal settlement, descriptor
ownership, durability and cancellation deadlines remain unchanged. Never
trade these guarantees for a smaller binary. Performance claims require the
same-host baseline and actual final measurements, not the language choice.

## Verification

Boundary tests accept 5,999,999 bytes and reject 6,000,000 for native and wasm
accounting, check every platform uses one default, and ensure workflows do
not replace the ceiling. Installed Nix payloads retain Makefile-owned checks.
All initial contracts and earlier ADRs remain immutable; the continuation
contract records the user's superseding authority.
