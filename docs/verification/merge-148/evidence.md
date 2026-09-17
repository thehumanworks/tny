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
