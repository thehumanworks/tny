# ADR 0120: Measured Linux aarch64 C++ artifact budget

Date: 2026-09-16. Status: superseded by
[ADR 0121](0121-maintainable-cpp-and-six-megabyte-ceiling.md), itself
superseded for numeric ceilings by
[ADR 0150](0150-agent-first-harness-and-measured-footprint.md). The
measured sizes in this file remain historical evidence. Originally accepted
before final candidate acceptance.

## Context and authority

Issue #137 explicitly prioritizes startup, extensibility and reliable error
handling over obsolete executable-size ceilings, and authorizes a documented,
measured replacement policy. ADR 0114 initially retained existing ceilings
and requires a separate frozen policy before accepting a candidate. The first
integrated preflight is not accepted as a passing size gate.

On the same Ubuntu 24.04 aarch64 host, GCC 13.3 and binutils 2.42, stripped
pre-series `1d8ad71` measures 986,192 bytes and the integrated private-C++
preflight 1,051,840 bytes. The increase is 65,648 bytes (6.66%), but exceeds
that lane's old 1,048,576 ceiling by only 3,264 bytes. `readelf -lW` shows the
existing 64 KiB LOAD/RELRO alignment step described in ADR 0111. The preflight
read-only segment grows 36,499 bytes; the entire file moves a full 64 KiB
step. System C++ runtime dependencies are additional and separately reported.

Measured fresh-build experiments with `-Oz`, alternate inline limits,
whole-program/visibility controls and a single LTO partition do not restore
the previous file-size step. A shared bad_alloc throw helper removes 840
function-body bytes, but does not reduce the stripped file. Those speculative
changes are not adopted. Dropping exception handling, unwinding, CPU erratum
mitigation, RELRO or supported page sizes is not an acceptable budget fix.

## Decision (frozen at the time; numeric ceilings superseded by ADR 0150)

Set the **Linux aarch64/arm64 dynamic executable ceiling to 1,052,672 bytes**:
one MiB plus 4 KiB. This changes only that architecture's ceiling by 0.39%,
leaving 832 bytes above the measured preflight. It does not grant another
64 KiB layout step or reserve an arbitrary percentage of growth.

Keep every other cap unchanged: other Linux dynamic 1,048,576; musl static
1,572,864; Darwin 1,887,436; MSYS 2,097,152; wasm with glue 1,572,864 bytes.
The Makefile remains the packaging/install source of truth; CI and release
matrices mirror it. Explicit caller SIZE_MAX overrides continue to work.

Record the frozen numeric policy in `cpp-series/size-policy.initial.json`,
then rebuild/evaluate the final integrated source against it. Failed old-cap
preflight results remain evidence and are not relabelled as passes. Public
ABI, capabilities, fuzz/fault/mutation coverage, startup/first-prompt, TTFT,
parser/event throughput and memory gates remain unchanged. Report actual
stripped size, loaded runtimes and exact source/toolchain identities.

## Consequences and tests

This is a small, disclosed platform-specific policy change, not a claim that
C++ makes the executable smaller. It follows the issue's authorized tradeoff
rather than trading error handling for a historical rounded size threshold.
Current size comparisons with fx require remeasurement; old marketing sizes
are not new evidence.

Regression tests verify each platform's numeric policy, caller overrides,
CI/release agreement, and an actual size-check success at the frozen boundary
and rejection one byte above it. Packaged payloads retain the same checks.
Final acceptance also requires the unchanged performance and safety gates.
