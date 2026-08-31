# tnytty performance and agent-readiness benchmark

`bench_tnytty.py` emits one machine-readable JSON report. It uses private
temporary HOME, config, runtime, broker-socket, and state paths; fixed system
commands; and a controlled broker process. It never connects to the user's
live broker.

```sh
make -C tnytty benchmark
```

`make benchmark` builds the release binary and the benchmark-only broker
entry point, then enforces the product budgets: mean `--help` and `--version`
below 5 ms, p95 PTY first byte below 15 ms, and a stripped binary below 500
KiB. Help and version always run 100 samples. `BENCH_ARGS` passes additional
arguments to the runner:

```sh
make -C tnytty benchmark \
  BENCH_ARGS='--baseline-binary /tmp/tnytty-before/build/tnytty \
  --output /tmp/tnytty-benchmark.json'
```

The report also measures full process lifetime, broker cold readiness,
session creation, snapshot polling, input-to-visible-screen latency, hot
reattach, same-child-PID survival after detach, and health latency while
`/usr/bin/yes` continuously fills another PTY. The broker sample count
defaults to 20; PTY startup defaults to 50. Increase either without changing
the report schema:

```sh
python3 tnytty/tests/bench/bench_tnytty.py \
  --binary tnytty/build/tnytty \
  --broker-helper tnytty/build/tnytty-bench-broker \
  --runs 100 --broker-runs 100 --enforce
```

## 2026-08-30 baseline and durable-session candidate

Apple arm64, Darwin 27.0.0, Apple clang 21, Python 3.14.7. Both binaries were
built sequentially from clean `git archive` exports with `-Os`; the baseline
is pre-change main `bfb7a1e`, and the candidate is `d04d55a`.

| Metric | `bfb7a1e` | `d04d55a` |
| --- | ---: | ---: |
| stripped binary | 240,160 B | 274,848 B |
| help mean, n=100 | 2.933 ms | 2.996 ms |
| version mean, n=100 | 2.923 ms | 2.882 ms |
| PTY first-byte p50, n=50 | 8.281 ms | 8.314 ms |
| PTY first-byte p95, n=50 | 10.105 ms | 8.941 ms |
| full-process p50, n=50 | 9.492 ms | 9.142 ms |
| full-process p95, n=50 | 20.457 ms | 20.002 ms |

Candidate-only broker results:

| Metric | p50 | p95 |
| --- | ---: | ---: |
| cold ready, n=20 | 4.141 ms | 4.700 ms |
| session create, n=20 | 1.781 ms | 5.509 ms |
| 115,256-byte snapshot poll, n=20 | 0.693 ms | 1.023 ms |
| input to visible screen, n=20 | 0.207 ms | 0.395 ms |
| hot attach plus snapshot, n=20 | 0.197 ms | 0.219 ms |
| health under continuous `yes`, n=50 | 2.478 ms | 2.614 ms |

Detach completed in 0.064 ms and both the OS process check and the restored
screen proved that the exact child PID survived. All 50 fairness requests
succeeded.

The durable broker, tabs, workspace state, canonical snapshots, and metadata
added 34,688 bytes (14.4 percent) while retaining 237,152 bytes of headroom
under the 500 KiB limit. The measured local snapshot/reattach path is already
sub-millisecond at p50, so connection pooling is not justified by these
results. A future optimization should first extend this harness to many idle
panes and show that unchanged full-snapshot serialization or per-request Unix
connections materially consume the 8 ms GUI polling budget.

## Final integrated candidate

The final worktree additionally includes the broker-owned public listener,
red-close lifecycle hardening, dynamic broker poll capacity, and the public
AppKit glass backdrop. A fresh enforced run on the same machine produced:

| Metric | Final candidate |
| --- | ---: |
| stripped binary | 291,440 B |
| help mean, n=100 | 3.019 ms |
| version mean, n=100 | 2.817 ms |
| PTY first-byte p50 / p95, n=50 | 8.420 / 10.231 ms |
| full-process p50 / p95, n=50 | 9.487 / 21.503 ms |
| broker cold-ready p50 / p95, n=20 | 6.694 / 9.125 ms |
| session create p50 / p95, n=20 | 4.839 / 6.957 ms |
| 115,256-byte snapshot p50 / p95, n=20 | 0.685 / 1.806 ms |
| input-to-screen p50 / p95, n=20 | 0.202 / 0.341 ms |
| hot reattach p50 / p95, n=20 | 0.780 / 1.127 ms |
| health under continuous `yes` p50 / p95, n=50 | 2.350 / 2.491 ms |

Detach completed in 0.059 ms and retained the exact child PID. Relative to
`bfb7a1e`, the complete feature set adds 51,280 bytes (21.4 percent) and leaves
220,560 bytes below the 500 KiB limit. The JSON report is produced by the
command above; this run was saved as `/private/tmp/tnytty-benchmark-goal-final.json`.
