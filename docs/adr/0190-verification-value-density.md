# ADR 0190: Verification must earn its maintenance and execution cost

Status: accepted, 2026-10-19.

A proof of an independent model is useful design guidance, but only a
source-derived check or explicit production correspondence detects code drift.
Count invariant/fault coverage, not theorems, example count or CI job count.

Remove detached shell-mode/catalog SMT assertions and historical documentation
freezes. Preserve actual production fixtures, strengthen second-turn shell
consumption and provider-set checks, and retain source-linked C/Lean/Z3 gates.
Keep bounded-model guarantees explicitly distinct from implementation proofs.

Run full SDK conformance at the installed artifact boundary once per selected
runtime/platform, not twice with the same adapter on source and package. Keep
source unit/type tests, native reference checks, all platform/runtime lanes,
packaging/dependency checks and all sanitizer/fuzz/mutation gates. Bound compiler
parallelism rather than inventing a new build artifact protocol.

Require every CI job, including Valgrind, in the aggregate release check. A
static assertion over this configuration has concrete value because a missing
dependency can allow a false-green release gate.

The [audit and compact invariant map](../verification/value-density.md) document
what remains unproved, actual baseline timing and validation limits. Hosted
before/after timings are still required before claiming a speed improvement.
