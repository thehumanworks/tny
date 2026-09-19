# ADR 0121: Maintainable C++ with a six-megabyte artifact ceiling

Date: 2026-09-16. Status: superseded by
[ADR 0150](0150-agent-first-harness-and-measured-footprint.md) for the
numeric artifact ceiling and competitor-size mission. Maintainability,
explicit ownership, reliable failure handling and measured speed remain
policy. Historical measurements in this file stay as recorded.

Originally accepted from explicit user direction. Superseded the
executable-size policy in ADR 0120 and earlier platform-specific caps, not
their historical measurements.

## Decision (historical; numeric ceiling superseded by ADR 0150)

Prioritize readable, extensible C++20 ownership, reliable failure handling and
measured performance. At the time, artifact size was a guardrail, not a
minimization target: the shipped tny executable had to be strictly below
**6,000,000 bytes** (decimal MB), including the wasm-plus-glue artifact. The
Makefile's inclusive maximum was 5,999,999; CI, release and Nix/install
checks reused it. Explicit SIZE_MAX overrides remained available for
stricter downstream builds. [ADR 0150](0150-agent-first-harness-and-measured-footprint.md)
removes that product ceiling; the measured sizes and maintainability
priority stay.

C++ runtime dependencies were reported separately from the executable. That
ceiling did not include optional external agent binaries, SDK wheel archives,
or debug-symbol/test executables. It did not authorize silent dependency
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

Boundary tests then accepted 5,999,999 bytes and rejected 6,000,000 for
native and wasm accounting, checked every platform used one default, and
ensured workflows did not replace the ceiling. Installed Nix payloads
retained Makefile-owned checks. Those numeric tests are historical;
[ADR 0150](0150-agent-first-harness-and-measured-footprint.md) removes the
product ceiling. All initial contracts and earlier ADRs remain immutable
records.
