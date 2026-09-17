# Evidence for checkpoint ownership (#142)

Baseline: `4e760be09908d61e23029486e1c9a3b91fbfcbbe`. Final verification is
performed on the isolated `feat/cpp-checkpoint-ownership` branch. The immutable
initial contract SHA-256 remains
`174316d68dca170aa16fdf0af7a3d97b77931405b62ffaeffb6d715816f78f3f`.

## Implementation and focused proof

The private checkpoint source is now C++20 behind the C facade. It owns context
reconstruction, JSON and temporary secret strings, checks required allocations,
and never mutates the borrowed resolved context during recovery. Required C
extension-construction failure paths now unwind safely. Runner packet creation
checks the private-context encoder result instead of emitting incomplete state.
The public ABI, persisted field schema, anonymous credential channel and C OS
seams are unchanged. ADR0126 records the precise language boundary.

Independent Fable design and code reviews are preserved with dispositions in
[reviews.md](reviews.md). The final focused suite passes ASan/UBSan with
**876 exhaustively discovered/injected allocation failures across the test
configurations**, plus a separate failed-checkpoint/engine-settlement/retry
probe. It checks every hit, caller bytes and pointed-to values, lifetime,
partial arrays, identity/authority rejection and the unresolved-backend sentinel.
The final **11/11 behavioral mutants** compiled, linked and were killed by
runtime assertions or sanitizer diagnostics; compilation failures never count.
The final native full fixture reports **0 leaks / 0 leaked bytes** under macOS
`leaks --atExit`, alongside the separate sanitizer run.

All **567 core unit tests / 32,022 assertions** now pass. The broader integration,
ABI and complete quality runs are being reconciled before delivery. This initial
implementation commit does not claim those still-running gates passed.

## Verification environment and failed-attempt transparency

Use resolved tools with an allow-listed environment and a fresh HOME. The
host's mise configuration re-injects `TNY_TOOLS=terminal` through Python shims;
that contaminated early baseline extension/ephemeral fixtures. An additional
provider setup baseline failure disappeared under the clean fixture environment.
No user credentials or configuration were changed. Direct fixtures receive an
absolute `TNY` binary path. The original checkout and all unrelated worktrees
remain untouched.

The coordinator's first complete candidate unit gate caught an overly strict
new backend bound. The existing -1 unresolved private-context sentinel is now
preserved and explicitly tested. Reviewer-identified invalid-backend and
null-model-routing problems were demonstrated as failing runtime tests before
fixing them. A verification attempt using a custom BUILD directory failed ABI
fixtures that intentionally refer to `build/lib` and `build/pic`; the canonical
ABI target is rerun without that override, rather than changing tests or copying
unverified libraries into their expected locations.

Worker-only earlier counts and performance measurements are retained as
historical evidence in [implementation-log.md](implementation-log.md), not
substituted for final-source results. Final gate summaries, source fingerprints,
measurements and hosted status will be appended in the delivery evidence commit.
