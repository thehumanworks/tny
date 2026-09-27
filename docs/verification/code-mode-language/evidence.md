# Code-mode language evidence ledger

Date: 2026-09-27. Production baseline: `23852ca`.
Branch: `feat/code-mode-language-benchmark-20260927`.

## Provenance and delivered scope

The decision protocol was committed before inference in `47972c0`. The original
live harness is preserved in `7ed5441`; the experiment manifest additionally
hashes each input file and actual native executable at launch, including files
that were not yet committed. Later formatting, watchdog reconciliation, audit
helpers and CI integration do not replace that original manifest.

Production `src/`, `include/`, `third_party/` and the existing native runtime tests
are unchanged from the baseline. The alternatives are optional development probes,
not runtime options shipped to users. `data/decision.json`, the report and ADR 0175
retain Lua. No API key, auth store, account identifier or access token was copied
into benchmark evidence. The normal authenticated Codex CLI generated synthetic
programs; the runner did not implement or inspect the credential flow.

## Completed benchmark and deterministic gates

| Gate | Observed result | Evidence |
|---|---|---|
| Normal Codex login/model preflight | ChatGPT login; explicit `gpt-6-luna` request completed | Local `build/code-mode-bench/preflight/`; no credential contents |
| Reference runtime controls before inference | 108/108 variant executions passed | `data/controls.json` |
| Complete live cohort | 108 first programs, 109 total generations, 36 programs/language | `data/samples.json`, `data/experiment.json`, raw generation archive |
| Typed effect audit | All 327 recorded variant outcomes independently reconciled | `data/audit.json` |
| Raw-event reconciliation | Generated code and usage agree with actual CLI completion events | `data/audit.json`, `data/raw-generations.tar.gz` |
| Frozen-code replay under common two-second watchdog | All 327 pass/fail outcomes reproduced, including the original three failing variants | `data/audit.json`; no fresh inference or generated-code edits |
| Offline Python benchmark/evidence tests | 15/15 passed | `make test-code-mode-language` |
| Source-linked Lean proof | 18 general theorems and both observed retention decisions checked | `make verify-code-mode-language`, Lean 4.12.0 |
| Lean negative mutations | All four weakened policies rejected by actual Lean errors | Output/completeness/parity removal and reversed first-pass comparison |
| Local Lake configuration | Empty pinned package builds successfully | `lake --dir tests/formal/code_mode_language build` |
| First-party C probe diagnostics | Host, Lua, empty, JS and Python adapters passed `-Wall -Wextra -Werror -fsyntax-only` | Native headers; external interpreter headers treated as system headers |
| Measurement probes | Native sizes, 50 rotated serial timing samples/arm, separate parity diagnostics recorded | `data/native-timings.json`, `data/python-footprint.json`, `data/boundaries.json` |

The replay count is not a claim that every original attempt succeeded: the three
NDJSON variants in Lua's first attempt still fail, and its repaired attempt passes.
Failures remain in the sample and token denominators. The task-family bootstrap
and all published per-language aggregates are recomputed in the offline gate.
`data/SHA256.json` binds exported evidence; the gate checks its lengths and hashes.

## Completed production regression checks

| Gate | Observed result | Local receipt |
|---|---|---|
| Native release/test build and `make test-unit` | Exit 0; 589 tests, 589 passed, zero failed/skipped, 32,651 assertions, default ASan/UBSan instrumentation | `build/code-mode-bench/unit-final.log` and `unit-final.exit` |
| Restricted tool-profile regressions | Two separate one-test invocations passed, 12 and 15 assertions | End of the same unit log |
| Focused production Lua runtime suite | 6/6 passed, 68 assertions | `./build/tny-test -s code_runtime_suite` |
| Production execution integration | 18 cases: 17 passed, one explicit wasm-only platform skip | `build/code-mode-bench/execution-integration-final.log` |
| Existing production source-linked protocol check | Seven universal obligations over all 192 input bits, satisfiable non-vacuity witness, 9,600 compiled representative cases | `build/code-mode-bench/protocol-final.log` |
| Existing abstract SMT obligations | Four ACP-catalog and six shell-mode obligations proved | `python3 tests/formal/check.py` |

The unchanged production protocol predicate hash is
`e7a1b727207551197fe372d46dd320b4b6fd8b57021a75a82ce032dc5c2d910b`.
The final benchmark policy hash is
`d402679402a5f13ace64de82ca4a958043e6129bb444f7ee1e9350a24bb2374a`.
The Lean check prints the hash of the proof text plus the two observed-decision
checks on each run. Its main theorems use standard propositional extensionality
(`propext`), not admitted proofs or newly introduced axioms.

## Host setup, failed attempts and reconciliation

The first broad native verification attempts did not pass and are not represented
as successful runs. Their logs remain under `build/code-mode-bench/`.

The host's default PATH lacked Zsh although an installed Nix Zsh existed. Its
mise `python3` shim could not initialize some extension-host fixtures after their
HOME/cwd changed. The extension test then exited early, producing secondary
LeakSanitizer reports for fixture resources whose cleanup was never reached.
Using the concrete installed Python binary directory made both the isolated
extension test and all 589 native tests pass without changing production code,
tests, assertions, deadlines or sanitizer settings. The production execution
integration also passed with that same explicit Python PATH.

A size-policy fixture initially hit the host's `/tmp` quota while copying a
Python executable. A short disk-backed temporary directory fixed this: the
five-case size-policy suite and subsequent native unit target passed. No other
user's files were removed and no test was weakened.

The existing protocol AST translator rejects Clang 22's changed enum spelling.
An installed Clang 21 produced the supported AST, but its Nix default executable
loader did not match this non-Nix host. A build-local driver uses **unmodified
Clang 21 arguments for AST/syntax compilation** and adds only the actual host ELF
loader for executable links. It does not normalize the AST, suppress diagnostics,
change the proof predicate or skip obligations:

```sh
#!/bin/sh
compiler=/nix/store/0skhb15j2szcq05929gs9m1dbpiydhb7-clang-21.1.8/bin/clang
for arg in "$@"; do
    case "$arg" in
        -fsyntax-only|-E|-c|-S|--version) exec "$compiler" "$@" ;;
    esac
done
exec "$compiler" "$@" -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
```

The resulting existing source-linked protocol gate passed. This does not claim
that its default Clang 22 path is compatible; that separate toolchain issue is
outside this benchmark's production changes.

Successful native regression invocations used the concrete Python 3.14.7 bin
first in PATH, a disk-backed TMPDIR and the ordinary sanitizer configuration.
The test build set `TNY_VERSION=0.22.0-code-mode-bench` to avoid unrelated generated
version churn during checks. That string is build metadata, not a release.

## Explicit verification limits

No new full serial `make test`, aggregate `make quality`, full Nix derivation,
wasm build, cross-architecture build, musl packaging run or dedicated Valgrind
run is claimed. The broad combined quality/unit/formal attempts were interrupted
by the host issues above; the completed component gates are recorded separately.
The final scoped style/lint receipt is recorded below. Hosted workflow status,
when available on the PR, is separate revision-bound evidence, not a local claim.

The original six-second outer watchdog and unequal interpreter heap limits are
fully described in the protocol and report. The two-second replay repairs the
outcome evidence, not the already recorded original timing experiment. CPython's
installed shared library was not rebuilt under the probes' C optimization flags
or minimized into a frozen/static distribution. Runtime probes are not full
production migrations or a universal language-size lower bound.

Lean establishes properties of the translated pure policy definitions and their
observed inputs. It does not prove the entire scorer/parser/runner, statistical
confidence coverage, model identity or competence, C memory safety, operating-
system containment, exactly-once external effects or the whole Tny application.
The independent audit is a separate implementation of effect canonicalization
and event reconciliation, not a second independent author of the task corpus.

## Final scoped style/lint receipt

`make -j4 format-check lint-py lint-sh lint-workflows lint-js warn-strict`
completed with exit 0 using the installed Zsh path. Ruff checked all first-party
Python, formatting reported 742 files already formatted, all 42 JS files passed
syntax checks, workflow/shell linters passed, and both C/C++ strict-warning syntax
passes completed. This deliberately names the completed targets rather than
claiming the separate whole-project `make quality` target.

`data/verification-logs.tar.gz` contains the complete successful offline/Lean,
native unit, execution integration, source-linked protocol and scoped quality
logs. The evidence SHA256 manifest includes this archive. Earlier failed host
setup attempts remain local and are explained above rather than mixed into the
successful receipts.
