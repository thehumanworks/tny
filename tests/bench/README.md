# Benchmarks

`bench_startup.py` (Python 3.14, standard library, POSIX) measures process
startup separately from `bench_ttft.py`'s Enter-to-first-token/local-mock turn.

```sh
make bench-startup BASELINE_TNY=/absolute/baseline/tny STARTUP_LABEL=phase-1
make size-report
python3 tests/bench/bench_startup.py --baseline /absolute/baseline/tny \
  --candidate /absolute/candidate/tny --json build/startup.json --label phase-1 \
  --build-metadata 'revision; compiler version; release flags'
python3 tests/bench/bench_startup.py --compare before.json after.json
make test-bench-startup
```

The `--compare` comparison is informational, not a contract verdict: it uses
the candidate observations in each input report without paired interleaving.
It prints deltas and exits 0 regardless of thresholds; invalid reports still
exit 2. A contract verdict requires a fresh paired benchmark. Use the
same host/compiler/flags and inspect metadata before interpreting deltas.
JSON retains raw latency and peak RSS samples in observation order, batch
ordering, p50/p95, artifact hashes, dependencies and thresholds. A sibling
`.md` summary is written automatically for a fresh paired benchmark. In that
mode, exit 0 means all startup gates pass,
1 a threshold failure, and 2 a measurement/argument error. `--size-only`
reports accounting, while `make size-check` still enforces native budgets.

Defaults: 102 observations per binary per CLI command, 21 per PTY, three
batches with paired baseline/candidate alternation (order reversed in batch
2). `--samples` and `--prompt-samples` cannot lower contract minima of 100/20.
No warmups or samples are discarded. Filesystem caches are not flushed; these
are fresh processes, not guaranteed cold-cache measurements.

The PTY is 80x24. The exact bytes `ESC[1m ESC[32m > SPACE ESC[0m` (without
spaces between escape sequences) identify the empty composer paint, as emitted
by `src/tui/tui_draw.c`; split reads are accumulated. The timer starts before
spawn, stops when those bytes are read, and excludes teardown. Each launch
uses an empty temporary HOME/workspace and an environment allowlist with no
credentials. `--provider openai` avoids host pre-warm; no Enter is sent. The
process is killed and reaped after detection. CLI timing ends at process exit.

RSS comes from `resource.getrusage(RUSAGE_CHILDREN)` inside a fresh worker per
launch, after reaping its sole child. It is peak child RSS through termination,
not idle RSS or incremental allocation usage. The worker fork is outside the
timer. Dynamic dependencies use `otool -L` / `ldd`; libc++ and libstdc++ are
explicit fields. Only trusted local binaries should be inspected with ldd.
Strip runs on a temporary copy. Adjacent `wasm/` artifacts are reported when
present; use `--wasm-dir` for a different directory. Missing wasm is marked
unavailable. Aggregate sizes may include both node and browser variants.

Reporting/budget policy is frozen in [ADR 0115](../../docs/adr/0115-startup-size-reporting.md).
The harness tests live here and are a `make test` prerequisite; Nix already
includes all of tests/ and the existing Python runtime. No additional package
or individually enumerated source input is required.
