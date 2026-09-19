# Instruction evolution: evidence record

Date: 2026-09-19. Base revision: `f90a610`.
Decision: [ADR 0153](../../adr/0153-bounded-instruction-evolution.md).
Contract and reproduction: [instruction improvement](../../instruction-improvement.md).

## What this proves

The optional controller can use measured feedback to accept a revision, use that
child as the next parent, and reject a later regression. It preserves evidence
and requires a separate verified promotion operation. The native proposer uses
tny's real detached sessions; fixture tests cover normal completion, launch-result
loss, timeout and controller interruption with confirmed session settlement.

This is **bounded instruction-policy evolution**, not model-weight training or
open-ended code self-modification. The published benchmark uses fixed,
manually authored strategies and a deterministic proposer/interpreter. It does
not demonstrate that a language model discovered these strategies or improves
on real coding tasks. No live-provider benchmark was authorized or run.

## Measured offline workload

The workload finds an exact key/value record in generated local files. The
baseline reads every file. Training feedback selects suffix filtering, then
header-only reads. A third candidate inspects only the first record: it is
cheaper on training cases but fails two validation cases, so the controller
rejects it. The accepted second revision remains the final parent.

| Split | Cases passed, baseline → final | Logical read bytes, baseline → final | File reads, baseline → final |
| --- | --- | ---: | ---: |
| Train | 3/3 → 3/3 | 27,627 → 315 | 24 → 12 |
| Validation | 3/3 → 3/3 | 34,272 → 390 | 24 → 12 |
| Held-out test | 3/3 → 3/3 | 26,298 → 300 | 24 → 12 |

The held-out reduction is **25,998 logical bytes (98.86%)** and **12 file reads
(50%)** on this deliberately small workload. The baseline is the initial
instruction policy under the same controller, **not the previous tny release**.
This is not a token, latency, API-cost or physical-I/O result.

The 30 evaluator executions read **113,969 workload bytes** in total:
**87,371** during search (baseline plus all accepted/rejected candidates on
train/validation), and **26,598** during the paired final test. Do not confuse
steady-state winner cost with search cost. These counts exclude fixture creation,
oracle reads, hashing, Python startup and controller/proposer overhead. The
benchmark does not measure those costs or claim end-to-end efficiency gains.

The corpus is purpose-built, with three cases per split. Repeated runs are
identical replay checks, not independent statistical samples. There are no
confidence intervals or claims of broad generalization. A live study needs
explicit provider authorization, a larger independent corpus, paired repeated
trials, full search/usage accounting and uncertainty estimates.

## Raw data and reproduction

- [`benchmark.json`](benchmark.json): all 30 case results, exact answer checks,
  workload-operation traces, fixture and source hashes, environment, full
  controller report and original archived evidence as UTF-8 strings.
- [`mutations.json`](mutations.json): pristine control and six targeted
  in-memory mutations of selection, parent reuse and evidence checks.
  All six mutants are killed by the existing tests. Compile/import errors are
  not counted as kills.

```sh
mise install
mise exec -- python3 tests/bench/bench_instruction_improvement.py --out /tmp/new-benchmark.json
mise exec -- python3 tests/mutation/instruction_improvement.py --out /tmp/new-mutations.json
mise exec -- python3 tests/integration/test_instruction_improvement.py
mise exec -- python3 tests/integration/test_improve_propose.py
mise exec -- python3 tests/integration/test_bench_instruction_improvement.py
```

Use new benchmark output filenames; it refuses overwrite. Scratch paths and
Python/platform metadata can differ. Fixture hashes, per-case workload results,
parent decisions and aggregate measurements reproduce. Original archive strings
retain scratch paths so their manifests remain independently hash-checkable;
reproduction regenerates fixture files instead of committing temporary inputs.
The benchmark checks that its source and controller do not change during a run.

## Verification contract

| Invariant | Evidence |
| --- | --- |
| Accepted children become parents; rejected children do not | Multi-round integration check; parent-reuse mutant killed |
| Per-case success cannot regress; cost cannot inflate; ties reject | Pareto checks, exact arithmetic boundary checks; four selection mutants killed |
| Validation/test data do not enter proposer protocol | Recorded proposer requests; final-only holdout checks; duplicate input/ID rejection |
| Invalid evidence fails closed | Nonfinite/Boolean costs, malformed/duplicate JSON, bad bodies, process errors, bounded-output and deadline tests |
| Promotion is explicit and preserves the baseline until verified | Archive replay/hash tests, stale/symlink/`..` containment checks, atomic failure and concurrent-change checks |
| Native launch is not confused with completion | Stub checks and real local HTTP/detached-runner tests, including interrupted launch recovery |
| No provider performance claim from synthetic data | Explicit benchmark scope; no live inference or latency/token claims |
| Runtime and packaging remain compatible | Native task unit check, install-prefix check; same release/Nix payload paths; no public ABI change |

## Check results

| Check | Result |
| --- | --- |
| Controller integration | 25 tests passed, including first-interrupt-during-cleanup and wrapper-descendant grace checks |
| Native proposal adapter | 9 tests passed, including real runner cancellation for SIGINT and SIGTERM |
| Benchmark integration | 12 tests passed, including fresh-run reproducibility and archive hashes |
| Targeted mutation campaign | Pristine control passed; 6/6 mutations killed |
| Install-prefix packaging | Passed, including both installed Python helpers and their help commands |
| Python compatibility | Both shipped modules pass Python 3.9 syntax parsing |
| `make quality` | Passed with pinned mise tools; GCC analyzer explicitly skipped on Darwin |
| `make leaks` | Passed: native macOS leak gate reports zero leaks |
| `make test` | Initial aggregate completed with three new-test failures. All were fixed and rerun successfully; a fresh final aggregate is in progress. This row is not a full-suite pass. |

The first quality attempt found a pre-existing formatting error in
`tests/fixtures/checkpoint_ownership.c`; the change includes that formatting-only
fix. The first aggregate encountered the integration runner's binary argument
in two new unittest entry points, plus an interrupted-launch identity race in a
new assertion. Both entry points and the assertion are fixed. Focused checks
above use the final versions.

The stripped macOS arm64 Release binary measures **1,087,584 bytes**. Its linked
runtime dependencies are `/usr/lib/libc++.1.dylib` and
`/usr/lib/libSystem.B.dylib`. This is an artifact measurement, not a size or
latency improvement claim. Python is an external, optional workflow dependency.

CI also found an existing wasm compile failure: the native `on_path` helper in
`cmd_doctor.c` became unused after provider removal. It now shares its caller's
wasm exclusion. A host compiler regression check exercises that branch with
`-Werror`; the actual wasm CI rerun remains the end-to-end check.

Nix and wasm execution were not run on this host. The workflow is native-local;
its source/packaging filters use existing dependencies. Python runs only when
explicitly invoked; the C11 runtime and permission defaults remain unchanged.
