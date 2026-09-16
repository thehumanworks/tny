# ADR 0119: Build-lane parity and exhaustive ownership-fault proof

Date: 2026-09-16. Status: accepted for the #137–#139 feature branch.

## Context

The integrated ownership fixtures passed on macOS, but native GCC rejected a
partially initialized C++ test record, Emscripten rejected an unused argument
in an unsupported path, and sanitizer-disabled ownership targets selected
ASan objects. Archive builds also tried to enumerate source files with Git,
while a dirty checkout retained deleted C filenames in the format inventory.
A passing platform-specific invocation did not establish a portable gate.

Allocation-failure discovery can vary with stream scheduling. Taking the
minimum of discovery counts, or forgetting an observed index when later runs
are shorter, does not establish exhaustive coverage.

## Decision

1. Every ownership fixture uses the allocator-instrumented object graph, but
   its sanitizer flags follow `SANITIZE`. Sanitizer-enabled lanes select the
   existing fault/sanitizer objects; musl and MSYS select fault-only objects.
   The C and C++ owner definitions agree across the complete linked graph.
   Disabling ASan on an unsupported platform never disables allocation faults.
2. The source inventory includes tracked and untracked, non-ignored files in
   a checkout and existing first-party files in a source archive. Deleted
   paths, build outputs, vendored code and frozen ABI/benchmark fixtures do
   not become accidental formatting inputs. A Git-less discovery regression
   checks both inclusion and exclusion, with no missing-Git diagnostic.
3. Keep `-Wall -Wextra -Werror`, all enabled analyzers, exception handling,
   unwind metadata and existing platform budgets. Explicit value initialization
   fixes the GCC fixture; an explicit unused argument fixes the unsupported
   wasm branch. GCC native release LTO uses `-flto=auto` instead of producing
   a serial-LTRANS warning. Clang retains `-flto`; PIC, debug and wasm retain
   their existing independent flag sets. No diagnostic is suppressed to pass.
4. Sweep the highest allocation index observed across discovery runs. Retry
   only a missed injection, with a finite limit. An injected crash, failed
   settlement, output leak or failed recovery is immediately fatal. A once
   observed index that cannot be injected remains a failure, not a silently
   removed case. Seven independent Python regressions enforce those rules,
   including high-water growth and a tail that disappears in later runs.
5. Bind delivery evidence to the source bytes, command, toolchain and actual
   exit status. A compile failure is not a behavioral mutation kill, a skipped
   platform is not a pass, and an older successful snapshot is not final proof.

## Consequences and verification

The native, static and browser builds can run equivalent ownership checks
without changing product capabilities or the public C ABI. Archive and Nix
source sets need no Git repository. Scheduling variance can now expose a real
verification failure instead of being mistaken for complete fault coverage.

Relevant checks are `make test-cpp-build`, `make quality`, the parser/search/
runtime/runner ownership targets, `test_fault_sweep_inventory.py`, the full
allocation-failure and sanitizer gates, and the native/static/wasm matrix.
Current outcomes and source identities belong in the series delivery ledger;
this decision does not itself claim that every platform has passed.
