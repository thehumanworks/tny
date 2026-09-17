# PR148 integration evidence

Initial state: local 2d710b6, remote feature 601caee, remote main cf423b8.
Local fixes are authorized for integration. Remote feature already includes main.
Existing hosted failures: Windows GCC LTO ICE (responses and runner),
Valgrind GCC internal-linkage warning, runtime mutation oracle mismatches,
Nix GCC array-bounds and Darwin SemVer/link failures.
Native goal omitted under higher-priority tool authorization rule.
Gate: INCOMPLETE.

## Integration and reviews

- 1d867bb preserves every pre-existing dirty fix and review artifact.
- 6b12285 merges published feature 601caee; ort automatically combined Makefile.
  Published feature includes remote main cf423b8. No conflict markers remain.
- Independent integration_plan_review: preserve remote ancestry and add explicit
  leak/ABI/size gates; all accepted in the contract amendment.
- Independent integration_code_review: read-only production corrections plus
  Makefile, Nix, runtime fixture and mutation-oracle changes; no blocking findings.
  Confirmed cancellation idempotence, allocator-latch OOM classification, explicit
  lease release, unchanged payload lifetime assertions and strict mutant baselines.
- Focused native request/lifecycle run exits 0: 36 tests, 28,812 assertions on
  integrated checkpoint tree. Earlier postreview counts are not reused as proof.
- Initial new numeric-version regression failed because the fixture supplied an
  explicit override and requested compatibility-library inputs it does not own.
  Corrected to remove fixture override and exercise lib-shared-active.
- GCC14 local compilation did not reproduce Nix GCC's array-bounds diagnostic.
  The fixture now copies a complete known-size template into the same heap buffer;
  overwrite/free and retained-event assertions are unchanged. Hosted GCC/Nix is
  the required reproduction environment.

## Local gate progress

- `make test-cpp-build`: PASS, 14 tests (one Linux-only analyzer fixture
  skipped on Darwin); numeric environment override and Windows LTO flag tests pass.
- `make test` initial run: 567 unit tests pass; OpenAI/background integration
  failed because the parent environment defines TNY_TOOLS. Cancelled that owned
  run and restarted as `env -u TNY_TOOLS make -j4 test`; no policy defaults changed.
- Preserved source fixes verified byte-for-byte against checkpoint 1d867bb;
  all baseline ADR hashes match. Remote main is an ancestor of integrated HEAD.
- Stripped Darwin release candidate: 1,189,536 bytes, below 6,000,000.

- `make -j4 quality`: PASS. Darwin explicitly skips GCC analyzer; hosted Linux
  quality is mandatory. `make format-check lint-py` rerun after fixture updates.
- Environment correction: unsetting TNY_TOOLS before the Mise Python shim is
  insufficient because the shim restores it. Final runs use
  `mise exec -- env -u TNY_TOOLS make ...` with resolved tool paths. The earlier
  leak run's early assertion failures were likewise environment contaminated.
- Native mutation baseline revealed a normal batch-save timestamp rollover.
  Body-construction tests now compare every persisted field except `updated`,
  require `updated` to remain a string, and retain zero-settlement-allocation
  checks. Non-body cases keep byte equality. Independent integration_code_review
  rechecked this boundary and found no blocking issue; timestamp presence was
  strengthened following the review.

- Published 2ac54a7 matches remote PR head. Full local quality and followup
  formatting/lint checks pass. Native ownership 15/15 semantic mutants pass at
  `build/merge148-native/native-mutations/run-m40ueitf/report.json`. Both native
  leak fixtures and clean-environment general `make leaks` exit 0 with zero leaks.
- Hosted 35228495551 Windows job 105226101765 exposes the same GCC ICE in
  stream_decode.cpp:55 after Responses/runner exemptions. Extend the same narrow
  workaround to that observed module (ADR0130); Windows remains a required gate.

- Windows eda78cf run 35229000001/job 105228193383 no longer reports the
  observed ICEs, but checkpoint recovery references a missing LTO-private
  unique_ptr destructor clone. Independent reviewer recommends a consistent
  private C++ native-object boundary on Windows/GCC, retaining C/final-link LTO.
  ADR0131 supersedes the per-file workaround; maintained tests enumerate all
  C++ objects and preserve override/Clang/non-Windows cases.

## Final integration validation

Code candidate: 026ba9b638144ac9cf39c0fa99cb860e7f224fbd.
Independent review of the final Windows C++ boundary: no blocking findings.

| Check | Observed result |
| --- | --- |
| Native ownership mutations | 15/15 compiled behavioral mutants killed; source-bound report run-m40ueitf |
| Runtime mutations | 15/15 killed in isolated checkout at 026ba9b; final runtime suite 40 tests, 5,074 assertions |
| ABI | make test-abi exit 0; frozen ABI0 and both ABI1 baseline comparisons pass |
| macOS leaks | General make leaks and both native leak fixtures exit 0, zero leaks |
| Local quality | Full make quality plus subsequent affected format/lint/build checks pass; GCC analyzer belongs to hosted Linux |
| Windows | 35229616755/job 105230261464 PASS: release 1,156,096 bytes (limit 5,999,999), 560 unit tests pass, 7 platform skips; ownership and durable-job checks pass |
| Hosted Linux | Same code passes Valgrind, both musl builds, TSan and fuzz/mutation lanes |
| Wasm | Same code passes wasm-node |

Raw local reports are retained under ~/.cache/tny-merge-148/evidence/.
Hosted evidence: https://github.com/thehumanworks/tny/actions/runs/35229616755
Nix: https://github.com/thehumanworks/tny/actions/runs/35229616640
SDK: https://github.com/thehumanworks/tny/actions/runs/35229616424

Full local make test completed every suite but its version assertion saw the
branch advance during the run (binary 6b12285 versus Git 026ba9b). That run exits
2 and is not a pass. Repeating with the main checkout frozen at 026ba9b.

Hosted quality found the new Darwin numeric-version dry-run fixture needs to
set UNAME_M=arm64 as well as UNAME_S=Darwin when run on Linux x86_64. The
production platform support check correctly rejected the incomplete simulated
platform. Correct only the fixture; no product behavior or gate is relaxed.
This correction and documentation are published from the idle isolated checkout
so the ongoing root test's Git version remains frozen.

Final check rollup and merge readback are linked from
https://github.com/thehumanworks/tny/pull/148 .
I1/I3 preservation checks pass; I2 full-suite and hosted reconciliation and
I4 merge/main synchronization remain pending in this pre-merge record.

Final cross-platform fixture review: no blocking findings; UNAME_M=arm64
completes the simulated supported Darwin platform without changing product
behavior or weakening the version assertion. `make test-cpp-build` passes all
14 tests locally (one Linux-only fixture skipped); format/lint pass.

Hosted Linux full suite (job 105230261840) and both Linux Nix suites
(105230150082, 105230149987) finish with only this fixture failure. Their
previous GCC array-bounds failure is resolved. Publish the one-line fixture
correction with this evidence, then require the fresh complete hosted rollup.
