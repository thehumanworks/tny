# ADR 0129: Integrated Windows runner LTO exemption

Date: 2026-09-17. Status at creation: accepted.

## Context

PR148 hosted run 35213882652, job 105177624005, reports GCC 15.3.0
`binds_to_current_def_p` internal compiler errors in both responses.cpp and
runner.cpp:1772 while linking the integrated checkpoint/native ownership tree.
ADR0128 covers responses; ADR0122 covers jobs. The runner failure remains.

## Decision

Include runner.cpp in the same native Windows/GCC release-only LTO exemption.
Retain -Os, exceptions and strict warnings. Every other graph/platform and the
final link retain their existing flags. Do not restructure ownership code or
disable LTO globally to work around a compiler internal error.

## Consequences and verification

This extends ADR0128's exemption list by one translation unit. No public ABI,
provider behavior or dependency changes. The maintained Makefile fixture checks
Windows/non-Windows release flags; hosted Windows compilation, runtime tests and
artifact size are required before merge. See ../verification/merge-148/evidence.md.
No performance improvement is claimed. Existing ADRs remain unchanged.
