# ADR 0124: Analyzable ownership and complete build inputs

Date: 2026-09-17. Status: accepted for C++ finalization.

## Context

The merged private-C++ migration passes its runtime safety tests, but GCC 14
reports uninitialized values for non-inlined `std::unique_ptr` factory returns.
A minimal independent malloc/placement-new/unique_ptr program reproduces the
same report, as does an ordinary `std::make_unique<int>` wrapper, on GCC 14
and GCC 15. Changing smart-pointer representations to placate that limitation
would add complexity without improving the ownership guarantee.

The merged Nix test source also omits `nix/package.nix`, which its actual
artifact-policy test reads. Release-only source archives eagerly read the
unit-test inventory just to calculate a leak-target variable. Generic mutation
baselines build the unit executable but omit the sibling CLI that a runner
unit test executes, allowing a missing build prerequisite to block verification.

## Decision

Keep standard move-only RAII, the existing allocator, C-facing facades and all
failure semantics. Inline the three short ownership factories (`make_owned`,
`parse`, `make_document`) so GCC sees construction and transfer at the call
site. Do not suppress uninitialized-value, use-after-free or other diagnostics.
A compiler gate analyzes the real helpers and deliberately rejects independent
uninitialized-read and use-after-free controls after including those helpers.
It is a dependency of the existing GCC analyzer lane; it changes no release
compiler optimization policy or platform capability.

Keep source inputs explicit: include `nix/package.nix` in the test fileset and
compute the leak suite list only when that target needs it. A release-archive
regression actually removes `tests/test_main.c` and requires a clean build.
Prepare both release and test executables before a generic mutation baseline;
never count an unrelated missing executable as a behavioral mutation kill.

Exercise both engine-local OOM latch paths independently: allocation failure
while copying an event, and an event arriving after the provider's allocation
scope has already failed. Tests clear the provider scope before polling, check
two consecutive exactly-once ERROR/TURN_END settlements without allocations,
then prove successful recovery. Each latch has its own intentional mutant.

The artifact guardrail remains strictly below 6,000,000 bytes (ADR 0121).
Maintainability and measured performance remain priorities; no byte-oriented
redesign, exception removal, warning suppression or public ABI change is made.

## Verification

Run GCC 14 on all private C++ translation units plus the positive/negative
factory controls. Run the runtime regression and both marker mutants, archive
and Nix-input tests, full native ownership/fault/sanitizer/ABI/SDK/leak suites,
and the hosted platform matrix. Measure final startup, parser/event throughput
and memory against the same-host pre-series baseline. Outcomes belong in
`docs/verification/cpp-finalization/evidence.md`; this ADR alone is not proof.
