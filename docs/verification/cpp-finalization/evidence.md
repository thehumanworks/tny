# C++ finalization evidence

Contract: [contract.md](contract.md). Base: fdd5aa7 (main after merged PR140).
Branch: `fix/cpp-ownership-finalization`. No main merge is authorized.

## Publication checkpoint

Production implementation is independently reviewed. The reviewer approved the
ownership, factory and build changes; all four findings were dispositioned in
[review.md](review.md). Runtime marker and transaction reset mutants are rerun
against the corrected sources, not counted from invalid compilation.

Current passing checks with source manifests/exit records under artifacts/:

- macOS `make -j8 quality test-cpp-build` (201.178 seconds, exit0).
- Fresh transaction-ownership fixture: 32 abandon/unwind cycles, 6 admission
  allocation faults, 4 persistence faults, real lock contention, descriptor
  counts, reset/reuse and explicit commit. The post-review directory-OOM
  assertion also passes and confirms no record allocation after its failure.
- Generic mutation gate: valid mutants are exercised and killed, with a passing
  real baseline that now builds its required release CLI. Uncompilable mutants
  are excluded, and empty/all-invalid inventories fail closed.
- GCC14 supplementary Linux compiler probe: all8 private C++ units analyze
  cleanly; real factory positive control passes, actual uninitialized-read and
  use-after-free controls are rejected. No diagnostic was disabled.

The first development full suite exposed the missing explicit C++ CI pin;
that pin is restored and its regression passes. That failed development log
is not final acceptance evidence. Overlapping edits were reconciled, then
separate frozen-source suites were started; the release-candidate suite is
bound to the complete post-review patch.

## Outstanding at this checkpoint

The follow-up PR is opened as draft to obtain exact hosted Linux/Windows/wasm,
Nix and SDK results while the final frozen local suite, fault/sanitizer,
ABI/SDK/leak/mutation and performance checks finish. No pending platform is
claimed passing. Final results will replace this checkpoint before delivery.

The optional Modal environment reproduces GCC's issue and validates focused
ownership checks. Its directory-open probe follows a symlink even with
O_DIRECTORY|O_NOFOLLOW, causing two pre-existing task-authority tests to fail;
therefore that environment cannot certify the full filesystem suite. The
hosted native Linux jobs remain mandatory; no product test is skipped or
weakened to accommodate that environment.

## Policy and preservation

The strict decimal-six-MB ceiling remains 5,999,999 maximum bytes for native
and wasm+glue artifacts (ADR0121); no new budget changes. Public headers, ABI
metadata, historical ADRs and initial verification snapshots hash-identical.
The combined changes preserve concurrent transaction work and its independent
slice review, adding the missing injected directory failure and reset mutants.
