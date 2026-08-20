---
name: tny-mutation-testing
description: Run and interpret tny's mutation-testing harness (tests/mutation/mutate.py) to verify that the test suite actually notices behavior changes. Use when asked to mutation-test a change, to "test extensively", or when adding code the unit suite might cover only nominally.
---

# Mutation testing in tny

## How to run

```sh
python3 tests/mutation/mutate.py            # full: unit kill, then pty integration for survivors
python3 tests/mutation/mutate.py --fast     # unit-only (minutes instead of an hour)
python3 tests/mutation/mutate.py --only tui_prewarm   # one file
```

The harness mutates one operator site at a time inside the functions listed
in `TARGETS`, rebuilds `make debug`, runs `./build/tny-test`, and (full mode)
rebuilds `make release` and runs `tests/integration/test_tui.py` for anything
the unit suite missed. Nonzero exit = at least one mutant survived.

## Scoping rule

Target **the code the current change touched**: whole new files/functions,
but only the changed lines inside pre-existing functions (use the per-target
`line_re` filter). Mutating untouched legacy code produces survivors that are
someone else's missing tests and drowns the signal.

## Traps learned the hard way

- **Stale-object false survivors.** macOS ships GNU make 3.81 with 1-second
  mtime granularity: a mutant written <1 s after the previous restore of the
  same file does NOT rebuild, so tests pass against the *original* code and
  the mutant is reported as survived. The harness bumps `os.utime` +2 s after
  every write and restore; keep that if you rewrite it. If a survivor looks
  absurd ("this must fail the suite"), re-apply it by hand and run the tests
  before believing the report.
- **Comment-text mutants.** Operator regexes match inside `/* ... */` and
  `//` text and "survive" trivially; the harness skips full-comment lines and
  trailing comments. An `EQUIVALENT` list exists for genuinely unobservable
  sites — annotate, don't delete the operator.
- **Hangs are kills.** Prewarm/condvar mutants can hang the test binary; the
  subprocess timeout counts as killed (the healthy suite runs in ~0.02 s).
- **Don't touch the tree or `build/` while it runs.** The harness rewrites
  target sources in place and relinks both build dirs; live-test against a
  copied binary (`cp build/tny scratch/tny-pristine`), never `build/tny`.

## Interpreting results

Kill ratio on scoped new code should be ≥95 % with only annotated
equivalents surviving. Every legitimate survivor is a missing assertion —
write the unit test that kills it (cheap) rather than leaning on the
integration stage (minutes per mutant).
