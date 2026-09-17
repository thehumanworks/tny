# Nix CI portability repair — 2026-09-17

## Scope and evidence

Base: `cf423b8` (`main`). Repair CI independently of the ongoing native-request
migration in PR #148; do not modify that branch or import its product changes.
No public API, application behavior, warning policy, sanitizer configuration,
mutation oracle, or CI matrix is changed.

Observed failures:

- Main run `35208523622`: GCC 15 array-bounds diagnostics in the allocation-
  instrumented runtime fixture fail both Linux Nix lanes. Constructing the same
  payload in a known-size array avoids the diagnostic without dropping any
  retained-payload or teardown assertion.
- The same run's Darwin fixture builds lose the numeric Mach-O version override
  while retaining a Git revision. This produces both the unrepresentable SemVer
  error and the fault-library `malformed ... version number: -dead_strip` error.
  Preserve the override through the environment when the fixture runner drops
  `MAKEFLAGS`, and honor it with `?=` in the Makefile.
- PR #148 run `35232590232`: the checkpoint mutation entry point rejects Darwin's
  single `-fsanitize=address,undefined` linker flag as a new command option.
  Bind compiler/linker flag values with `=` in both shared-runner recipes.
- Local verification exposed `gcc` incorrectly mapping to nonexistent `gc++` in
  automatic C++ driver discovery. Perform the specific compiler substitutions
  before the generic `cc` suffix transformation. Explicit driver overrides are
  unchanged.

The relevant runtime fixture and Darwin override repairs reuse PR #148 commit
`2ac54a7`; the full Darwin platform fixture follows `8a25773`; linker argument
binding follows `410cece`. Those commits are by Tomas Roda. This branch adds
independent regression coverage and the compiler-pairing correction rather than
importing the native-request migration or its Windows LTO changes.

## Verification contract

- [x] Instrumented runtime tests compile under Nix GCC 15.3 with `-Werror`;
      retained payload independence and teardown assertions remain unchanged.
- [x] All three Darwin library recipes preserve the environment-provided numeric
      version and command-line precedence, without changing the Git revision.
- [x] Parser/checkpoint recipes preserve empty, single dash-prefixed, and multiple
      linker flags through the real shared runner's argument parser.
- [x] Default C++ compiler discovery handles unversioned, versioned, cross-prefix,
      absolute-path, and compiler-cache command forms.
- [x] Regression controls fail against the original Makefile and pass after repair.
- [x] The actual runtime ownership, parser mutation, and checkpoint mutation gates
      pass with the Nix GCC toolchain and ASan/UBSan instrumentation.
- [ ] Full hosted quality/unit/integration/Nix matrix passes on the delivered SHA.
      The pull request's attached checks are the authoritative final result.

## Local evidence

`tests/integration/test_cpp_build.py`: 14 tests, 13 passed, one existing
Emscripten-runtime skip delegated to the hosted wasm lane. The Darwin regression
checks normal, fault-injected, and sanitizer libraries. Compiler discovery covers
seven command forms. Restoring only the ambiguous linker argument binding in a
disposable Makefile produces exactly two parser errors for the single sanitizer
flag; restoring the original compiler mapping produces three pairing failures.

GCC 15.3 runtime ownership: 40 tests passed. Parser ownership: four behavioral
mutants killed. Checkpoint ownership: complete allocation-failure sweep passed
and all eleven behavioral mutants killed. Mutants must compile/link and fail a
behavioral oracle; infrastructure failure is not counted as a kill.

Ruff formatting/checks and `git diff --check` passed. A read-only Claude Fable
review found no blocking findings, independently checked before/after behavior,
and confirmed that the runtime assertions remain intact. Its non-blocking request
to cover the fault-library version symptom was incorporated.

## Environment limits and final gate

The Modal image is Debian 12 with GCC 12, unlike the hosted CI toolchain. Pinned
mise tools were resolved to real binaries for isolated fixture subprocesses;
ambient credential-injecting wrappers were excluded. Nix GCC 15.3 was used for
ownership verification, and GCC 14.4 for the attempted full quality gate.

The broad local unit invocation encountered unrelated TLS-library discovery and
symlink-policy assertions in unchanged tests; assertion early returns also
produced leak diagnostics. The local GCC 14.4/Nix analyzer reports a null-argument
path in unchanged `src/util/util.c`. These are not suppressed or described as
passing. The initial Modal `nix flake check` compiled libtny but failed starting
a subsequent derivation with `unexpected EOF reading a line`; it is not a
successful full Nix check. Native hosted CI and the three existing Nix jobs must
validate the delivered commit. Final check results and links belong in the pull
request evidence, without a documentation-only commit invalidating that SHA.
