# libtny fuzz gate

Corpus version: **1**.

`fuzz_libtny.c` owns every buffer and pointer it supplies. It exercises every
public sized initializer, runtime create/query and rejected event reads, plus
custom-tool descriptor/schema validation, sync/async results, generation
misuse, limits, UTF-8/NUL handling and ignored tails. It never starts a turn or
contacts a provider.

Sized APIs receive an exact-capacity allocation surrounded by canaries (and
poisoned redzones under ASan). `UINT32_MAX` is exercised with the public type's
full frozen size and must succeed without crossing it; larger capacities use a
non-null zero-capacity guarded pointer and must reject before touching memory.

Portable deterministic smoke gate:

```sh
make test-libtny-fuzz-smoke
```

Linux x86_64 Clang/libFuzzer gate (the CI command):

```sh
FUZZ_CC=clang FUZZ_RUNS=20000 FUZZ_SECONDS=60 \
  make test-libtny-fuzz
```

The libFuzzer binary uses
`-fsanitize=fuzzer,address,undefined`, leak detection, a five-second per-input
timeout, and the checked-in `corpus-v1` seeds. Crashes are written beneath
`build/fuzz-artifacts/`; any crash, sanitizer finding, leak or timeout fails.

The smoke gate also runs a positive class-coverage self-test and an expected
failing negative self-test. This proves the harness distinguishes accepted and
rejected capacities, creates, schemas and results, and reaches wrong-generation
rejection rather than merely returning successfully for every input.
