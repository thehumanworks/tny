# 0188 — Focus verification maintenance on production invariants

Date: 2026-10-18
Status: accepted

## Context

The repository has five Lean packages covering different kinds of evidence.
The code-cell checker translates actual production C into width-exact bit
vectors and proves overflow-sensitive arithmetic under its evaluation guards.
ACP, dictation and GUI packages prove independent specifications and export
tables replayed by the actual C/Rust implementations. These have continuing
production value, subject to their documented abstraction limits.

The code-mode language and Python-runtime benchmark helpers are different:
their 23-line and 35-line preregistered policies decide whether offline
experimental results meet acceptance and promotion criteria. They do not run
inside the shipped harness. Separate AST translators, Lean theorems and
translator mutation suites added a second verification implementation for
these small Python gates. The ordinary benchmark suite already exercised the
acceptance truth table, promotion boundaries and recorded language decisions.

## Decision

Remove the benchmark-only Lean translators and proofs. Exercise the actual,
unchanged policy functions from the ordinary offline suite instead:

- Preserve the complete eight-case acceptance truth table and promotion
  Boolean/count cross product. Add adjacent integer thresholds beyond binary64
  and uint64, non-vacuity and mutations for each guard and comparison.
- Cover every combination of labelled runtime-selection decision partitions:
  nonempty and complete corpus, complete trials, first/final nonregression,
  solved counts, cost per solved task, semantics and strict size improvement.
- Compare token ratios with Python's exact `Fraction`, including zero/negative
  solved counts, integer values beyond floating-point precision, equality and
  floor-division counterexamples. Check positive scaling of both arms.
- Replay the archived runtime decision and independently reproduce its size
  and semantics rejection reasons. Mutation tests use the same checks as the
  unmodified control and require weakened copies to fail.

Run these checks in `make test-code-mode-language`, `make test`, the existing
offline CI workflow and the Nix test derivation. Retire
`verify-code-mode-language`; it has no independent purpose once the benchmark
checks live in the ordinary test target. Keep the dedicated production
`verify-code-policy`, ACP, dictation and GUI proof gates. Nix still needs Lean
for the production C checker.

Keep preregistered policy files, recorded results, raw receipts, hashes and
golden fixtures unchanged. Historical proof results remain explicitly dated;
the removed proof sources remain retrievable from Git history.

Remove the redundant ACP catalog query that assumed a requested ID was absent
and contradicted that assumption. Its "cannot be made selectable" description
was misleading after ADR 0181. Retain catalog-listing provenance checks and
the current Lean model-selection proof and subprocess fixtures, which permit
unlisted IDs when the adapter confirms them.

## Guarantees and limits

The replacement benchmark tests are finite checks. They do not preserve the
former all-integer theorem or the AST whitelist's purity restriction, and
they are not a proof of experiment quality. The benchmark decision criteria
remain unchanged and receive direct, fast regression and mutation coverage.
Retaining the stronger production proofs avoids trading away general
overflow, normalization or lifecycle guarantees merely to reduce file count.

No provider, runtime behavior, public ABI, interpreter implementation or
production limit changes as a result of this decision. Test timing is recorded
in the cleanup audit; historical proof timings are not presented as a fresh
before/after benchmark.

## Non-mutating proof fixture checks

Keep full-package `lake build`, then run the ACP, dictation and GUI exporters
into a temporary directory through one `check_lean_golden.py` helper. Compare
both the complete relative file set and exact bytes. The previous in-place
export followed by `git diff` could modify tracked fixtures on failure and
miss newly generated untracked files. The new helper fails for extra, missing
or changed output, an empty fixture set, or a failed build/export, and does not
rewrite the committed tables. Focused negative controls use a local fake Lake;
the actual three packages are also checked with the pinned toolchain.
