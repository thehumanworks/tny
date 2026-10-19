# Verification value density

Audit: 2026-10-19; baseline `40efe932` (merged PR #202).

**Verdict:** real boundary/failure tests and source-linked proofs earn their
cost. A green model that never consults production, assertions about historical
prose, and repeated execution of the same adapter are not equivalent evidence.
Test counts and theorem counts are not measures of stability or intelligence.

## Invariant map

| Goal / executable contract | Value and production connection | Limit / disposition |
| --- | --- | --- |
| Code-cell arithmetic and permission predicates: `make verify-code-policy` | Clang AST of actual `code_policy.c` translated to width-exact Lean arithmetic; independent properties and targeted C mutants | Keep. Translator is trusted; does not prove callers, CPython or OS effects. |
| Execution reply admission: `make verify-execution-protocol` | Production C AST to Z3, compiled oracle, positive witness and universal predicate obligations | Keep. Pure admission only, not whole RPC/process lifecycle. |
| Dictation lifecycle: `test_dictation.c` plus `verify-dictation-proofs` | Full finite lifecycle transition replay against C plus Lean model | Keep. String normalization/dictionary fixtures are finite, not an all-input C equivalence proof. |
| ACP admission/lifecycle: `test_acp_client.py`, `verify-acp-proofs` | Real subprocess fixtures replay model transitions; semver C replay | Keep, label scope. `reachable_safe` projects propositions stored in `Authority`; its reachability argument is unused. Not proof that external adapters enforce isolation. |
| GUI turn state: `gui/proofs/Export.lean` and Rust replay | Finite exported turn transitions connect specification to implementation | Keep. GUI Effort proofs have no exported implementation correspondence; design guidance only. |
| Shell disclosure: `test_tui.c`, `test_tui.py::test_shell_disclosure_reaches_provider` | Real shell execution and outgoing provider request, now including next-prompt non-repetition | Keep. Unit test manually clears its buffer, so only PTY checks production consumption. |
| Stream framing, cancellation, crash recovery, ownership | Split-boundary fixtures, process mocks, sanitizer/fault/mutation suites at their owning seams | Keep. These address failures abstract policy proofs cannot see; real provider/account service behavior remains untested offline. |
| Public ABI and SDK packaging | C reference plus full installed wheel/npm conformance and source tests/type checks | Keep the actual shipping boundary; remove seven repeated source adapter runs from CI. Same five Python/four Node matrix lanes. |
| Extension capability API: `test_extension_contract.py` | Independent fixture compared to real `doctor --json` provider/key/state/reason output | Keep, now rejects missing/extra providers and ignores JSON key order. Remove frozen competitor hook lists, prose headings and research hash assertions. |
| Release gate: `test_nix_ci_matrix.py`, actionlint | Every declared CI job must be a dependency and have a success check | Strengthen. Previously Valgrind ran but was absent from required aggregate. Static configuration assertions are justified here, not proof of runtime behavior. |

## Removed and consolidated

- Retired `shell_mode.smt2`, `acp_catalog.smt2`, and their unused 22-line runner.
  The shell model assumed its transitions before checking their negations;
  catalog identity queries restated selector definitions. Neither read production
  nor replayed production output, so arbitrary C regressions could remain green.
  This saves maintenance and misleading claims, not meaningful wall-clock time.
- Removed historical competitor/prose/pin checks from the capability API test.
  Removed obsolete Cursor/Codex provider expectations from its fixture. These
  were dead entries: the old test only iterated observed providers and therefore
  passed despite the stale expected set. No production capability changed.
- Node adapter conformance now runs on the installed npm package only (four
  lanes); Python adapter conformance on installed wheels only (three selected
  latest-version platform lanes). Source tests/type checks, reference C,
  discovery-wheel smoke, artifact hashes and dependencies still run. Existing
  adapters execute the same scenarios with their installed-package import mode.
- SDK builds use bounded parallel make (maximum four workers), propagated to
  later fixture builds. No new cache, manifest protocol or prebuilt bypass.

Do not conflate fewer checks with higher value. Full platform matrices, fuzzing,
ASan/UBSan, TSan, Valgrind, mutation and source-linked proof gates remain.
The standalone benchmark-policy check identifier remains for existing branch
protection; local `make test` still includes it. Artifact build fan-out was
considered but deferred: prebuilt modes and provenance machinery would add new
maintenance before establishing a reusable artifact boundary.

## Timing evidence and limits

Baseline SDK run [39268478033](https://github.com/thehumanworks/tny/actions/runs/39268478033):

| Baseline macOS job / stage | Observed duration |
| --- | ---: |
| [Node](https://github.com/thehumanworks/tny/actions/runs/39268478033/job/118114207155), native build | 4m59s |
| Node source tests + conformance + typecheck | 20m05s |
| Node packed artifact stage | 11m55s |
| [Python](https://github.com/thehumanworks/tny/actions/runs/39268478033/job/118114206961), native build | 5m05s |
| Python source tests | 10m29s |
| Python C reference + source conformance | 9m01s |
| Python installed-wheel stage | 3m38s |

Node's source test reporter accounted for 447.455 seconds of the combined step.
Its remainder is **not** guaranteed savings: first-use C debug compilation moves
into retained package conformance. Python's combined step includes retained C
reference work. Seven duplicate adapter executions are structurally removed;
actual critical-path and runner-minute savings require a new hosted run on the
same runner classes. No before/after speedup claim is made here.

## Local validation

- Source-linked execution admission: 9,600 compiled cases, seven obligations
  over 192 input bits, satisfiable non-vacuity witness; seven exporter controls.
- Two temporary C mutations (relaxed reply correlation and broken zero-ID guard)
  failed the compiled oracle. This detects real predicate faults but is not
  independent evidence of solver mutation sensitivity.
- Capability checker: unchanged and reordered output accepted; missing/extra
  provider, missing capability, wrong state and wrong reason rejected at the
  contract assertions using synthetic executable output. Real `doctor` output
  passes. Historical-document formatting is intentionally no longer enforced.
- actionlint, shell syntax and unchanged SDK matrix checks pass. Focused Python
  lint/format and CI configuration checks pass.
- Existing binary shell unit tests: four tests/24 assertions pass with LSan
  disabled because of executor ptrace. This is not a leak result or fresh build.
- Strengthened PTY test is blocked before provider contact by the executor's
  AF_UNIX listener restriction. It must pass hosted CI; no skip was introduced.
- The strengthened two-prompt PTY case passes as an explicitly in-process
  diagnostic (`TNY_ISOLATE=0`) against the existing binary. This does not replace
  the normal detached-runner gate, which is unchanged and must run in CI.
- Event/schema, conformance-contract, extension (33 cases), make-contract (five
  cases), and complete `make quality` pass: C/C++ tidy, strict warnings,
  whole-repository format/linters, C++ analyzer controls and 157-file GCC analysis.
- Full Lean packages, full integration suite, fresh SDK builds and cross-platform
  execution have not been rerun locally. Existing main results are not results
  for this change. Hosted checks must establish those outcomes before completion.
