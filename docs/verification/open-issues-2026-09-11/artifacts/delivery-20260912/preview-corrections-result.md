The review findings match the current code. I will fix terminal cleanup and captured-byte ownership first, then add regressions for later-turn isolation, control validation, and bounded reads. I will preserve the worker changes and leave generated-result preview out of scope.
The terminal-path matrix now passes for extension stop, cancellation, denial, step exhaustion, and persistence failure. It checks the next turn’s actual request bytes, not just queue state. The new control and SSH regressions also pass. One broader core check failed because the harness supplied `TNY_NESTED`; I will rerun it with that variable unset.
Seven focused proofs now pass: each corrected version passed, each original version or injected fault compiled and failed the intended assertion, and each restored version passed. Quality also passes. The wasm build is blocked by missing `emcc`; the actual no-socket helper branch passes a native-ABI check. I also traced the leak-gate failures to `mise` re-injecting `TNY_TOOLS=terminal`; the clean rerun is in progress.
The full native `make test` run and macOS leak gate now pass after removing the injected tool-profile setting. Both hard-cancellation regressions pass, including cancellation while permission is parked and cancellation during a blocking tool. I am finishing the source-hash-bound evidence. The code will then be left unchanged for independent review.
Fixed the A15 captured-queue prerequisite in **14 authorized worker files**. Existing worker work is preserved. Generated-result preview remains deferred.

| Work completed | Checks and results | Blockers |
|---|---|---|
| Centralized preview cleanup. Stopped batches do not flush. Unsent transcript images are removed. Hard cancellation drains disposition before engine destruction. | Second-turn exact-byte checks pass for extension stops, cancellation, denial, step exhaustion and persistence failure. | Fresh independent review remains required. |
| Bounded opened-file reads and loaded-builder limits. SSH retains fetched bytes instead of rereading staged paths. | Deterministic growth, builder-refusal and staged-file mutation regressions pass. | None identified in focused native checks. |
| Full JSON-string length/NUL validation and exact reply correlation. | Malformed-field, role and reply-correlation regressions pass. | None identified in focused native checks. |
| Verification and evidence | **164 focused unit tests pass.** Preview/control integration: **16 pass, 1 wasm skip**. Image-input and SSH checks pass. Full `make test`, quality and macOS leaks pass. **Eight original/fault/restored proofs** compile and fail the intended assertions, then pass after restoration. | Actual wasm verification is blocked by missing `emcc`. The production refusal branch passes a native-ABI check, not a wasm-runtime check. |

Initial failed attempts remain documented. The broad test/leak failures were resolved by removing `mise`’s injected `TNY_TOOLS=terminal` from the test environment.

**Evidence:** [Report, file hashes and results]( /Users/tomas/projects/tny-open-issues-2026-09-11/docs/verification/open-issues-2026-09-11/artifacts/delivery-20260912/preview-corrections/REPORT.md). The same directory contains `after.json`, `corrections.patch`, `proof-final.json` and all gate logs.

Correction-patch SHA-256:
```text
77be569485f630bf0ac3bb5cd15cd51300a64a18cefcd5393d73b9d2d63469d9
```

No delegation, goals, repository commits or pushes. **Stopped for fresh independent review.**
