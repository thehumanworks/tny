# ADR 0115: Startup and size reporting for the mixed-language series

Date: 2026-09-16. Status: accepted; reporting policy `0115-v1` frozen before
migration candidate evaluation. Scope: issue #137 item 6 and the cumulative
series performance comparison. Language policy is allocated ADR 0114.

## Decision

Retain every existing native and wasm hard size ceiling and the automated
`size-check` / `wasm-size-check` gates. No ceiling is relaxed until a measured,
same-target migration delta justifies a separately documented policy decision.
A language change alone supplies no such justification. Report both file size
and the size of a stripped temporary copy, SHA-256, dynamic dependencies, and
explicit libc++/libstdc++ matches. Never strip the benchmark input in place.
Report available wasm and JS/MJS files individually and in aggregate; missing
artifacts are unavailable evidence, never a zero-size claim. The directory
aggregate may contain both node and browser variants and is not a per-bundle
budget gate. Existing wasm-size-check remains authoritative for that gate.

The historical fx figures describe old artifacts and marketing claims, not a
current like-for-like comparison. A new comparison requires matched target,
version, configuration and measurement method; this series makes no fx claim.

Freeze the measurement method as follows: 102 fresh launches for each of
`--help` and `--version`, 21 fresh PTY launches, three deterministic batches,
with baseline/candidate paired alternation and reversed order in batch two.
Minimum user-selected counts are 100 and 20. Retain all ordered samples,
median and nearest-rank p95. CLI medians must be below 5 ms and added median
at most max(0.25 ms, 10% baseline); PTY below 10 ms and at most
max(0.5 ms, 10% baseline). Failure is nonzero; there is no outlier trimming.

Use a fresh empty HOME and workspace, an allowlisted environment, and OpenAI
provider selection with no credentials or submitted input. This provider does
not pre-warm. Detect the exact bold-green composer `> ` plus reset bytes in
`tui_draw.c`, including across reads, not the welcome banner. Start the clock
immediately before Popen and stop on observed prompt bytes (CLI: process exit).
The 80x24 PTY measurement includes spawning and terminal transport; it is not
Enter-to-first-token or a physical-display latency measurement. No backend
request is initiated. A timeout or missing prompt is a harness error, not a
latency observation. Existing mock TTFT benchmarks remain separate gates.

A fresh forked measurement worker resets RUSAGE_CHILDREN for each launch;
record ru_maxrss in bytes (Darwin bytes, Linux KiB converted). Worker creation
is outside the timer. This is process peak RSS through exit/forced termination,
not idle RSS at prompt, an allocation count, or a subtractable cumulative RSS
counter. No memory budget conclusion follows from this proxy alone.

## Evidence and consequences

`tests/bench/bench_startup.py` emits JSON and Markdown, supports two-result
comparison, and reports dependencies with otool/ldd. Record compiler, flags,
revision, host and binary hash with baseline evidence. Same-binary observations
check harness noise before evaluating migration candidates. Reports do not
replace Linux/Windows/wasm evidence or parser/queue throughput/resource checks.
The benchmark needs POSIX fork/PTY support; native Windows timing requires a
separate harness. Routine make test exercises deterministic thresholds and
real child/PTY behavior without enforcing noisy wall-clock budgets in CI.
